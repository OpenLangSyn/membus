#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "membus.hpp"

#include <algorithm>
#include <climits>
#include <cstring>
#include <string>
#include <system_error>

#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace membus {

/* ── SHM layout (internal) ── */

struct ReaderSlot {
    size_t  read_pos;
    pid_t   pid;
    char    name[READER_NAME_LEN];
};

struct ShmBuffer {
    uint32_t        magic;
    uint32_t        version;
    size_t          size;
    size_t          write_pos;
    uint32_t        reader_count;
    pthread_mutex_t write_lock;
    pthread_mutex_t reader_lock;
    uint32_t        write_seq;
    ReaderSlot      readers[MAX_READERS];
    char            data[];
};

/* ── Helpers ── */

static std::string make_shm_name(std::string_view bus_name) {
    std::string s("/");
    s += bus_name;
    s += "_shm";
    return s;
}

static void init_robust_mutex(pthread_mutex_t& m) {
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0)
        throw std::system_error(errno, std::system_category(), "mutexattr_init");

    auto cleanup = [&]{ pthread_mutexattr_destroy(&attr); };

    if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0) {
        cleanup();
        throw std::system_error(errno, std::system_category(), "mutexattr_setpshared");
    }
    if (pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) != 0) {
        cleanup();
        throw std::system_error(errno, std::system_category(), "mutexattr_setrobust");
    }

    int rc = pthread_mutex_init(&m, &attr);
    cleanup();
    if (rc != 0)
        throw std::system_error(rc, std::system_category(), "mutex_init");
}

static int robust_lock(pthread_mutex_t& m) {
    int rc = pthread_mutex_lock(&m);
    if (rc == EOWNERDEAD) {
        pthread_mutex_consistent(&m);
        return 0;
    }
    if (rc == ENOTRECOVERABLE) {
        errno = EIO;
        return -1;
    }
    return rc == 0 ? 0 : -1;
}

/* ── Free functions ── */

void create(std::string_view name, size_t size) {
    if (name.empty())
        throw std::system_error(EINVAL, std::system_category(), "membus::create: empty name");

    if (size == 0) size = DEFAULT_SIZE;

    auto shm_name = make_shm_name(name);
    int fd = shm_open(shm_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0660);
    if (fd == -1)
        throw std::system_error(errno, std::system_category(), "membus::create: shm_open");

    size_t total = sizeof(ShmBuffer) + size;

    if (ftruncate(fd, static_cast<off_t>(total)) == -1) {
        int e = errno;
        close(fd);
        shm_unlink(shm_name.c_str());
        throw std::system_error(e, std::system_category(), "membus::create: ftruncate");
    }

    auto* buf = static_cast<ShmBuffer*>(
        mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (buf == MAP_FAILED) {
        int e = errno;
        close(fd);
        shm_unlink(shm_name.c_str());
        throw std::system_error(e, std::system_category(), "membus::create: mmap");
    }

    std::memset(buf, 0, sizeof(ShmBuffer));
    buf->magic   = MAGIC;
    buf->version = VERSION;
    buf->size    = size;

    try {
        init_robust_mutex(buf->write_lock);
        init_robust_mutex(buf->reader_lock);
    } catch (...) {
        munmap(buf, total);
        close(fd);
        shm_unlink(shm_name.c_str());
        throw;
    }

    munmap(buf, total);
    close(fd);
}

void destroy(std::string_view name) {
    if (name.empty())
        throw std::system_error(EINVAL, std::system_category(), "membus::destroy: empty name");

    auto shm_name = make_shm_name(name);
    if (shm_unlink(shm_name.c_str()) == -1 && errno != ENOENT)
        throw std::system_error(errno, std::system_category(), "membus::destroy: shm_unlink");
}

/* ── Bus::Impl ── */

struct Bus::Impl {
    std::string name;
    int         shm_fd      = -1;
    ShmBuffer*  buffer      = nullptr;
    size_t      map_size    = 0;
    int         reader_slot = -1;

    ~Impl() {
        if (reader_slot >= 0 && buffer) {
            if (robust_lock(buffer->reader_lock) == 0) {
                buffer->readers[reader_slot].pid = 0;
                buffer->readers[reader_slot].name[0] = '\0';
                if (buffer->reader_count > 0)
                    buffer->reader_count--;
                pthread_mutex_unlock(&buffer->reader_lock);
            }
        }
        if (buffer)
            munmap(buffer, map_size);
        if (shm_fd != -1)
            ::close(shm_fd);
    }
};

/* ── Bus lifecycle ── */

Bus::Bus(std::string_view bus_name) : impl_(std::make_unique<Impl>()) {
    if (bus_name.empty())
        throw std::system_error(EINVAL, std::system_category(), "Bus: empty name");

    impl_->name = bus_name;
    auto shm_name = make_shm_name(bus_name);

    impl_->shm_fd = shm_open(shm_name.c_str(), O_RDWR, 0660);
    if (impl_->shm_fd == -1)
        throw std::system_error(errno, std::system_category(), "Bus: shm_open");

    struct stat st;
    if (fstat(impl_->shm_fd, &st) == -1)
        throw std::system_error(errno, std::system_category(), "Bus: fstat");

    impl_->map_size = static_cast<size_t>(st.st_size);
    impl_->buffer = static_cast<ShmBuffer*>(
        mmap(nullptr, impl_->map_size, PROT_READ | PROT_WRITE,
             MAP_SHARED, impl_->shm_fd, 0));

    if (impl_->buffer == MAP_FAILED) {
        impl_->buffer = nullptr;
        throw std::system_error(errno, std::system_category(), "Bus: mmap");
    }

    if (impl_->buffer->magic != MAGIC)
        throw std::system_error(EINVAL, std::system_category(), "Bus: bad magic");

    if (robust_lock(impl_->buffer->reader_lock) != 0)
        throw std::system_error(errno, std::system_category(), "Bus: reader_lock");

    /* Reclaim dead-process slots */
    for (int i = 0; i < MAX_READERS; i++) {
        pid_t p = impl_->buffer->readers[i].pid;
        if (p != 0 && kill(p, 0) == -1 && errno == ESRCH) {
            impl_->buffer->readers[i].pid = 0;
            impl_->buffer->readers[i].name[0] = '\0';
            if (impl_->buffer->reader_count > 0)
                impl_->buffer->reader_count--;
        }
    }

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < MAX_READERS; i++) {
        if (impl_->buffer->readers[i].pid == 0) {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        pthread_mutex_unlock(&impl_->buffer->reader_lock);
        throw std::system_error(ENOSPC, std::system_category(), "Bus: no free reader slot");
    }

    impl_->buffer->readers[slot].pid      = getpid();
    impl_->buffer->readers[slot].read_pos = impl_->buffer->write_pos;
    impl_->buffer->readers[slot].name[0]  = '\0';
    impl_->buffer->reader_count++;
    impl_->reader_slot = slot;

    pthread_mutex_unlock(&impl_->buffer->reader_lock);
}

Bus::~Bus() = default;
Bus::Bus(Bus&&) noexcept = default;
Bus& Bus::operator=(Bus&&) noexcept = default;

std::string_view Bus::name() const { return impl_->name; }

/* ── Bus::write ── */

size_t Bus::write(const uint8_t* data, size_t len) {
    if (!data || len == 0) return 0;

    auto* shm = impl_->buffer;

    if (robust_lock(shm->write_lock) != 0)
        throw std::system_error(errno, std::system_category(), "Bus::write: lock");

    size_t buf_size  = shm->size;
    size_t write_pos = shm->write_pos;

    /* Available space — skip dead slots */
    size_t available = buf_size - 1;
    for (int i = 0; i < MAX_READERS; i++) {
        pid_t p = shm->readers[i].pid;
        if (p != 0) {
            if (kill(p, 0) == -1 && errno == ESRCH) {
                shm->readers[i].pid = 0;
                continue;
            }
            size_t rpos      = shm->readers[i].read_pos;
            size_t distance  = (write_pos - rpos + buf_size) % buf_size;
            size_t slot_free = buf_size - distance - 1;
            if (slot_free < available)
                available = slot_free;
        }
    }

    /* Advance slow live readers (lossy) */
    if (len > available) {
        for (int i = 0; i < MAX_READERS; i++) {
            if (shm->readers[i].pid == 0) continue;
            size_t rpos      = shm->readers[i].read_pos;
            size_t distance  = (write_pos - rpos + buf_size) % buf_size;
            size_t slot_free = buf_size - distance - 1;
            if (slot_free < len)
                shm->readers[i].read_pos = write_pos;
        }
    }

    /* Circular write */
    size_t tail = buf_size - write_pos;
    if (len <= tail) {
        std::memcpy(&shm->data[write_pos], data, len);
        write_pos = (write_pos + len) % buf_size;
    } else {
        std::memcpy(&shm->data[write_pos], data, tail);
        std::memcpy(&shm->data[0], data + tail, len - tail);
        write_pos = len - tail;
    }

    shm->write_pos = write_pos;
    __atomic_add_fetch(&shm->write_seq, 1, __ATOMIC_RELEASE);

    pthread_mutex_unlock(&shm->write_lock);

    syscall(SYS_futex, &shm->write_seq, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);

    return len;
}

/* ── Bus::read ── */

size_t Bus::read(uint8_t* out, size_t len) {
    if (!out || len == 0) return 0;

    auto* shm = impl_->buffer;
    int slot  = impl_->reader_slot;

    if (robust_lock(shm->reader_lock) != 0)
        throw std::system_error(errno, std::system_category(), "Bus::read: lock");

    size_t buf_size  = shm->size;
    size_t write_pos = shm->write_pos;
    size_t read_pos  = shm->readers[slot].read_pos;

    size_t avail = (write_pos >= read_pos)
                 ? (write_pos - read_pos)
                 : (buf_size - (read_pos - write_pos));

    size_t to_read = std::min(len, avail);

    if (to_read == 0) {
        pthread_mutex_unlock(&shm->reader_lock);
        return 0;
    }

    /* Circular read */
    size_t tail = buf_size - read_pos;
    if (to_read <= tail) {
        std::memcpy(out, &shm->data[read_pos], to_read);
        read_pos = (read_pos + to_read) % buf_size;
    } else {
        std::memcpy(out, &shm->data[read_pos], tail);
        std::memcpy(out + tail, &shm->data[0], to_read - tail);
        read_pos = to_read - tail;
    }

    shm->readers[slot].read_pos = read_pos;
    pthread_mutex_unlock(&shm->reader_lock);

    return to_read;
}

/* ── Bus::read_wait ── */

size_t Bus::read_wait(uint8_t* out, size_t len, int timeout_ms) {
    size_t n = read(out, len);
    if (n != 0) return n;
    if (timeout_ms == 0) return 0;

    uint32_t seq = __atomic_load_n(&impl_->buffer->write_seq, __ATOMIC_ACQUIRE);

    struct timespec ts;
    struct timespec* tsp = nullptr;
    if (timeout_ms > 0) {
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }

    syscall(SYS_futex, &impl_->buffer->write_seq, FUTEX_WAIT, seq, tsp, nullptr, 0);

    return read(out, len);
}

/* ── Bus::set_reader_name ── */

void Bus::set_reader_name(std::string_view reader_name) {
    auto* shm = impl_->buffer;
    int slot  = impl_->reader_slot;

    if (robust_lock(shm->reader_lock) != 0) return;

    size_t n = std::min(reader_name.size(), READER_NAME_LEN - 1);
    std::memcpy(shm->readers[slot].name, reader_name.data(), n);
    shm->readers[slot].name[n] = '\0';

    pthread_mutex_unlock(&shm->reader_lock);
}

} // namespace membus
