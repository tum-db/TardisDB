#pragma once

#include "native/sql/SqlValues.hpp"
#include "sql/SqlType.hpp"

namespace Native {
namespace Sql {

class Register {
public:
    Sql::SqlType type;
    bool is_null;
    Native::Sql::Bool bool_value;
    Native::Sql::Date date_value;
    Native::Sql::Integer integer_value;
    Native::Sql::Numeric numeric_value;
    Native::Sql::Text text_value;
    Native::Sql::Timestamp timestamp_value;

//    Register(const Register & other);

    void load_from(const void * ptr);

    Native::Sql::Value & get_value();
};

template<typename T>
struct TypedRegister {
    T sql_value;
    inline void load_from(const void * ptr) {
        auto typed_ptr = static_cast<typename T::value_type const *>(ptr);
        sql_value.value = *typed_ptr;
    }
};

template<>
struct TypedRegister<Sql::Text> {
    Native::Sql::Text sql_value;
    inline void load_from(const void * ptr) {
        const uintptr_t * src_array = reinterpret_cast<const uintptr_t *>(ptr);
        sql_value.value[0] = src_array[0];
        sql_value.value[1] = src_array[1];
    }
};

template<uint8_t length, uint8_t scale>
struct NumericRegister {
    Native::Sql::Numeric sql_value;
    NumericRegister()
        : sql_value(::Sql::getNumericTy(length, scale), 0ull)
    { }
    inline void load_from(const void * ptr) {
        auto typed_ptr = static_cast<typename Native::Sql::Numeric::value_type const *>(ptr);
        sql_value.value = *typed_ptr;
    }
};

} // end namespace Sql
} // end namespace Native
