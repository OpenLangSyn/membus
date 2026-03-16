# libmembus

C++17 POSIX shared memory IPC library.

Circular ring buffer with multi-reader broadcast. Bytes in, bytes out.
No routing, parsing, or protocol logic.

## Build

```bash
make              # Build libmembus.a
make test         # Build and run all 19 tests
make install      # Install to /usr/local (sudo required)
make clean        # Remove build artifacts
```

Requires: `g++` with C++17 support. Linux only (futex, POSIX SHM).

## API

Single public header: `#include <membus/membus.hpp>`

```cpp
membus::create("bus_name", 65536);     // Create bus
membus::Bus bus("bus_name");           // Open + register reader
bus.write(data, len);                  // Write (lossy, never blocks)
bus.read(buf, len);                    // Non-blocking read
bus.read_wait(buf, len, timeout_ms);   // Blocking read (futex)
// ~Bus() cleans up reader slot (RAII)
membus::destroy("bus_name");           // Unlink SHM
```

## Design

- **Lossy**: Writers never block. Slow readers are silently advanced.
- **Multi-reader**: Up to 32 concurrent readers per bus, each with independent cursor.
- **Crash-safe**: Robust mutexes recover from process death. Dead slots reclaimed.
- **Zero-poll**: Blocking reads use Linux futex (no polling).
- **SHM naming**: Bus `"foo"` maps to `/dev/shm/foo_shm`
- **Buffer**: Circular, wraps at boundaries. Usable capacity = `size - 1` bytes.

See `docs/MEMBUS_SPEC.md` for the full specification.

## License

Copyright (c) 2025-2026 Are Bjorby. All rights reserved. See `LICENSE`.
