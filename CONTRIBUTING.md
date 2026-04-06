# Contributing to libmembus

## Architecture

```
membus.hpp (single public header)

Namespace: membus
  create(name, size)       Create bus SHM segment (O_CREAT|O_EXCL)
  destroy(name)            Unlink SHM segment

Class: Bus (pimpl)
  Bus(name)                Open + register reader slot (RAII)
  write(data, len)         Write bytes (lossy, never blocks)
  read(buf, len)           Non-blocking read
  read_wait(buf, len, ms)  Blocking read with futex
  set_reader_name(name)    Debug label
  name()                   Bus name
```

## Source Files

| File | Purpose |
|------|---------|
| membus.cpp | Full implementation: SHM layout, circular buffer, robust mutexes, futex |

## Canonical Spec

[`docs/MEMBUS_SPEC.md`](docs/MEMBUS_SPEC.md) is the specification.
All implementation decisions defer to this document.

## Build

```bash
make              # Build libmembus.a + libmembus.so
make test         # Build and run all 45 tests
make install      # Install library + header to /usr/local (sudo)
make uninstall    # Remove installed files (sudo)
make clean        # Remove build artifacts
```

## Coding Standards

- **C++17**, compiled with `-Wall -Wextra -Werror -O2 -fPIC`
- **No external dependencies** — libc, librt, libpthread only
- **Linux only** — futex, POSIX SHM
- **Single public header**: `membus.hpp`
- Errors: `std::system_error` for create/open failures, return 0 for empty reads
- No commented-out code, no bare TODOs, no debug prints
- No routing, framing, or protocol logic — bytes only
- Lossy by design — slow readers are advanced, never block writers

## Adding Tests

Tests use a lightweight custom framework (`tests/test_common.hpp`):

```cpp
static void test_my_feature() {
    membus::create("test_bus", 4096);
    membus::Bus bus("test_bus");
    // ... test logic ...
    membus::destroy("test_bus");
}
```

Register in the suite runner function (`test_membus_run`) and it will be
included in `make test`.

## Test Suite

45 tests covering: lifecycle, multi-reader, dead-slot reclamation, crash recovery,
circular buffer wrap-around, binary data integrity, blocking reads, move semantics,
error paths, cross-process IPC.

## Before Submitting

- Run `make clean && make test` — all 45 tests must pass
- New public API requires documentation in `membus.hpp` header comments
