#include <cassert>
#include "common.hpp"
#include "thirdparty/profile.hpp"

int main(int argc, char** argv) {
    assert(argc > 1);
    Database db;
    load_tables(db, argv[1]);

    PerfEvents e;
    e.timeAndProfile("tpch1", db.lineitem.l_linestatus.size(),
        [&]() {
            query_1(db);
        },
        10, {{"approach", "cpu_only"}, {"threads", std::to_string(1)}});

//    query_1(db);
//    query_14(db);
    return 0;
}
