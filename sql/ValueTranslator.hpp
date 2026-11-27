//
// Created by Blum Thomas on 2020-06-16.
//

#ifndef PROTODB_VALUETRANSLATOR_HPP
#define PROTODB_VALUETRANSLATOR_HPP

#include "runtime/SqlValues.hpp"
#include "sql/SqlValues.hpp"

class ValueTranslator {
public:
    static std::unique_ptr<Native::Sql::Value> sqlValueToNativeSqlValue(Sql::Value *original);
private:
    static void storeStringInTextFormat(Native::Sql::Varchar* destination, char* source, size_t length);
    static void storeCharInTextFormat(Native::Sql::Varchar* destination, uint8_t character);
};


#endif //PROTODB_VALUETRANSLATOR_HPP
