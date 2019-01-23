#include "foundations/StringPool.hpp"

#include <cstring>
#include "utils/hashing.hpp"

using namespace HashUtils;

size_t SqlStringHash::operator()(const sql_string_t & str) const {
    return hashByteArray(str.second.get(), str.first);
}

bool SqlStringEqual::operator()(const sql_string_t & str1, const sql_string_t & str2) const {
    // compare length
    if (str1.second != str2.second) {
        return false;
    }

    return (0 == std::memcmp(str1.second.get(), str2.second.get(), str1.first));
}

StringPool & StringPool::instance() {
    static StringPool pool;
    return pool;
}

const sql_string_t & StringPool::put(sql_string_t && str) {
//    return *pool.insert(std::move(str)).first;
    pool.push_back(std::move(str));
    return pool.back();
}
