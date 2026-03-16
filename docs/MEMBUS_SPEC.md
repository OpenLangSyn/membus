# MemBus Specification v2.0.0

**Author:** Are Bjorby <are.bjorby@proton.me>
**Date:** 2026-03-16
**Language:** C++17
**License:** Proprietary

---

## 1. Overview

MemBus is a POSIX shared memory IPC library. It provides a circular ring buffer
with multi-reader broadcast over `/dev/shm`. Completely dumb — no routing,
no parsing, no protocol logic, no message framing. Bytes in, bytes out. Higher
layers handle protocol semantics.

**Design principles:**
- Completely dumb. Raw byte transport only.
- Single writer position, multiple independent reader cursors.
- Lossy for slow readers — writers are never blocked.
- Crash-resilient — robust mutexes, dead-slot reclamation.
- Zero-poll blocking reads via Linux futex.
- No global state. Each bus is an independent shared memory segment.

---

## 2. Architecture

### 2.1 Shared Memory Segment

Each bus maps to one POSIX shared memory object at `/dev/shm/<name>_shm`.
The segment contains a fixed header (`ShmBuffer`) followed by a
variable-length circular data buffer.

```
┌────────────────────────────────────────────────┐
│ ShmBuffer header                               │
│   magic, version, size, write_pos              │
│   reader_count                                 │
│   write_lock (robust mutex, process-shared)    │
│   reader_lock (robust mutex, process-shared)   │
│   write_seq (atomic uint32, futex counter)     │
│   readers[32] (per-reader cursor slots)        │
├────────────────────────────────────────────────┤
│ data[] (circular buffer, `size` bytes)         │
└────────────────────────────────────────────────┘
```

Total segment size: `sizeof(ShmBuffer) + size`.

### 2.2 Bus Handle

Each `Bus` constructor call creates a process-local RAII handle via pimpl
(`Bus::Impl`). The handle holds the mmap pointer, file descriptor, and
the index of this handle's reader slot in shared memory. The handle is
heap-allocated internally and freed by the `Bus` destructor.

### 2.3 SHM Naming

Bus name `"my_bus"` maps to POSIX SHM object `"/my_bus_shm"`.
The `_shm` suffix is appended internally. Bus names must not be empty
and must fit within `MAX_NAME` (256 bytes) including the suffix.

### 2.4 Permissions

Shared memory objects are created with mode `0660`. The consuming application
controls ownership via `umask` at creation time.

---

## 3. Constants

All constants live in namespace `membus`.

| Constant | Value | Description |
|----------|-------|-------------|
| DEFAULT_SIZE | 65536 (64 KB) | Default buffer capacity when size=0 |
| MAX_NAME | 256 | Maximum bus name length (bytes) |
| MAX_READERS | 32 | Maximum concurrent reader slots per bus |
| READER_NAME_LEN | 32 | Maximum reader debug label length |
| MAGIC | 0x4D425553 | Header magic ("MBUS" in ASCII) |
| VERSION | 3 | Shared memory layout version |

---

## 4. Data Structures

All structures are internal to the implementation (not exposed in the
public header). Documented here for specification completeness.

### 4.1 ReaderSlot

Per-reader cursor stored in shared memory. 32 slots per bus.

| Field | Type | Description |
|-------|------|-------------|
| read_pos | size_t | This reader's current position in the circular buffer |
| pid | pid_t | Owner process ID (0 = slot is free) |
| name | char[32] | Debug label (human-readable, optional) |

### 4.2 ShmBuffer

Shared memory header. Lives at offset 0 of the mmap'd segment. The flexible
array member `data[]` follows immediately after the struct.

| Field | Type | Description |
|-------|------|-------------|
| magic | uint32_t | Must equal MAGIC |
| version | uint32_t | Must equal VERSION |
| size | size_t | Circular buffer capacity (bytes) |
| write_pos | size_t | Current write offset into data[] |
| reader_count | uint32_t | Number of active reader slots |
| write_lock | pthread_mutex_t | Process-shared robust mutex for write serialization |
| reader_lock | pthread_mutex_t | Process-shared robust mutex for reader slot management |
| write_seq | uint32_t | Atomic futex counter, incremented on each write |
| readers | ReaderSlot[32] | Reader slot array |
| data | char[] | Circular buffer (flexible array member) |

### 4.3 Bus::Impl

Per-process handle, heap-allocated via `std::unique_ptr`. Not in
shared memory. Hidden behind pimpl.

| Field | Type | Description |
|-------|------|-------------|
| name | std::string | Bus name |
| shm_fd | int | File descriptor for the SHM object |
| buffer | ShmBuffer* | Pointer to mmap'd shared memory |
| map_size | size_t | Total mmap size |
| reader_slot | int | Index into buffer->readers (-1 = not registered) |

---

## 5. Synchronization

### 5.1 Mutexes

Both mutexes are initialized with:
- `PTHREAD_PROCESS_SHARED` — usable across processes via shared memory.
- `PTHREAD_MUTEX_ROBUST` — recoverable after holder death.

**Robust lock protocol:** Every lock acquisition goes through `robust_lock()`,
which handles `EOWNERDEAD` by calling `pthread_mutex_consistent()` and
continuing. `ENOTRECOVERABLE` is treated as a fatal error (throws
`std::system_error` with `EIO`).

### 5.2 Write Lock

Held during `Bus::write()`. Serializes all writes to the bus. One writer
at a time. Released after the write and futex wake.

### 5.3 Reader Lock

Held during reader slot registration (`Bus` constructor), deregistration
(`Bus` destructor), reads (`Bus::read`), and label setting
(`Bus::set_reader_name`).

### 5.4 Futex

`write_seq` is an atomic `uint32_t` used as a Linux futex. After each write:

1. Writer atomically increments `write_seq` (`__ATOMIC_RELEASE`).
2. Writer calls `FUTEX_WAKE` with `INT_MAX` (wake all waiters).

Readers in `Bus::read_wait()`:

1. Read `write_seq` with `__ATOMIC_ACQUIRE`.
2. Call `FUTEX_WAIT` with the observed sequence value and optional timeout.
3. On wake (or timeout), perform a normal `Bus::read()`.

This eliminates polling. Readers sleep in the kernel until a write occurs.

---

## 6. Circular Buffer

### 6.1 Write Semantics

Data is written at `write_pos`, wrapping around when it reaches `size`.
If the write would exceed `end_space` (remaining bytes before wrap), it
is split: first part fills to the end, remainder wraps to offset 0.

After the write, `write_pos` advances by `len`.

### 6.2 Read Semantics

Each reader has an independent `read_pos` in its slot. A read copies
`min(len, available)` bytes from `read_pos`, handling wrap-around
identically to writes. Returns 0 if no data is available (non-blocking).

Available bytes: `(write_pos - read_pos + size) % size`.

### 6.3 Slow Reader Policy

**Lossy, not blocking.** If a write of `len` bytes would be blocked by a
slow reader (insufficient space between `write_pos` and the slowest
`read_pos`), the writer advances that reader's `read_pos` to `write_pos`.
The reader loses unread data. The write always succeeds.

This prevents any single unpumped reader from stalling the bus.

### 6.4 Buffer Capacity

Usable capacity is `size - 1` bytes (one byte reserved to distinguish
full from empty). A write of exactly `size` bytes will always trigger
slow reader advancement.

---

## 7. Reader Slot Management

### 7.1 Registration (Bus constructor)

1. Lock `reader_lock`.
2. **Dead slot reclamation:** Scan all 32 slots. For each slot with
   `pid != 0`, call `kill(pid, 0)`. If `ESRCH` (process dead), clear
   the slot (`pid = 0`, decrement `reader_count`).
3. **Slot allocation:** Find first slot with `pid == 0`. If none found,
   throw `std::system_error(ENOSPC)`.
4. Set `pid = getpid()`, `read_pos = write_pos` (new readers see only
   future data), increment `reader_count`.
5. Unlock `reader_lock`.

### 7.2 Deregistration (Bus destructor)

1. Lock `reader_lock`.
2. Clear slot: `pid = 0`, clear name, decrement `reader_count`.
3. Unlock `reader_lock`.
4. `munmap`, `close(shm_fd)`.

### 7.3 Same-PID Multiple Handles

A single process may open multiple handles to the same bus. Each
`Bus` constructor allocates a separate slot. This is the intended pattern
for services that both read and write on the same bus.

### 7.4 Dead Process Recovery

Dead-slot reclamation occurs at two points:
- **On open:** Proactive scan of all slots (Section 7.1).
- **On write:** Per-slot `kill(pid, 0)` check. Dead slots are cleared
  inline and excluded from available-space calculations.

This means a bus can never be permanently stuck due to crashed processes.

---

## 8. API Reference

All public API lives in namespace `membus`. The header is `membus.hpp`.

### 8.1 membus::create

```cpp
void membus::create(std::string_view name, size_t size = DEFAULT_SIZE);
```

Create a new shared memory bus. Uses `O_CREAT | O_EXCL` — fails if the
bus already exists.

| Parameter | Description |
|-----------|-------------|
| name | Bus name (non-empty, max 256 chars) |
| size | Buffer capacity in bytes (0 = 64 KB default) |

**Throws:** `std::system_error` on failure (`EEXIST` if bus exists,
`EINVAL` if name is empty).

**Behavior:**
1. Open SHM object with `O_CREAT | O_EXCL | O_RDWR`, mode 0660.
2. `ftruncate` to `sizeof(ShmBuffer) + size`.
3. `mmap` the segment, zero-initialize header, set magic/version/size.
4. Initialize both robust mutexes.
5. `munmap` and `close` — creator does not retain a handle.

### 8.2 membus::destroy

```cpp
void membus::destroy(std::string_view name);
```

Remove the shared memory object. Does not close any open handles — callers
must close first. Silently succeeds if already gone (`ENOENT` suppressed).

**Throws:** `std::system_error` on failure (`EINVAL` if name is empty).

### 8.3 Bus::Bus (constructor)

```cpp
explicit membus::Bus(std::string_view name);
```

Attach to an existing bus and register a reader slot. RAII — destructor
releases the reader slot.

| Parameter | Description |
|-----------|-------------|
| name | Bus name (must already exist) |

**Throws:** `std::system_error` on failure (`ENOENT` if bus does not exist,
`ENOSPC` if all 32 reader slots are occupied by live processes, `EINVAL`
if name is empty or magic is bad).

**Behavior:** Opens SHM, validates magic, reclaims dead slots, allocates
a reader slot with `read_pos = write_pos` (new readers see only future data).

Move constructible and move assignable. Copy disabled.

### 8.4 Bus::write

```cpp
size_t Bus::write(const uint8_t* data, size_t len);
```

Write data to the bus. Advances slow live readers if necessary (lossy).

| Parameter | Description |
|-----------|-------------|
| data | Source buffer |
| len | Number of bytes to write |

**Returns:** `len` on success. Returns 0 if `data` is null or `len` is 0.

**Behavior:**
1. Lock `write_lock`.
2. Compute available space (skip dead slots).
3. If `len > available`: advance slow readers' `read_pos` to `write_pos`.
4. Copy data to circular buffer (split at wrap boundary if needed).
5. Update `write_pos`.
6. Atomic increment `write_seq`, unlock, `FUTEX_WAKE` all waiters.

### 8.5 Bus::read

```cpp
size_t Bus::read(uint8_t* buf, size_t len);
```

Non-blocking read from this handle's private cursor.

| Parameter | Description |
|-----------|-------------|
| buf | Destination buffer |
| len | Maximum bytes to read |

**Returns:** Bytes read (may be less than `len`), 0 if no data available or
if `buf` is null / `len` is 0.

### 8.6 Bus::read_wait

```cpp
size_t Bus::read_wait(uint8_t* buf, size_t len, int timeout_ms = -1);
```

Blocking read with futex-based wait.

| Parameter | Description |
|-----------|-------------|
| buf | Destination buffer |
| len | Maximum bytes to read |
| timeout_ms | -1 = block forever, 0 = poll (same as read), >0 = timeout in ms |

**Returns:** Bytes read, 0 on timeout with no data.

**Behavior:**
1. Try `read()`. If data available, return immediately.
2. If `timeout_ms == 0`, return 0.
3. Load `write_seq` (atomic acquire).
4. `FUTEX_WAIT` on `write_seq` with optional timeout.
5. Try `read()` again after wake.

### 8.7 Bus::set_reader_name

```cpp
void Bus::set_reader_name(std::string_view reader_name);
```

Set a human-readable debug label for this handle's reader slot. Truncated
to `READER_NAME_LEN - 1` (31 chars).

### 8.8 Bus::name

```cpp
std::string_view Bus::name() const;
```

Returns the bus name this handle is attached to.

---

## 9. Error Handling

Functions that can fail throw `std::system_error` with the appropriate
system error code. `write()`, `read()`, and `read_wait()` return 0
for edge cases (null pointers, zero length, no data, timeout).

| Error code | Condition |
|------------|-----------|
| EINVAL | Empty name, bad magic |
| ENOSPC | All 32 reader slots occupied by live processes |
| EEXIST | Bus already exists (create with O_EXCL) |
| ENOENT | Bus does not exist (Bus constructor) |
| EIO | Mutex is ENOTRECOVERABLE (unrecoverable corruption) |

---

## 10. Platform Requirements

- **OS:** Linux (futex is Linux-specific via `SYS_futex`)
- **Standard:** C++17 (`-std=c++17`)
- **POSIX:** `shm_open`, `mmap`, `ftruncate`, `pthread` robust mutexes
- **Libraries:** `-lrt` (POSIX realtime), `-lpthread`
- **Kernel:** futex support (any Linux >= 2.6)

---

## 11. Build

```bash
make              # Build libmembus.a
make test         # Build + run 19 unit tests
sudo make install # Install to /usr/local/{lib,include/membus}
make clean        # Remove build artifacts
```

**Compiler flags:** `-std=c++17 -Wall -Wextra -Werror -O2 -fPIC`
**Link flags:** `-lrt -lpthread`
**Output:** `libmembus.a` (static library)
**Install targets:** `/usr/local/lib/libmembus.a`, `/usr/local/include/membus/membus.hpp`

---

## 12. Limitations

- **Linux only.** Futex syscall is not portable to other POSIX systems.
- **No message framing.** Raw byte stream — the consumer must handle
  message boundaries (the consuming protocol layer provides this).
- **32-reader hard limit.** Compile-time constant. Exceeding it requires
  a recompile.
- **Lossy by design.** Slow readers silently lose data. This is an
  architectural choice, not a bug.
- **No persistence.** All data lives in RAM. Lost on reboot.
- **Single-segment bus.** No multi-segment chaining or dynamic resize.
- **write_seq is 32-bit.** Wraps after ~4 billion writes. Harmless for
  futex semantics (only equality is checked).
