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
// TODO specialize for Text and Numeric

} // end namespace Sql
} // end namespace Native
