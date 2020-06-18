//
// Created by Blum Thomas on 2020-06-18.
//

#ifndef PROTODB_QUERYCOMPILER_HPP
#define PROTODB_QUERYCOMPILER_HPP

#include <string>

#include "foundations/Database.hpp"

namespace QueryCompiler {

    void compileAndExecute(const std::string & query, Database &db);

}

#endif //PROTODB_QUERYCOMPILER_HPP
