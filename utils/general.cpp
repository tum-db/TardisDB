#include <sstream>
#include <cstdio>
#include <cstdlib>

#include "utils/general.hpp"

std::vector<std::string> split(const std::string & str, char delim)
{
    std::vector<std::string> result;
    std::stringstream s(str);
    std::string part;

    // getline() works with arbitrary delimiters
    while (std::getline(s, part, delim)) {
        result.emplace_back(part);
    }

    return result;
}

std::string readline()
{
    char * line = nullptr;
    size_t len = 0;
    getline(&line, &len, stdin);

    std::string result(line);
    std::free(line);

    return result;
}
