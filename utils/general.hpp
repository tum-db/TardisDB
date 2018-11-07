#pragma once

#include <set>
#include <string>
#include <vector>

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

std::vector<std::string> split(const std::string & str, char delim);

std::string readline();
