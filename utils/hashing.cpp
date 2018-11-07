#include "utils/hashing.hpp"

namespace HashUtils {

hash_t hashByteArray(const uint8_t * bytes, size_t len) {
    // TODO vectorized hash function
    hash_t h = 0;
    for (size_t i = 0; i < len; ++i) {
        h = (h << 5) | (h >> 27);
        h ^= static_cast<hash_t>(bytes[i]);
    }
    return h;
}

} // end namespace Hashing
