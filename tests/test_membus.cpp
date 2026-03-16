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

/* ── Tests ── */

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

/* ── Suite runner ── */

void test_membus_run(int& out_run, int& out_passed) {
    std::printf("\n[membus]\n");

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

    out_run    = tests_run;
    out_passed = tests_passed;
}
