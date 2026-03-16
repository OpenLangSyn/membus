# Membus — Claude Code Context

## What is this?
C++17 POSIX shared memory IPC library. Circular ring buffer with
multi-reader broadcast. Completely dumb — no routing, no parsing, no
protocol logic, no message framing. Bytes in, bytes out.

Proprietary. No Antheos, Verus, or trace dependencies.

## Canonical spec
`docs/MEMBUS_SPEC.md` — the spec. All implementation decisions defer to this document.

## Build
```bash
make              # Build libmembus.a
make test         # Build and run all 19 tests
make install      # Install library + header to /usr/local (sudo)
make uninstall    # Remove installed files (sudo)
make clean        # Remove build artifacts
```

## Directory structure
- `include/membus.hpp` — Single public header
- `src/membus.cpp` — Full implementation (pimpl)
- `tests/` — Test suite (45 tests + test_common.hpp)
- `docs/` — Spec, baseline

## API
```
membus::create(name, size)         Create bus (O_CREAT|O_EXCL)
membus::destroy(name)              Unlink SHM segment
membus::Bus(name)                  Open + register reader slot (RAII)
  .write(data, len)                Write bytes (lossy, never blocks)
  .read(buf, len)                  Non-blocking read
  .read_wait(buf, len, timeout)    Blocking read with futex
  .set_reader_name(name)           Debug label
  .name()                          Bus name
```

## Coding standards
- C++17, compiled with `-Wall -Wextra -Werror -O2 -fPIC`
- No external dependencies (libc, librt, libpthread)
- Single public header: `membus.hpp`
- Errors: `std::system_error` for create/open failures, return 0 for empty reads
- Linux only (futex, POSIX SHM)

## Rules for modifications
- Run `make clean && make test` before every commit — all 45 tests must pass
- No routing, framing, or protocol logic — bytes only
- Lossy by design — slow readers are advanced, never block writers
- No commented-out code, no bare TODOs, no debug prints
