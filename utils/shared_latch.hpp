#pragma once

#include <pthread.h>
#include <system_error>

/// A read-write latch that prefers writers: once a writer waits, new readers wait as well, so that
/// statements that keep overlapping cannot starve a statement that needs the latch exclusive (the
/// std::shared_mutex of glibc prefers readers). A thread must not take it shared twice, since a
/// writer waiting in between would block the second request. Meets the SharedMutex requirements of
/// std::shared_lock and std::unique_lock.
class SharedLatch {
public:
    SharedLatch() {
        pthread_rwlockattr_t attributes;
        pthread_rwlockattr_init(&attributes);
#ifdef __GLIBC__
        pthread_rwlockattr_setkind_np(&attributes, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif
        pthread_rwlock_init(&_latch, &attributes);
        pthread_rwlockattr_destroy(&attributes);
    }

    ~SharedLatch() { pthread_rwlock_destroy(&_latch); }

    SharedLatch(const SharedLatch &) = delete;
    SharedLatch & operator =(const SharedLatch &) = delete;

    void lock() { check(pthread_rwlock_wrlock(&_latch)); }
    bool try_lock() { return pthread_rwlock_trywrlock(&_latch) == 0; }
    void unlock() { pthread_rwlock_unlock(&_latch); }

    void lock_shared() { check(pthread_rwlock_rdlock(&_latch)); }
    bool try_lock_shared() { return pthread_rwlock_tryrdlock(&_latch) == 0; }
    void unlock_shared() { pthread_rwlock_unlock(&_latch); }

private:
    static void check(int error) {
        if (error != 0) throw std::system_error(error, std::generic_category());
    }

    pthread_rwlock_t _latch;
};
