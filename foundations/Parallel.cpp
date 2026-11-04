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

/// Threads that stay alive between queries, so that short queries do not pay for starting threads.
/// Queries that run at the same time share the threads: a free thread joins the job with the fewest
/// workers that still has morsels and stays with it until its morsels are gone.
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

    /// Runs job(worker) on the calling thread (worker 0) and on up to workers - 1 threads of the pool
    /// (workers 1 ..); job returns once no morsel is left
    void run(size_t workers, const std::function<void(size_t)> & job) {
        Job current{&job, workers};
        std::unique_lock<std::mutex> lock(_mutex);
        while (_threads.size() + 1 < workers) {
            _threads.emplace_back(&WorkerPool::loop, this);
        }
        _jobs.push_back(&current);
        lock.unlock();
        _wake.notify_all();
        job(0);
        lock.lock();
        current.exhausted = true;
        _jobs.erase(std::find(_jobs.begin(), _jobs.end(), &current));
        _done.wait(lock, [&] { return current.running == 0; });
    }

private:
    struct Job {
        const std::function<void(size_t)> * function;
        size_t workers;
        size_t joined = 1;      // worker numbers handed out; 0 is the calling thread
        size_t running = 0;     // threads of the pool inside function
        bool exhausted = false; // a worker has returned from function
    };

    /// \returns The job with the fewest workers that still takes workers, or nullptr
    Job * pick() {
        Job * best = nullptr;
        for (Job * job : _jobs) {
            if (job->exhausted || job->joined >= job->workers) continue;
            if (best == nullptr || job->joined < best->joined) best = job;
        }
        return best;
    }

    void loop() {
        std::unique_lock<std::mutex> lock(_mutex);
        for (;;) {
            Job * job = nullptr;
            _wake.wait(lock, [&] { return _stop || (job = pick()) != nullptr; });
            if (_stop) return;
            const size_t worker = job->joined++;
            ++job->running;
            lock.unlock();
            (*job->function)(worker);
            lock.lock();
            job->exhausted = true;
            if (--job->running == 0) _done.notify_all();
        }
    }

    std::mutex _mutex;
    std::condition_variable _wake;
    std::condition_variable _done;
    std::vector<std::thread> _threads;
    std::vector<Job *> _jobs;
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
