//
// Created by Blum Thomas on 2020-06-16.
//

#ifndef PROTODB_VALUETRANSLATOR_HPP
#define PROTODB_VALUETRANSLATOR_HPP

#include "native/sql/SqlValues.hpp"
#include "sql/SqlValues.hpp"

class ValueTranslator {
public:
    static std::unique_ptr<Native::Sql::Value> sqlValueToNativeSqlValue(Sql::Value *original);
private:
    static void storeStringInTextFormat(Native::Sql::Varchar* destination, char* source, size_t length);
};


#endif //PROTODB_VALUETRANSLATOR_HPP
