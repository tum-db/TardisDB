#pragma once

#include <atomic>
#include <tuple>
#include <cstdint>
#include <immintrin.h>

namespace opt_lock {

using lock_t = std::atomic<uint64_t>;

/*
usage:
struct Node {
    lock_t lock;
    // ...
};
*/

// Helper functions
inline uint64_t setLockedBit(uint64_t version) {
    return (version + 2);
}

inline bool isLocked(uint64_t version) {
    return ((version & 2) == 2);
}

inline bool isObsolete(uint64_t version) {
    return ((version & 1) == 1);
}

template<class T>
uint64_t awaitNodeUnlocked(T * node) {
    uint64_t version = node->lock.load();
    while (isLocked(version)) {
        _mm_pause();
        version = node->lock.load();
    }
    return version;
}

/// \return (version, restartRequired)
template<class T>
auto readLockOrRestart(T * node) {
    uint64_t version = awaitNodeUnlocked(node);
    bool restart = isObsolete(version);
    return std::make_tuple(version, restart);
}

/// \return restartRequired
template<class T>
auto readUnlockOrRestart(T * node, uint64_t version) {
    return (version != node->lock.load());
}

/// \return restartRequired
template<class T>
auto checkOrRestart(T * node, uint64_t version) {
    return readUnlockOrRestart(node, version);
}

/// \return restartRequired
template<class T1, class T2>
auto readUnlockOrRestart(T1 * node, uint64_t version, T2 * lockedNode) {
    if (version != node->lock.load()) {
        writeUnlock(lockedNode);
        return true;
    }
    return false;
}

template<class T>
void writeUnlock(T * node) {
    // reset locked bit and overflow into version
    node->lock.fetch_add(2);
}

template<class T>
void writeUnlockObsolete(T * node) {
    // set obsolete, reset locked, overflow into version
    node->lock.fetch_add(3);
}

/// \return restartRequired
template<class T>
auto upgradeToWriteLockOrRestart(T * node, uint64_t version) {
    return (!node->lock.compare_exchange_strong(version, setLockedBit(version)));
}

/// \return restartRequired
template<class T1, class T2>
auto upgradeToWriteLockOrRestart(T1 * node, uint64_t version, T2 * lockedNode) {
    if (!node->lock.compare_exchange_strong(version, setLockedBit(version))) {
        writeUnlock(lockedNode);
        return true;
    }
    return false;
}

/// \return (version, restartRequired)
template<class T>
auto writeLockOrRestart(T * node) {
    uint64_t version;
    bool restartRequired;
    do {
        std::tie(version, restartRequired) = readLockOrRestart(node);
    } while (upgradeToWriteLockOrRestart(node, version));
    return std::make_tuple(version, restartRequired);
}

/// \return version
template<class T>
auto downgradeToReadLock(T * node) {
    // reset locked bit and overflow into version
    uint64_t previousVersion = node->lock.fetch_add(2);
    assert(isLocked(previousVersion));
    return (previousVersion + 2);
}

} // end namespace OptimisticLock
