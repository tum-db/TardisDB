#pragma once

#include <cstddef>
#include <functional>

/// Morsel-driven execution of compiled pipelines: a compiled morsel function scans a range of
/// tuples; workers of a persistent pool take morsels from a shared counter.
namespace Parallel {

/// workers per query (default: all hardware threads); 1 keeps the serial code paths. Queries that run
/// at the same time share one pool of threads.
extern size_t threads;

/// tuples per morsel
extern size_t morselSize;

using morsel_function_t = void (*)(size_t begin, size_t end, size_t worker, size_t position);

/// Calls function(begin, end, worker, position) for morsels covering [0, tuples) and every position
/// in [0, positions); all morsels of a position are handed out before those of the next one.
/// Rethrows the first exception of a worker.
void runMorsels(size_t workers, size_t tuples, size_t positions, morsel_function_t function);

/// Calls task(t, worker) for every t in [0, tasks), handed out one at a time to up to \p workers threads; rethrows the first exception
void runTasks(size_t workers, size_t tasks, const std::function<void(size_t task, size_t worker)> & task);

/// Serialise the result callbacks of the workers (called from compiled code)
void lockResults();
void unlockResults();

} // end namespace Parallel
