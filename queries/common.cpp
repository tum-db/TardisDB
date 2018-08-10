#include "queries/common.hpp"

#include <random>
#include <limits>

#include "codegen/CodeGen.hpp"
#include "foundations/Database.hpp"
#include "foundations/exceptions.hpp"
#include "foundations/loader.hpp"
#include "sql/SqlValues.hpp"

std::unique_ptr<Database> load_tpch_db()
{
    std::unique_ptr<Database> db;

    try {
        printf("loading data into tables...\n");
        db = loadDatabase();
        printf("done\n");
    } catch (const std::exception & e) {
        fprintf(stderr, "%s\n", e.what());
        throw;
    }

    return db;
}

#ifdef USE_INTERNAL_NULL_INDICATOR

static void storeNull(void * ptr, Sql::SqlType type)
{
    using namespace Sql;

    // there is currently no better way to achieve this (without generating code)
    switch (type.typeID) {
        case SqlType::TypeID::BoolID:
            throw NotImplementedException();
        case SqlType::TypeID::IntegerID: {
            Integer::value_type nullIndicator = std::numeric_limits<typename Integer::value_type>::min();
            Integer::value_type * valuePtr = reinterpret_cast<Integer::value_type *>(ptr);
            *valuePtr = nullIndicator;
            break;
        }
        case SqlType::TypeID::NumericID: {
            Numeric::value_type nullIndicator = std::numeric_limits<typename Numeric::value_type>::min();
            Numeric::value_type * valuePtr = reinterpret_cast<Numeric::value_type *>(ptr);
            *valuePtr = nullIndicator;
            break;
        }
        case SqlType::TypeID::VarcharID:
            throw NotImplementedException();
        case SqlType::TypeID::CharID:
            throw NotImplementedException();
        case SqlType::TypeID::DateID:
        case SqlType::TypeID::TimestampID:
            throw NotImplementedException();
        default:
            unreachable();
    }
}

#endif

static void setNullValues(Database & db)
{
    const double nullPercentage = 0.0;
    const int seed = 597;

    std::mt19937 gen(seed);
    std::uniform_real_distribution<> dis(0.0, 100.0);

    Table * lineitem = db.getTable("lineitem");
    assert(lineitem != nullptr);

    // only used for external null indicators
    auto & nullTable = lineitem->getNullIndicatorTable();
    for (tid_t tid = 0, limit = lineitem->size(); tid < limit; ++tid) {
        for (const std::string & columnName : lineitem->getColumnNames()) {
            ci_p_t ci = lineitem->getCI(columnName);

            if (ci->type.nullable) {
                bool isNull = (dis(gen) <= nullPercentage);

                // set either the a null bit or the internal indicator value
#ifndef USE_INTERNAL_NULL_INDICATOR
                nullTable.set(tid, ci->nullColumnIndex, isNull);
#else
                if (isNull) {
                    Vector & column = *ci->column;
                    storeNull( column.at(tid), ci->type );
                }
#endif
            }
        }
    }
}

std::unique_ptr<Database> load_tpch_null_db()
{
    auto db = load_tpch_db();

    printf("setting values to null...\n");
    setNullValues(*db);

    return db;
}
