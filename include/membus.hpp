/*
 * membus.hpp — POSIX Shared Memory IPC
 *
 * Circular ring buffer with multi-reader broadcast.
 * Bytes in, bytes out. No routing, parsing, or protocol logic.
 *
 * Copyright (c) 2025-2026 Are Bjørby <are.bjorby@langsyn.org>
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace membus {

inline constexpr size_t   DEFAULT_SIZE    = 65536;   // 64 KB
inline constexpr size_t   MAX_NAME        = 256;
inline constexpr int      MAX_READERS     = 32;
inline constexpr size_t   READER_NAME_LEN = 32;
inline constexpr uint32_t MAGIC           = 0x4D425553; // "MBUS"
inline constexpr uint32_t VERSION         = 3;

/* Create a new bus (O_CREAT|O_EXCL). Throws std::system_error on failure. */
void create(std::string_view name, size_t size = DEFAULT_SIZE);

/* Unlink the SHM segment. Throws std::system_error on failure. */
void destroy(std::string_view name);

class Bus {
public:
    /* Open + register a reader slot. Throws std::system_error on failure. */
    explicit Bus(std::string_view name);
    ~Bus();

    Bus(Bus&&) noexcept;
    Bus& operator=(Bus&&) noexcept;
    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;

    /* Write bytes. Always returns len. Slow readers silently advanced. */
    size_t write(const uint8_t* data, size_t len);

    /* Non-blocking read. Returns bytes read, 0 if nothing available. */
    size_t read(uint8_t* buf, size_t len);

    /* Blocking read with futex. Returns bytes read, 0 on timeout.
       timeout_ms: -1 = block forever, 0 = poll, >0 = timeout in ms. */
    size_t read_wait(uint8_t* buf, size_t len, int timeout_ms = -1);

    /* Set debug label for this reader slot. */
    void set_reader_name(std::string_view reader_name);

    /* Bus name this handle is attached to. */
    std::string_view name() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace membus
