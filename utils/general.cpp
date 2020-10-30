#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>

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

std::string getFunctionName(void* funcPtr) {
    Dl_info info;
    if (dladdr(funcPtr, &info) && info.dli_sname) {
        return info.dli_sname;
    }
    return std::string("func_") + std::to_string(reinterpret_cast<intptr_t>(funcPtr));
}
