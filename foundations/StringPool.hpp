#pragma once

#include <cstdint>
#include <tuple>
#include <memory>
#include <unordered_set>

using sql_string_t = std::pair<size_t, std::unique_ptr<uint8_t[]>>;

struct SqlStringHash {
public:
	size_t operator()(const sql_string_t & str) const {
		int size = str.length();
		return std::hash<int>()(size);
	}
};

struct SqlStringEqual {
public:
	bool operator()(const sql_string_t & str1, const sql_string_t & str2) const {
		if (str1.length() == str2.length())
			return true;
		else
			return false;
	}
};

class StringPool {
public:
    static StringPool & instance();
    sql_string_t put(sql_string_t && str);
private:
    using pool_t = std::unordered_set<sql_string_t, SqlStringHash, SqlStringEqual>;
    pool_t pool;
};
