#pragma once

#include <cstdint>
#include <tuple>

using hash_t = uint64_t;

namespace HashUtils {

// source: boost
inline void hash_combine(uint64_t & h, uint64_t k)
{
    const uint64_t m = 0xc6a4a7935bd1e995ul;
    const int r = 47;

    k *= m;
    k ^= k >> r;
    k *= m;

    h ^= k;
    h *= m;

    // Completely arbitrary number, to prevent 0's
    // from hashing to 0.
    h += 0xe6546b64;
}

template<class T>
hash_t make_hash(const T & v)
{
    return std::hash<T>()(v);
}

template<class T>
static hash_t hashInteger(T value) {
    hash_t x = value;
    hash_t r = 88172645463325252ul;
    r = r ^ x;
    r = r ^ (r << 13ul);
    r = r ^ (r >> 7ul);
    r = r ^ (r << 17ul);
    return r;
}

hash_t hashByteArray(const uint8_t * bytes, size_t len);

// recursive template reduce algorithm
template<typename Tuple, size_t size = std::tuple_size<Tuple>::value>
struct HashTuple {
    static size_t reduce(Tuple t, size_t seed)
    {
        HashUtils::hash_combine( seed, HashUtils::make_hash( std::get<size - 1>(t) ) );
        return HashTuple<Tuple, size - 1>::reduce(t, seed);
    }
};

// base case: only one value left
template<typename Tuple>
struct HashTuple<Tuple, 1> {
    static size_t reduce(Tuple t, size_t seed)
    {
        HashUtils::hash_combine( seed, HashUtils::make_hash( std::get<0>(t) ) );
        return seed;
    }
};

} // end namespace HashUtils

namespace std {

template<typename ... T>
struct hash<std::tuple<T...>> {
    size_t operator()(const std::tuple<T...> & t) const
    {
        size_t seed = 0;
        return HashUtils::HashTuple<std::tuple<T...>>::reduce(t, seed);
    }
};

} // end namespace std
