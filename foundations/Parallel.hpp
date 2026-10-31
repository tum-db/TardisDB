#pragma once

#include <cstddef>

/// Morsel-driven execution of compiled pipelines: a compiled morsel function scans a range of
/// tuples; workers of a persistent pool take morsels from a shared counter.
namespace Parallel {

/// workers per query (default: all hardware threads); 1 keeps the serial code paths
extern size_t threads;

/// tuples per morsel
extern size_t morselSize;

using morsel_function_t = void (*)(size_t begin, size_t end, size_t worker, size_t position);

/// Calls function(begin, end, worker, position) for morsels covering [0, tuples) and every position
/// in [0, positions); all morsels of a position are handed out before those of the next one.
/// Rethrows the first exception of a worker.
void runMorsels(size_t workers, size_t tuples, size_t positions, morsel_function_t function);

/// Serialise the result callbacks of the workers (called from compiled code)
void lockResults();
void unlockResults();

} // end namespace Parallel
