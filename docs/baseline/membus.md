# libmembus C++17

Updated: 2026-03-16

## What It Is

C++17 POSIX shared memory IPC library. Circular ring buffer with multi-reader
broadcast. Completely dumb — no routing, no parsing, no protocol logic, no
message framing. Bytes in, bytes out. Antheos runs on top of it.

## Location

- Source: `~/projects/langsyn/base/membus/`
- Header: `include/membus.hpp` (single public header)
- Implementation: `src/membus.cpp`
- Tests: `tests/test_membus.cpp` (19 tests + test_common.hpp)
- Spec (canon): `docs/MEMBUS_SPEC.md`
- Compiled: `libmembus.a` (static library)

## Current State

### API

| Function | Purpose |
|----------|---------|
| `membus::create(name, size)` | Create bus SHM segment (O_CREAT\|O_EXCL) |
| `membus::destroy(name)` | Unlink SHM segment |
| `Bus(name)` | Open + register reader slot (RAII) |
| `Bus::write(data, len)` | Write bytes. Lossy — slow readers advanced. |
| `Bus::read(buf, len)` | Non-blocking read. Returns 0 if empty. |
| `Bus::read_wait(buf, len, ms)` | Blocking read with futex. |
| `Bus::set_reader_name(name)` | Set debug label for reader slot. |
| `Bus::name()` | Bus name this handle is attached to. |

### SHM Layout (Version 3)

```
ShmBuffer header:
  magic (uint32_t, 0x4D425553)
  version (uint32_t, 3)
  size (size_t, buffer capacity)
  write_pos (size_t, single writer cursor)
  reader_count (uint32_t, active readers)
  write_lock (pthread_mutex_t, PROCESS_SHARED + ROBUST)
  reader_lock (pthread_mutex_t, PROCESS_SHARED + ROBUST)
  write_seq (uint32_t, futex wakeup counter)
  readers[32] (ReaderSlot: read_pos, pid, name[32])
  data[] (circular buffer)
```

### Error Model

- `create()` / `Bus()` constructor throw `std::system_error` on failure
- `destroy()` throws `std::system_error` on failure
- `write()` always succeeds (returns len)
- `read()` returns 0 if nothing available
- `read_wait()` returns 0 on timeout

### Synchronization

- Robust mutexes (PTHREAD_PROCESS_SHARED + PTHREAD_MUTEX_ROBUST)
- EOWNERDEAD recovery via pthread_mutex_consistent()
- Futex-based blocking reads (write_seq atomic counter)
- Dead process slot reclamation on open and write

### Constants

| Constant | Value |
|----------|-------|
| DEFAULT_SIZE | 65536 (64 KB) |
| MAX_NAME | 256 |
| MAX_READERS | 32 |
| READER_NAME_LEN | 32 |
| MAGIC | 0x4D425553 |
| VERSION | 3 |

### Build

```bash
make              # Build libmembus.a
make test         # Run all 19 tests
make install      # Install to /usr/local/lib + /usr/local/include/membus/
# Flags: -std=c++17 -Wall -Wextra -Werror -O2 -fPIC
# Links: -lrt -lpthread
```

### Test Suite

| Test | What |
|------|------|
| test_basic | create/open/write/read/close |
| test_slot_reuse | close/open cycles don't leak slots |
| test_multi_handle_same_pid | same PID, different slots, independent reads |
| test_different_pids | fork, verify separate slots |
| test_dead_reclaim | dead process slots reclaimed |
| test_slot_exhaustion | fill 32 slots, get ENOSPC, reclaim on kill |
| test_no_write_block | close/reopen doesn't block write |
| test_write_skips_dead | write not blocked by dead slots |
| test_slow_reader_advanced | slow reader silently advanced |
| test_magic_validation | magic/version check on open |
| test_no_semaphore_files | no sem.* files in /dev/shm |
| test_read_wait_basic | returns immediately if data available |
| test_read_wait_blocks | blocks until write via futex |
| test_read_wait_timeout | respects timeout_ms |
| test_robust_writer_death | recovers EOWNERDEAD on writer death |
| test_robust_reader_death | recovers EOWNERDEAD on reader death |
| test_move_semantics | Bus move construct/assign |
| test_create_throws_on_exists | std::system_error on duplicate create |
| test_open_throws_on_missing | std::system_error on missing bus |

## Known Issues

- 32-reader hard limit per bus (compile-time constant)
- Linux only (futex is Linux-specific)
- write_seq is 32-bit (wraps after ~4 billion writes, harmless for futex)
- No message framing — consumer handles boundaries
