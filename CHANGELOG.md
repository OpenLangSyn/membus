# Changelog

## v2.1.0 — 2026-04-03

Open-source release under MIT license.

- MIT license (was proprietary)
- SPDX-License-Identifier headers in source files
- CONTRIBUTING.md (was CLAUDE.md)
- 45 tests across full API surface

## v2.0.0 — 2026-03-16

Full C++17 rewrite. Native `membus::Bus` class replaces C11 API.

### Changed

- **Language**: C11 → C++17. Single public header `membus.hpp`.
- **API**: RAII `Bus` class, `std::system_error` exceptions, pimpl.
- **Build output**: `libmembus.a` (static) replaces `libmembus.so` (shared).
- **Tests**: 19 tests (16 ported from C11 + 3 C++ specific).

### Preserved

- SHM layout (version 3) — binary compatible with v1.x.
- All v1.1.0 features: robust mutexes, futex, dead-slot reclamation, lossy writes.

---

## v1.1.0 — 2026-03-16

Complete rewrite of synchronization and buffer management (v3 architecture).

### Changed

- **Robust mutexes** replace semaphores — `PTHREAD_PROCESS_SHARED` +
  `PTHREAD_MUTEX_ROBUST` embedded in shared memory. Handles crashed process
  recovery via `EOWNERDEAD` → `pthread_mutex_consistent()`.
- **Futex wakeup** replaces polling — `write_seq` atomic counter with
  `FUTEX_WAKE` broadcast. New `membus_read_wait()` for efficient blocking reads.
- **Slow reader advance** — `membus_write()` advances slow readers instead of
  returning `EAGAIN`. Bus is lossy for unpumped handles, never blocking.
- **Dead-slot reclamation** — `membus_open()` proactively reclaims dead process
  reader slots via `kill(pid, 0)`. Also checked inline during writes.
- **Magic + version validation** — header contains `0x4D425553` ("MBUS") +
  version 3, validated on every `membus_open()`.
- **Shared library output** — builds as `libmembus.so` instead of static binaries.
- **Simplified Makefile** — library + test only; no server/writer/reader binaries.

### Fixed

- **MEMBUS-01:** Reader slot leak — slots not released on close.
- **MEMBUS-02:** Slow live readers block writes — writer returned `EAGAIN` instead
  of advancing the slow reader.
- **BOOT-01:** Stale shared memory after hard reboot — semaphore files left in
  `/dev/shm`. Eliminated by moving to embedded robust mutexes (no external
  semaphore files).

### Removed

- `membus_server` binary (bus creation is now just `membus_create()`).
- `membus_writer` / `membus_reader` test binaries (replaced by unit tests).
- Example programs (echo server, log aggregator, pubsub, benchmark).
- Named semaphore files — synchronization is fully embedded in shared memory.

## v1.0.0 — 2025-12-01

Initial release. Semaphore-based synchronization, server/writer/reader binaries.
