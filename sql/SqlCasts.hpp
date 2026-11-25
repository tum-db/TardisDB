// The casts between the SQL types: every type holds a table of the types it converts to, in which
// context and by which method (as pg_cast of PostgreSQL)

#pragma once

#include <vector>

#include "sql/SqlType.hpp"
#include "sql/SqlValues.hpp"

namespace Sql {
namespace Casts {

/// Where a cast may happen without being written: implicitly in any expression (a comparison, the
/// arithmetic), on an assignment (SET c = ...), or only as CAST(... AS type)/::type. Each context
/// allows the casts of the ones before it.
enum class Context { Implicit, Assignment, Explicit };

/// Converts a value (never NULL) to the target type (not nullable); the context decides what happens to
/// a string that is too long: an explicit cast truncates it, any other sets the overflow flag
using cast_method_t = value_op_t (*)(const Value & value, SqlType target, Context context);

struct Cast {
    SqlType::TypeID target;
    Context context;
    cast_method_t method;
};

/// The casts of a source type to other types (and to the same type with other parameters)
const std::vector<Cast> & castTable(SqlType::TypeID source);

/// The cast of the table of \p source to \p target, or nullptr
const Cast * findCast(SqlType source, SqlType target);

/// The context a cast needs at least: that of the table, an assignment if it narrows (a varchar or a
/// numeric to fewer characters or digits). \returns false if there is no cast
bool castContext(SqlType source, SqlType target, Context & context);

/// Whether \p source converts to \p target in \p context (a type converts to itself everywhere)
bool canCast(SqlType source, SqlType target, Context context);

/// The type two values are compared or computed in, to which both cast implicitly (not nullable):
/// numbers the wider one (integer, bigint, numeric, double precision), two numerics the digits of both;
/// strings text if one is text, else varchar of the longer length (a character without its trailing
/// spaces).
/// \returns false if there is none
bool commonType(SqlType a, SqlType b, SqlType & result);

/// The value in the target type (nullable as the value); NULL stays NULL. Throws if there is no cast.
value_op_t cast(const Value & value, SqlType target, Context context);

/// "integer", "character varying(12)", ...: the names of PostgreSQL, for messages
std::string typeName(SqlType type);

} // end namespace Casts
} // end namespace Sql
