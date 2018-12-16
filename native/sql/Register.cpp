#include "foundations/exceptions.hpp"
#include "native/sql/Register.hpp"
#include "utils/general.hpp"

namespace Native {
namespace Sql {

void Register::load_from(const void * ptr) {
    if (is_null) {
        throw NotImplementedException();
    }

    switch (type.typeID) {
        case Sql::SqlType::TypeID::UnknownID:
            assert(false);
        case Sql::SqlType::TypeID::BoolID:
            new (&bool_value) Native::Sql::Bool(ptr);
            break;
        case Sql::SqlType::TypeID::DateID:
            new (&date_value) Native::Sql::Date(ptr);
            break;
        case Sql::SqlType::TypeID::IntegerID:
            new (&integer_value) Native::Sql::Integer(ptr);
            break;
//        case SqlType::TypeID::VarcharID:
//        case SqlType::TypeID::CharID:
        case Sql::SqlType::TypeID::NumericID:
            new (&numeric_value) Native::Sql::Numeric(ptr, type);
            break;
        case Sql::SqlType::TypeID::TextID:
            new (&text_value) Native::Sql::Text(ptr);
            break;
        case Sql::SqlType::TypeID::TimestampID:
            new (&timestamp_value) Native::Sql::Timestamp(ptr);
            break;
        default:
            unreachable();
    }
}

Native::Sql::Value & Register::get_value() {
    if (is_null) {
        assert(false);
    }

    switch (type.typeID) {
        case Sql::SqlType::TypeID::UnknownID:
            assert(false);
        case Sql::SqlType::TypeID::BoolID:
            return bool_value;
        case Sql::SqlType::TypeID::DateID:
            return date_value;
        case Sql::SqlType::TypeID::IntegerID:
            return integer_value;
//        case SqlType::TypeID::VarcharID:
//        case SqlType::TypeID::CharID:
        case Sql::SqlType::TypeID::NumericID:
            return numeric_value;
        case Sql::SqlType::TypeID::TextID:
            return text_value;
        case Sql::SqlType::TypeID::TimestampID:
            return timestamp_value;
        default:
            unreachable();
    }
}

} // end namespace Sql
} // end namespace Native
