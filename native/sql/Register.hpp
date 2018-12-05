#pragma once

#include "native/sql/SqlValues.hpp"
#include "sql/SqlType.hpp"

namespace Native {
namespace Sql {

class Register {
    Sql::SqlType type;
    bool is_null;
    Native::Sql::Bool bool_value;
    Native::Sql::Date date_value;
    Native::Sql::Integer integer_value;
    Native::Sql::Numeric numeric_value;
    Native::Sql::Text text_value;
    Native::Sql::Timestamp timestamp_value;

    void load_from(const void * ptr);

    Native::Sql::Value & get_value();
};

} // end namespace Sql
} // end namespace Native
