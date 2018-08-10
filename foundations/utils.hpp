
#pragma once

#include <set>

#include <llvm/IR/Type.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Value.h>

// some color codes
#define RED   "\x1B[31m"
#define GRN   "\x1B[32m"
#define YEL   "\x1B[33m"
#define BLU   "\x1B[34m"
#define MAG   "\x1B[35m"
#define CYN   "\x1B[36m"
#define WHT   "\x1B[37m"
#define RESET "\x1B[0m"

// source: linux kernel
#define likely(x)      __builtin_expect(!!(x), 1)
#define unlikely(x)    __builtin_expect(!!(x), 0)

#define unreachable()  __builtin_unreachable()

typedef uint64_t hash_t;

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

} // end namespace HashUtils

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


namespace std {

template<typename ... T>
struct hash<std::tuple<T...>> {
    size_t operator()(const std::tuple<T...> & t) const
    {
        size_t seed = 0;
        return HashTuple<std::tuple<T...>>::reduce(t, seed);
    }
};

} // end namespace std

template<typename T>
bool is_subset(std::set<T> subset, std::set<T> superset)
{
    for (const auto el : subset) {
        if (superset.find(el) == superset.end()) {
            return false;
        }
    }
    return true;
}

inline llvm::Type * getPointeeType(llvm::Value * ptr)
{
    return llvm::cast<llvm::PointerType>(ptr->getType()->getScalarType())->getElementType();
}

std::vector<std::string> split(const std::string & str, char delim);

std::string readline();
