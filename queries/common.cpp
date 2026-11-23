#include "queries/common.hpp"

#include <random>
#include <limits>

#include "codegen/CodeGen.hpp"
#include "foundations/Database.hpp"
#include "foundations/exceptions.hpp"
#include "foundations/loader.hpp"
#include "foundations/version_management.hpp"
#include "sql/SqlValues.hpp"

std::unique_ptr<Database> load_tables()
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

std::unique_ptr<Database> load_tpch_null_db()
{
    // the columns hold no nulls (formerly a share of 0% was set to null)
    return load_tables();
}
