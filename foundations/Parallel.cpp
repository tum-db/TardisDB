#include "foundations/Parallel.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace Parallel {

size_t threads = std::max(1u, std::thread::hardware_concurrency());
size_t morselSize = 16384;

namespace {

/// Threads that stay alive between queries, so that short queries do not pay for starting threads
class WorkerPool {
public:
    ~WorkerPool() {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _stop = true;
        }
        _wake.notify_all();
        for (auto & thread : _threads) thread.join();
    }

    /// Runs job(worker) for worker = 0 .. workers - 1; worker 0 is the calling thread
    void run(size_t workers, const std::function<void(size_t)> & job) {
        std::unique_lock<std::mutex> lock(_mutex);
        while (_threads.size() + 1 < workers) {
            _threads.emplace_back(&WorkerPool::loop, this, _threads.size() + 1);
        }
        _job = &job;
        _workers = workers;
        _pending = workers - 1;
        ++_generation;
        lock.unlock();
        _wake.notify_all();
        job(0);
        lock.lock();
        _done.wait(lock, [&] { return _pending == 0; });
        _job = nullptr;
    }

private:
    void loop(size_t worker) {
        uint64_t seen = 0;
        std::unique_lock<std::mutex> lock(_mutex);
        for (;;) {
            _wake.wait(lock, [&] { return _stop || _generation != seen; });
            if (_stop) return;
            seen = _generation;
            if (worker >= _workers) continue;
            const auto * job = _job;
            lock.unlock();
            (*job)(worker);
            lock.lock();
            if (--_pending == 0) _done.notify_one();
        }
    }

    std::mutex _mutex;
    std::condition_variable _wake;
    std::condition_variable _done;
    std::vector<std::thread> _threads;
    const std::function<void(size_t)> * _job = nullptr;
    size_t _workers = 0;
    size_t _pending = 0;
    uint64_t _generation = 0;
    bool _stop = false;
};

WorkerPool & workerPool()
{
    static WorkerPool pool;
    return pool;
}

std::mutex resultMutex;

} // end anonymous namespace

void runMorsels(size_t workers, size_t tuples, size_t positions, morsel_function_t function)
{
    const size_t size = std::max<size_t>(1, morselSize);
    const size_t perPosition = (tuples + size - 1) / size;
    const size_t total = perPosition * positions;
    std::atomic<size_t> next(0);
    std::atomic<bool> failed(false);
    std::exception_ptr failure;
    std::function<void(size_t)> work = [&](size_t worker) {
        try {
            for (size_t m = next.fetch_add(1, std::memory_order_relaxed); m < total && !failed; m = next.fetch_add(1, std::memory_order_relaxed)) {
                const size_t begin = (m % perPosition) * size;
                function(begin, std::min(tuples, begin + size), worker, m / perPosition);
            }
        } catch (...) {
            if (!failed.exchange(true)) failure = std::current_exception();
        }
    };
    workerPool().run(std::max<size_t>(1, workers), work);
    if (failure) std::rethrow_exception(failure);
}

void lockResults()
{
    resultMutex.lock();
}

void unlockResults()
{
    resultMutex.unlock();
}

} // end namespace Parallel
