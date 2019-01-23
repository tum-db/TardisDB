#pragma once

#include <cstdint>
#include <tuple>
#include <memory>
#include <unordered_set>
#include <vector>

using sql_string_t = std::pair<size_t, std::unique_ptr<uint8_t[]>>;

struct SqlStringHash {
	size_t operator()(const sql_string_t & str) const;
};

struct SqlStringEqual {
	bool operator()(const sql_string_t & str1, const sql_string_t & str2) const;
};

class StringPool {
public:
    static StringPool & instance();
    const sql_string_t & put(sql_string_t && str);
private:
/*
    using pool_t = std::unordered_set<sql_string_t, SqlStringHash, SqlStringEqual>;
    pool_t pool;
*/
    std::vector<sql_string_t> pool;
};
