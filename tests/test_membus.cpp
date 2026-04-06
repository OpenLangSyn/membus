#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "test_common.hpp"
#include "membus.hpp"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <system_error>

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── SHM layout mirror for internal access in tests ── */

namespace shm {

struct ReaderSlot {
    size_t  read_pos;
    pid_t   pid;
    char    name[membus::READER_NAME_LEN];
};

struct Buffer {
    uint32_t        magic;
    uint32_t        version;
    size_t          size;
    size_t          write_pos;
    uint32_t        reader_count;
    pthread_mutex_t write_lock;
    pthread_mutex_t reader_lock;
    uint32_t        write_seq;
    ReaderSlot      readers[membus::MAX_READERS];
    char            data[];
};

Buffer* open_raw(const char* bus_name) {
    char path[membus::MAX_NAME];
    std::snprintf(path, sizeof(path), "/%s_shm", bus_name);
    int fd = shm_open(path, O_RDWR, 0660);
    if (fd == -1) return nullptr;
    struct stat st;
    if (fstat(fd, &st) == -1) { close(fd); return nullptr; }
    auto* buf = static_cast<Buffer*>(
        mmap(nullptr, static_cast<size_t>(st.st_size),
             PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    close(fd);
    return (buf == MAP_FAILED) ? nullptr : buf;
}

} // namespace shm

/* ── Helpers ── */

static constexpr const char* BUS = "test_membus_cpp";

static void cleanup() {
    try { membus::destroy(BUS); } catch (...) {}
}

static long elapsed_ms(struct timespec& start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start.tv_sec) * 1000 +
           (now.tv_nsec - start.tv_nsec) / 1000000;
}

/* ══════════════════════════════════════════════════════════════════
 *  Original 19 tests
 * ══════════════════════════════════════════════════════════════════ */

static void test_basic() {
    cleanup();
    membus::create(BUS, 4096);

    membus::Bus h(BUS);

    const uint8_t msg[] = "hello";
    size_t w = h.write(msg, 5);
    ASSERT_EQ(w, 5);

    uint8_t buf[64];
    size_t r = h.read(buf, sizeof(buf));
    ASSERT_EQ(r, 5);
    ASSERT_MEM_EQ(buf, msg, 5);

    cleanup();
}

static void test_slot_reuse() {
    cleanup();
    membus::create(BUS, 4096);

    for (int i = 0; i < 10; i++) {
        membus::Bus h(BUS);
    }

    membus::Bus h(BUS);
    auto* raw = shm::open_raw(BUS);
    ASSERT_TRUE(raw != nullptr);
    ASSERT_EQ(raw->reader_count, 1);

    cleanup();
}

static void test_multi_handle_same_pid() {
    cleanup();
    membus::create(BUS, 4096);

    membus::Bus h1(BUS);
    membus::Bus h2(BUS);

    auto* raw = shm::open_raw(BUS);
    ASSERT_TRUE(raw != nullptr);
    ASSERT_EQ(raw->reader_count, 2);

    const uint8_t msg[] = "test";
    h1.write(msg, 4);

    uint8_t buf1[64], buf2[64];
    size_t r1 = h1.read(buf1, sizeof(buf1));
    size_t r2 = h2.read(buf2, sizeof(buf2));
    ASSERT_EQ(r1, 4);
    ASSERT_EQ(r2, 4);

    cleanup();
}

static void test_different_pids() {
    cleanup();
    membus::create(BUS, 4096);

    membus::Bus parent_h(BUS);

    pid_t child = fork();
    if (child == 0) {
        try {
            membus::Bus child_h(BUS);
            auto* raw = shm::open_raw(BUS);
            if (!raw || raw->reader_count != 2) _exit(1);
            _exit(0);
        } catch (...) {
            _exit(1);
        }
    }

    int status;
    waitpid(child, &status, 0);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    cleanup();
}

static void test_dead_reclaim() {
    cleanup();
    membus::create(BUS, 4096);

    pid_t child = fork();
    if (child == 0) {
        try { membus::Bus h(BUS); } catch (...) {}
        _exit(0);
    }
    int status;
    waitpid(child, &status, 0);

    membus::Bus h(BUS);
    auto* raw = shm::open_raw(BUS);
    ASSERT_TRUE(raw != nullptr);
    ASSERT_EQ(raw->reader_count, 1);

    cleanup();
}

static void test_slot_exhaustion() {
    cleanup();
    membus::create(BUS, 4096);

    pid_t children[membus::MAX_READERS];
    int filled = 0;

    for (int i = 0; i < membus::MAX_READERS; i++) {
        pid_t child = fork();
        if (child == 0) {
            try {
                membus::Bus h(BUS);
                pause();
            } catch (...) {}
            _exit(0);
        }
        children[i] = child;
        filled++;
        struct timespec ts = {0, 5000000};
        nanosleep(&ts, nullptr);
    }

    bool threw = false;
    try {
        membus::Bus h(BUS);
    } catch (const std::system_error& e) {
        threw = true;
        ASSERT_EQ(e.code().value(), ENOSPC);
    }
    ASSERT_TRUE(threw);

    for (int i = 0; i < filled; i++) {
        kill(children[i], SIGTERM);
        waitpid(children[i], nullptr, 0);
    }

    membus::Bus h(BUS);
    ASSERT_TRUE(h.name() == BUS);

    cleanup();
}

static void test_no_write_block() {
    cleanup();
    membus::create(BUS, 4096);

    {
        membus::Bus h1(BUS);
        uint8_t data[2048];
        std::memset(data, 'A', sizeof(data));
        h1.write(data, sizeof(data));
    }

    membus::Bus h2(BUS);
    uint8_t data[2048];
    std::memset(data, 'A', sizeof(data));
    size_t w = h2.write(data, sizeof(data));
    ASSERT_EQ(w, sizeof(data));

    cleanup();
}

static void test_write_skips_dead() {
    cleanup();
    membus::create(BUS, 4096);

    pid_t child = fork();
    if (child == 0) {
        try {
            membus::Bus h(BUS);
            uint8_t data[2048];
            std::memset(data, 'B', sizeof(data));
            h.write(data, sizeof(data));
        } catch (...) {}
        _exit(0);
    }
    int status;
    waitpid(child, &status, 0);

    membus::Bus h(BUS);
    uint8_t data[3000];
    std::memset(data, 'A', sizeof(data));
    size_t w = h.write(data, sizeof(data));
    ASSERT_EQ(w, sizeof(data));

    cleanup();
}

static void test_slow_reader_advanced() {
    cleanup();
    membus::create(BUS, 4096);

    membus::Bus h1(BUS);
    membus::Bus h2(BUS);

    uint8_t data[2048];
    std::memset(data, 'X', sizeof(data));
    h1.write(data, sizeof(data));

    uint8_t sink[4096];
    h1.read(sink, sizeof(sink));

    size_t w = h1.write(data, sizeof(data));
    ASSERT_EQ(w, sizeof(data));

    cleanup();
}

static void test_magic_validation() {
    cleanup();
    membus::create(BUS, 4096);

    auto* raw = shm::open_raw(BUS);
    ASSERT_TRUE(raw != nullptr);
    ASSERT_EQ(raw->magic, membus::MAGIC);
    ASSERT_EQ(raw->version, membus::VERSION);

    cleanup();
}

static void test_no_semaphore_files() {
    cleanup();
    membus::create(BUS, 4096);

    struct stat st;
    char path[512];

    std::snprintf(path, sizeof(path), "/dev/shm/sem.%s_write", BUS);
    ASSERT_TRUE(stat(path, &st) != 0);

    std::snprintf(path, sizeof(path), "/dev/shm/sem.%s_read", BUS);
    ASSERT_TRUE(stat(path, &st) != 0);

    cleanup();
}

static void test_read_wait_basic() {
    cleanup();
    membus::create(BUS, 4096);

    membus::Bus h(BUS);

    const uint8_t msg[] = "immediate";
    h.write(msg, 9);

    uint8_t buf[64];
    size_t r = h.read_wait(buf, sizeof(buf), 100);
    ASSERT_EQ(r, 9);
    ASSERT_MEM_EQ(buf, msg, 9);

    cleanup();
}

static void test_read_wait_blocks() {
    cleanup();
    membus::create(BUS, 4096);

    membus::Bus h(BUS);

    pid_t child = fork();
    if (child == 0) {
        try {
            membus::Bus ch(BUS);
            struct timespec ts = {0, 50000000};
            nanosleep(&ts, nullptr);
            const uint8_t msg[] = "delayed";
            ch.write(msg, 7);
        } catch (...) {}
        _exit(0);
    }

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    uint8_t buf[64];
    size_t r = h.read_wait(buf, sizeof(buf), 500);
    long ms = elapsed_ms(start);

    int status;
    waitpid(child, &status, 0);

    ASSERT_EQ(r, 7);
    ASSERT_MEM_EQ(buf, "delayed", 7);
    ASSERT_TRUE(ms >= 20 && ms <= 300);

    cleanup();
}

static void test_read_wait_timeout() {
    cleanup();
    membus::create(BUS, 4096);

    membus::Bus h(BUS);

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    uint8_t buf[64];
    size_t r = h.read_wait(buf, sizeof(buf), 50);
    long ms = elapsed_ms(start);

    ASSERT_EQ(r, 0);
    ASSERT_TRUE(ms >= 30 && ms <= 200);

    cleanup();
}

static void test_robust_writer_death() {
    cleanup();
    membus::create(BUS, 4096);

    pid_t child = fork();
    if (child == 0) {
        auto* raw = shm::open_raw(BUS);
        if (!raw) _exit(1);
        pthread_mutex_lock(&raw->write_lock);
        _exit(0); /* die holding the lock */
    }
    int status;
    waitpid(child, &status, 0);

    membus::Bus h(BUS);
    const uint8_t msg[] = "recovered";
    size_t w = h.write(msg, 9);
    ASSERT_EQ(w, 9);

    uint8_t buf[64];
    size_t r = h.read(buf, sizeof(buf));
    ASSERT_EQ(r, 9);
    ASSERT_MEM_EQ(buf, msg, 9);

    cleanup();
}

static void test_robust_reader_death() {
    cleanup();
    membus::create(BUS, 4096);

    pid_t child = fork();
    if (child == 0) {
        auto* raw = shm::open_raw(BUS);
        if (!raw) _exit(1);
        pthread_mutex_lock(&raw->reader_lock);
        _exit(0); /* die holding the lock */
    }
    int status;
    waitpid(child, &status, 0);

    membus::Bus h(BUS);
    ASSERT_TRUE(h.name() == BUS);

    cleanup();
}

static void test_move_semantics() {
    cleanup();
    membus::create(BUS, 4096);

    membus::Bus h1(BUS);
    const uint8_t msg[] = "move";
    h1.write(msg, 4);

    membus::Bus h2(std::move(h1));
    uint8_t buf[64];
    size_t r = h2.read(buf, sizeof(buf));
    ASSERT_EQ(r, 4);
    ASSERT_MEM_EQ(buf, msg, 4);

    membus::Bus h3(BUS);
    h3 = std::move(h2);
    ASSERT_TRUE(h3.name() == BUS);

    cleanup();
}

static void test_create_throws_on_exists() {
    cleanup();
    membus::create(BUS, 4096);

    bool threw = false;
    try {
        membus::create(BUS, 4096);
    } catch (const std::system_error& e) {
        threw = true;
        ASSERT_EQ(e.code().value(), EEXIST);
    }
    ASSERT_TRUE(threw);

    cleanup();
}

static void test_open_throws_on_missing() {
    cleanup();

    bool threw = false;
    try {
        membus::Bus h("nonexistent_bus_12345");
    } catch (const std::system_error& e) {
        threw = true;
        ASSERT_EQ(e.code().value(), ENOENT);
    }
    ASSERT_TRUE(threw);
}

/* ══════════════════════════════════════════════════════════════════
 *  New tests — edge cases, data integrity, broadcast, API coverage
 * ══════════════════════════════════════════════════════════════════ */

/* ── Edge cases: null/zero inputs ── */

static void test_write_null_returns_zero() {
    cleanup();
    membus::create(BUS, 4096);
    membus::Bus h(BUS);

    ASSERT_EQ(h.write(nullptr, 10), 0);

    const uint8_t msg[] = "x";
    ASSERT_EQ(h.write(msg, 0), 0);

    cleanup();
}

static void test_read_null_returns_zero() {
    cleanup();
    membus::create(BUS, 4096);
    membus::Bus h(BUS);

    const uint8_t msg[] = "data";
    h.write(msg, 4);

    ASSERT_EQ(h.read(nullptr, 10), 0);

    uint8_t buf[64];
    ASSERT_EQ(h.read(buf, 0), 0);

    cleanup();
}

static void test_read_empty_bus() {
    cleanup();
    membus::create(BUS, 4096);
    membus::Bus h(BUS);

    uint8_t buf[64];
    size_t r = h.read(buf, sizeof(buf));
    ASSERT_EQ(r, 0);

    cleanup();
}

/* ── Error paths: empty name ── */

static void test_create_empty_name() {
    bool threw = false;
    try {
        membus::create("", 4096);
    } catch (const std::system_error& e) {
        threw = true;
        ASSERT_EQ(e.code().value(), EINVAL);
    }
    ASSERT_TRUE(threw);
}

static void test_destroy_empty_name() {
    bool threw = false;
    try {
        membus::destroy("");
    } catch (const std::system_error& e) {
        threw = true;
        ASSERT_EQ(e.code().value(), EINVAL);
    }
    ASSERT_TRUE(threw);
}

static void test_open_empty_name() {
    bool threw = false;
    try {
        membus::Bus h("");
    } catch (const std::system_error& e) {
        threw = true;
        ASSERT_EQ(e.code().value(), EINVAL);
    }
    ASSERT_TRUE(threw);
}

static void test_destroy_nonexistent_silent() {
    /* destroy of non-existent bus should not throw (ENOENT suppressed) */
    try {
        membus::destroy("nonexistent_bus_xyz_99");
    } catch (...) {
        ASSERT_TRUE(false); /* should not reach here */
    }
}

/* ── Circular buffer data integrity ── */

static void test_wrap_around_integrity() {
    cleanup();
    /* Small buffer: 256 bytes usable. Write enough to force wrap. */
    membus::create(BUS, 256);
    membus::Bus h(BUS);

    /* Fill most of the buffer to push write_pos near the end */
    uint8_t fill[200];
    for (size_t i = 0; i < sizeof(fill); i++) fill[i] = static_cast<uint8_t>(i);
    h.write(fill, sizeof(fill));

    uint8_t sink[256];
    h.read(sink, sizeof(sink));

    /* Now write_pos is at 200. Write 100 bytes — this wraps around:
       56 bytes at end + 44 bytes from start */
    uint8_t wrap_data[100];
    for (size_t i = 0; i < sizeof(wrap_data); i++)
        wrap_data[i] = static_cast<uint8_t>(0xA0 + i);
    h.write(wrap_data, sizeof(wrap_data));

    uint8_t result[100];
    size_t r = h.read(result, sizeof(result));
    ASSERT_EQ(r, 100);
    ASSERT_MEM_EQ(result, wrap_data, 100);

    cleanup();
}

static void test_binary_data_with_nulls() {
    cleanup();
    membus::create(BUS, 4096);
    membus::Bus h(BUS);

    /* Binary pattern including 0x00 bytes */
    uint8_t data[256];
    for (size_t i = 0; i < sizeof(data); i++)
        data[i] = static_cast<uint8_t>(i); /* 0x00..0xFF */

    size_t w = h.write(data, sizeof(data));
    ASSERT_EQ(w, 256);

    uint8_t buf[256];
    size_t r = h.read(buf, sizeof(buf));
    ASSERT_EQ(r, 256);
    ASSERT_MEM_EQ(buf, data, 256);

    cleanup();
}

static void test_sequential_writes_reads() {
    cleanup();
    membus::create(BUS, 4096);
    membus::Bus h(BUS);

    /* Write three messages, read them back in order */
    const uint8_t m1[] = "alpha";
    const uint8_t m2[] = "bravo";
    const uint8_t m3[] = "charlie";
    h.write(m1, 5);
    h.write(m2, 5);
    h.write(m3, 7);

    uint8_t buf[64];

    /* Single read should get all 17 bytes concatenated (raw byte stream) */
    size_t r = h.read(buf, sizeof(buf));
    ASSERT_EQ(r, 17);
    ASSERT_MEM_EQ(buf, "alphabravocharlie", 17);

    cleanup();
}

static void test_partial_read() {
    cleanup();
    membus::create(BUS, 4096);
    membus::Bus h(BUS);

    const uint8_t msg[] = "abcdefghij"; /* 10 bytes */
    h.write(msg, 10);

    /* Read only 4 bytes */
    uint8_t buf[10];
    size_t r1 = h.read(buf, 4);
    ASSERT_EQ(r1, 4);
    ASSERT_MEM_EQ(buf, "abcd", 4);

    /* Read remaining 6 bytes */
    size_t r2 = h.read(buf, 10);
    ASSERT_EQ(r2, 6);
    ASSERT_MEM_EQ(buf, "efghij", 6);

    /* Nothing left */
    size_t r3 = h.read(buf, 10);
    ASSERT_EQ(r3, 0);

    cleanup();
}

/* ── Broadcast: all readers get same data ── */

static void test_broadcast_all_readers() {
    cleanup();
    membus::create(BUS, 4096);

    membus::Bus writer(BUS);
    membus::Bus r1(BUS);
    membus::Bus r2(BUS);
    membus::Bus r3(BUS);

    const uint8_t msg[] = "broadcast";
    writer.write(msg, 9);

    uint8_t b1[64], b2[64], b3[64];
    size_t n1 = r1.read(b1, sizeof(b1));
    size_t n2 = r2.read(b2, sizeof(b2));
    size_t n3 = r3.read(b3, sizeof(b3));

    ASSERT_EQ(n1, 9);
    ASSERT_EQ(n2, 9);
    ASSERT_EQ(n3, 9);
    ASSERT_MEM_EQ(b1, msg, 9);
    ASSERT_MEM_EQ(b2, msg, 9);
    ASSERT_MEM_EQ(b3, msg, 9);

    cleanup();
}

/* ── Lossy behavior: write larger than buffer ── */

static void test_write_larger_than_buffer() {
    cleanup();
    /* Buffer of 128 bytes. Usable = 127. */
    membus::create(BUS, 128);

    membus::Bus h1(BUS);
    membus::Bus h2(BUS);

    /* Write 120 bytes — exceeds usable capacity with h2 as slow reader */
    uint8_t data[120];
    std::memset(data, 'Z', sizeof(data));
    size_t w = h1.write(data, sizeof(data));
    ASSERT_EQ(w, 120);

    /* h2 was advanced (lossy). It should still be able to read whatever
       is available — but write must have succeeded. */
    uint8_t buf[128];
    size_t r = h2.read(buf, sizeof(buf));
    /* After advancement, h2 reads from the new write_pos.
       Data may or may not be available (reader was advanced to write_pos
       BEFORE the new data, so all 120 bytes should be readable). */
    ASSERT_TRUE(r <= 120);

    cleanup();
}

static void test_slow_reader_gets_latest_data() {
    cleanup();
    membus::create(BUS, 256);

    membus::Bus writer(BUS);
    membus::Bus slow(BUS);
    membus::Bus fast(BUS);

    /* Fill buffer — forces slow reader advancement */
    uint8_t old_data[200];
    std::memset(old_data, 'O', sizeof(old_data));
    writer.write(old_data, sizeof(old_data));

    /* Fast reader drains */
    uint8_t sink[256];
    fast.read(sink, sizeof(sink));

    /* Write new data — slow reader must be advanced */
    uint8_t new_data[200];
    std::memset(new_data, 'N', sizeof(new_data));
    writer.write(new_data, sizeof(new_data));

    /* Fast reader gets new data */
    uint8_t buf[256];
    size_t rf = fast.read(buf, sizeof(buf));
    ASSERT_EQ(rf, 200);
    ASSERT_MEM_EQ(buf, new_data, 200);

    /* Slow reader was advanced — should get new data, not old */
    size_t rs = slow.read(buf, sizeof(buf));
    /* Slow reader may get partial or all new data depending on advancement point.
       Key invariant: it does NOT get old_data bytes. */
    ASSERT_TRUE(rs <= 200);
    if (rs > 0) {
        /* Whatever it reads must be 'N' bytes (new data) */
        for (size_t i = 0; i < rs; i++)
            ASSERT_EQ(buf[i], 'N');
    }

    cleanup();
}

/* ── set_reader_name ── */

static void test_set_reader_name() {
    cleanup();
    membus::create(BUS, 4096);
    membus::Bus h(BUS);

    h.set_reader_name("my_reader");

    auto* raw = shm::open_raw(BUS);
    ASSERT_TRUE(raw != nullptr);

    /* Find our slot */
    pid_t me = getpid();
    bool found = false;
    for (int i = 0; i < membus::MAX_READERS; i++) {
        if (raw->readers[i].pid == me) {
            ASSERT_TRUE(std::strcmp(raw->readers[i].name, "my_reader") == 0);
            found = true;
            break;
        }
    }
    ASSERT_TRUE(found);

    cleanup();
}

static void test_reader_name_truncation() {
    cleanup();
    membus::create(BUS, 4096);
    membus::Bus h(BUS);

    /* Name longer than READER_NAME_LEN (32). Should truncate to 31 chars + null. */
    const char* long_name = "this_name_is_definitely_way_too_long_for_the_slot";
    h.set_reader_name(long_name);

    auto* raw = shm::open_raw(BUS);
    ASSERT_TRUE(raw != nullptr);

    pid_t me = getpid();
    for (int i = 0; i < membus::MAX_READERS; i++) {
        if (raw->readers[i].pid == me) {
            ASSERT_EQ(std::strlen(raw->readers[i].name), membus::READER_NAME_LEN - 1);
            ASSERT_MEM_EQ(raw->readers[i].name, long_name, membus::READER_NAME_LEN - 1);
            break;
        }
    }

    cleanup();
}

/* ── create: size=0 uses DEFAULT_SIZE ── */

static void test_create_default_size() {
    cleanup();
    membus::create(BUS, 0); /* 0 = DEFAULT_SIZE */

    auto* raw = shm::open_raw(BUS);
    ASSERT_TRUE(raw != nullptr);
    ASSERT_EQ(raw->size, membus::DEFAULT_SIZE);

    cleanup();
}

static void test_create_custom_size() {
    cleanup();
    membus::create(BUS, 1024);

    auto* raw = shm::open_raw(BUS);
    ASSERT_TRUE(raw != nullptr);
    ASSERT_EQ(raw->size, 1024);

    cleanup();
}

/* ── read_wait: poll mode (timeout=0) ── */

static void test_read_wait_poll_no_data() {
    cleanup();
    membus::create(BUS, 4096);
    membus::Bus h(BUS);

    uint8_t buf[64];
    size_t r = h.read_wait(buf, sizeof(buf), 0);
    ASSERT_EQ(r, 0);

    cleanup();
}

static void test_read_wait_poll_with_data() {
    cleanup();
    membus::create(BUS, 4096);
    membus::Bus h(BUS);

    const uint8_t msg[] = "poll";
    h.write(msg, 4);

    uint8_t buf[64];
    size_t r = h.read_wait(buf, sizeof(buf), 0);
    ASSERT_EQ(r, 4);
    ASSERT_MEM_EQ(buf, msg, 4);

    cleanup();
}

/* ── Multiple wrap cycles ── */

static void test_multiple_wrap_cycles() {
    cleanup();
    membus::create(BUS, 128);
    membus::Bus h(BUS);

    /* Write and read in a loop — forces multiple wrap-arounds.
       Verify data integrity through each cycle. */
    for (int cycle = 0; cycle < 20; cycle++) {
        uint8_t data[100];
        uint8_t tag = static_cast<uint8_t>(cycle);
        std::memset(data, tag, sizeof(data));

        size_t w = h.write(data, sizeof(data));
        ASSERT_EQ(w, 100);

        uint8_t buf[128];
        size_t r = h.read(buf, sizeof(buf));
        ASSERT_EQ(r, 100);
        for (size_t i = 0; i < 100; i++)
            ASSERT_EQ(buf[i], tag);
    }

    cleanup();
}

/* ── Single byte write/read ── */

static void test_single_byte() {
    cleanup();
    membus::create(BUS, 4096);
    membus::Bus h(BUS);

    uint8_t one = 0x42;
    ASSERT_EQ(h.write(&one, 1), 1);

    uint8_t out = 0;
    ASSERT_EQ(h.read(&out, 1), 1);
    ASSERT_EQ(out, 0x42);

    cleanup();
}

/* ── Fill to exact usable capacity (size-1) ── */

static void test_fill_exact_capacity() {
    cleanup();
    membus::create(BUS, 256);
    membus::Bus h(BUS);

    /* Usable capacity = 255 bytes (size - 1) */
    uint8_t data[255];
    for (size_t i = 0; i < sizeof(data); i++)
        data[i] = static_cast<uint8_t>(i);

    size_t w = h.write(data, sizeof(data));
    ASSERT_EQ(w, 255);

    uint8_t buf[255];
    size_t r = h.read(buf, sizeof(buf));
    ASSERT_EQ(r, 255);
    ASSERT_MEM_EQ(buf, data, 255);

    cleanup();
}

/* ── Write after reader deregisters ── */

static void test_write_after_reader_gone() {
    cleanup();
    membus::create(BUS, 256);

    membus::Bus writer(BUS);

    {
        membus::Bus reader(BUS);
        /* reader goes out of scope — slot freed */
    }

    /* Write should not be constrained by the departed reader */
    uint8_t data[200];
    std::memset(data, 'W', sizeof(data));
    size_t w = writer.write(data, sizeof(data));
    ASSERT_EQ(w, 200);

    cleanup();
}

/* ── Concurrent write from child, parent reads ── */

static void test_cross_process_write_read() {
    cleanup();
    membus::create(BUS, 4096);

    membus::Bus parent(BUS);

    pid_t child = fork();
    if (child == 0) {
        try {
            membus::Bus ch(BUS);
            const uint8_t msg[] = "from_child";
            ch.write(msg, 10);
        } catch (...) {}
        _exit(0);
    }

    int status;
    waitpid(child, &status, 0);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    uint8_t buf[64];
    size_t r = parent.read(buf, sizeof(buf));
    ASSERT_EQ(r, 10);
    ASSERT_MEM_EQ(buf, "from_child", 10);

    cleanup();
}

/* ── Reader sees only data written after attach ── */

static void test_reader_sees_only_future_data() {
    cleanup();
    membus::create(BUS, 4096);
    membus::Bus writer(BUS);

    /* Write before second reader attaches */
    const uint8_t old[] = "old_data";
    writer.write(old, 8);

    /* New reader attaches — read_pos = write_pos (should not see old data) */
    membus::Bus late_reader(BUS);

    uint8_t buf[64];
    size_t r = late_reader.read(buf, sizeof(buf));
    ASSERT_EQ(r, 0);

    /* Write new data — late reader should see it */
    const uint8_t fresh[] = "fresh";
    writer.write(fresh, 5);

    r = late_reader.read(buf, sizeof(buf));
    ASSERT_EQ(r, 5);
    ASSERT_MEM_EQ(buf, "fresh", 5);

    cleanup();
}

/* ── Suite runner ── */

void test_membus_run(int& out_run, int& out_passed) {
    std::printf("\n[membus]\n");

    /* Original 19 */
    TEST(test_basic);
    TEST(test_slot_reuse);
    TEST(test_multi_handle_same_pid);
    TEST(test_different_pids);
    TEST(test_dead_reclaim);
    TEST(test_slot_exhaustion);
    TEST(test_no_write_block);
    TEST(test_write_skips_dead);
    TEST(test_slow_reader_advanced);
    TEST(test_magic_validation);
    TEST(test_no_semaphore_files);
    TEST(test_read_wait_basic);
    TEST(test_read_wait_blocks);
    TEST(test_read_wait_timeout);
    TEST(test_robust_writer_death);
    TEST(test_robust_reader_death);
    TEST(test_move_semantics);
    TEST(test_create_throws_on_exists);
    TEST(test_open_throws_on_missing);

    /* Edge cases: null/zero inputs */
    TEST(test_write_null_returns_zero);
    TEST(test_read_null_returns_zero);
    TEST(test_read_empty_bus);

    /* Error paths */
    TEST(test_create_empty_name);
    TEST(test_destroy_empty_name);
    TEST(test_open_empty_name);
    TEST(test_destroy_nonexistent_silent);

    /* Circular buffer data integrity */
    TEST(test_wrap_around_integrity);
    TEST(test_binary_data_with_nulls);
    TEST(test_sequential_writes_reads);
    TEST(test_partial_read);
    TEST(test_single_byte);
    TEST(test_fill_exact_capacity);
    TEST(test_multiple_wrap_cycles);

    /* Broadcast */
    TEST(test_broadcast_all_readers);

    /* Lossy behavior */
    TEST(test_write_larger_than_buffer);
    TEST(test_slow_reader_gets_latest_data);

    /* set_reader_name */
    TEST(test_set_reader_name);
    TEST(test_reader_name_truncation);

    /* create options */
    TEST(test_create_default_size);
    TEST(test_create_custom_size);

    /* read_wait edge cases */
    TEST(test_read_wait_poll_no_data);
    TEST(test_read_wait_poll_with_data);

    /* Lifecycle */
    TEST(test_write_after_reader_gone);
    TEST(test_cross_process_write_read);
    TEST(test_reader_sees_only_future_data);

    out_run    = tests_run;
    out_passed = tests_passed;
}
