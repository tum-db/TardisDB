//
// Created by Blum Thomas on 2020-08-13.
//

#include <fstream>
#include <iostream>
#include <sstream>
#include <random>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/TargetSelect.h>
#include <foundations/StringPool.hpp>

#include "codegen/CodeGen.hpp"
#include "foundations/Database.hpp"
#include "queryExecutor/queryExecutor.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include "foundations/version_management.hpp"
#include "sql/SqlType.hpp"
#include "sql/SqlValues.hpp"
#include "utils/general.hpp"

#if LIBXML_AVAILABLE
    #include "wikiParser/WikiParser.hpp"
#endif

#if PERF_AVAILABLE
#include "perfevent/PerfEvent.hpp"
#endif

#include "gflags/gflags.h"

DEFINE_bool(b, false, "isBenchmarking");
DEFINE_string(l, "wikidb", "database");
DEFINE_double(d, 0.5, "distribution");
DEFINE_uint64(r, 1, "runs");
DEFINE_uint64(lowerBound, 1, "lowerBound");
DEFINE_uint64(upperBound, 30303, "upperBound");

static bool ValidateDatabase(const char *flagname, const std::string &value) {
    return value.compare("wikidb") == 0;
}

DEFINE_validator(l, &ValidateDatabase);

void loadWikiDb(Database *db, int lowerBound, int upperBound)
{
    uint64_t loadDuration = 0;

    std::stringstream ssRange;
    ssRange << "_";
    ssRange << lowerBound;
    ssRange << "_";
    ssRange << upperBound;
    std::string pageRangeStr = ssRange.str();


    std::string pageFileName = "page" + pageRangeStr + ".tbl";
    std::string revisionFileName = "revision" + pageRangeStr + ".tbl";
    std::string contentFileName = "content" + pageRangeStr + ".tbl";
    std::string userFileName = "user" + pageRangeStr + ".tbl";

    QueryCompiler::compileAndExecute("CREATE TABLE page ( id INTEGER NOT NULL, title TEXT NOT NULL , userId INTEGER NOT NULL , content TEXT NOT NULL );",*db);
    QueryCompiler::compileAndExecute("CREATE TABLE user ( id INTEGER NOT NULL, name TEXT NOT NULL );",*db);

    std::ifstream streamRevision(revisionFileName);
    if (!streamRevision) { throw std::runtime_error("file not found: tables/revision.tbl"); }
    std::ifstream streamPage(pageFileName);
    if (!streamPage) { throw std::runtime_error("file not found: tables/page.tbl"); }
    std::ifstream streamContent(contentFileName);
    if (!streamContent) { throw std::runtime_error("file not found: tables/content.tbl"); }
    std::ifstream streamUser(userFileName);
    if (!streamUser) { throw std::runtime_error("file not found: tables/user.tbl"); }

    Table *pageTable = db->getTable("page");
    Table *userTable = db->getTable("user");

    std::set<std::string> userIDS = {};
    std::string userRowStr;

    QueryContext userCtx(*db);
    userCtx.executionContext.branchId = 0;
    db->constructBranchLineage(0, userCtx.executionContext);
    while(std::getline(streamUser,userRowStr)) {
        std::vector<std::string> userValues = split(userRowStr,'|');

        if (userIDS.find(userValues[0]) == userIDS.end()) {
            userIDS.insert(userValues[0]);
            std::vector<std::unique_ptr<Native::Sql::Value>> sqlvalues;
            int counter = 0;
            for (auto &columnName : userTable->getColumnNames()) {
                sqlvalues.push_back(Native::Sql::Value::castString(userValues[counter],userTable->getCI(columnName)->type));
                counter++;
            }
            Native::Sql::SqlTuple userTuple(std::move(sqlvalues));
            insert_tuple(userTuple, *userTable, userCtx);
        }
    }

    streamUser.close();
    streamUser = std::ifstream(userFileName);
    if (!streamUser) { throw std::runtime_error("file not found: tables/user.tbl"); }

    std::string skippingPageId = "";

    std::string currentPageId = "";
    std::string currentPageTitle = "";

    std::string revisionRowStr;
    std::string pageRowStr;
    std::string contentRowStr;

    QueryContext firstctx(*db);
    firstctx.executionContext.branchId = 0;
    db->constructBranchLineage(0, firstctx.executionContext);
    while(std::getline(streamRevision, revisionRowStr)) {
        std::vector<std::string> revisionValues = split(revisionRowStr,'|');

        if (std::stoi(revisionValues[2]) < lowerBound) {
            if (skippingPageId.compare("") == 0 || skippingPageId.compare(revisionValues[2]) != 0) {
                skippingPageId = revisionValues[2];
                assert(std::getline(streamPage,pageRowStr));
            }
            assert(std::getline(streamContent,contentRowStr));
            assert(std::getline(streamUser,userRowStr));
            continue;
        }
        if (std::stoi(revisionValues[2]) > upperBound) {
            break;
        }

        if (currentPageId.compare("") == 0 || currentPageId.compare(revisionValues[2]) != 0) {
            currentPageId = revisionValues[2];

            assert(std::getline(streamPage,pageRowStr));
            std::vector<std::string> pageValues = split(pageRowStr,'|');
            assert(pageValues.size() == 2);
            assert(pageValues[0].compare(currentPageId) == 0);

            assert(std::getline(streamUser,userRowStr));
            std::vector<std::string> userValues = split(userRowStr,'|');
            assert(userValues.size() == 2);
            pageValues.push_back(userValues[0]);
            assert(std::getline(streamContent,contentRowStr));
            std::vector<std::string> contentValues = split(contentRowStr,'|');
            assert(contentValues.size() == 2);
            pageValues.push_back(contentValues[1]);

            std::vector<std::unique_ptr<Native::Sql::Value>> sqlvalues;
            int counter = 0;
            for (auto &columnName : pageTable->getColumnNames()) {
                sqlvalues.push_back(Native::Sql::Value::castString(pageValues[counter],pageTable->getCI(columnName)->type));
                counter++;
            }
            Native::Sql::SqlTuple pageTuple(std::move(sqlvalues));
            auto loadStart = std::chrono::high_resolution_clock::now();
            insert_tuple(pageTuple, *pageTable, firstctx);
            loadDuration += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - loadStart).count();
        } else {
            assert(std::getline(streamContent,contentRowStr));
            assert(std::getline(streamUser,userRowStr));
        }
    }

    QueryCompiler::compileAndExecute("CREATE BRANCH branch1 FROM master;",*db);

    streamRevision.close();
    streamPage.close();
    streamContent.close();
    streamUser.close();
    streamRevision = std::ifstream(revisionFileName);
    if (!streamRevision) { throw std::runtime_error("file not found: tables/revision.tbl"); }
    streamPage = std::ifstream(pageFileName);
    if (!streamPage) { throw std::runtime_error("file not found: tables/page.tbl"); }
    streamContent = std::ifstream(contentFileName);
    if (!streamContent) { throw std::runtime_error("file not found: tables/content.tbl"); }
    streamUser = std::ifstream(userFileName);
    if (!streamUser) { throw std::runtime_error("file not found: tables/user.tbl"); }

    currentPageId = "";
    skippingPageId = "";
    std::vector<std::string> currentPageValues;
    std::vector<std::string> lastContentValues;
    std::vector<std::string> lastUserValues;
    tid_t pageTID = 0;
    bool wasFirstRevision = true;

    QueryContext secondctx(*db);
    secondctx.executionContext.branchId = 0;
    db->constructBranchLineage(0, secondctx.executionContext);
    while(std::getline(streamRevision, revisionRowStr)) {
        std::vector<std::string> revisionValues = split(revisionRowStr,'|');

        if (std::stoi(revisionValues[2]) < lowerBound) {
            if (skippingPageId.compare("") == 0 || skippingPageId.compare(revisionValues[2]) != 0) {
                skippingPageId = revisionValues[2];
                assert(std::getline(streamPage,pageRowStr));
            }
            assert(std::getline(streamContent,contentRowStr));
            assert(std::getline(streamUser,userRowStr));
            continue;
        }
        if (std::stoi(revisionValues[2]) > upperBound) {
            break;
        }

        if (currentPageId.compare("") == 0) {
            currentPageId = revisionValues[2];

            assert(std::getline(streamPage,pageRowStr));
            currentPageValues = split(pageRowStr,'|');
            assert(currentPageValues.size() == 2);
            assert(currentPageValues[0].compare(currentPageId) == 0);

            assert(std::getline(streamContent,contentRowStr));
            assert(std::getline(streamUser,userRowStr));
            wasFirstRevision = true;
            continue;
        }

        if (currentPageId.compare(revisionValues[2]) != 0) {
            currentPageId = revisionValues[2];

            assert(std::getline(streamPage,pageRowStr));
            currentPageValues = split(pageRowStr,'|');
            assert(currentPageValues.size() == 2);
            assert(currentPageValues[0].compare(currentPageId) == 0);

            assert(std::getline(streamContent,contentRowStr));
            assert(std::getline(streamUser,userRowStr));

            pageTID++;
            wasFirstRevision = true;
            continue;
        }

        if (!wasFirstRevision && currentPageId.compare(revisionValues[2]) == 0) {
            std::vector<std::string> lastPageValues = currentPageValues;
            lastPageValues.push_back(lastUserValues[0]);
            lastPageValues.push_back(lastContentValues[1]);

            std::vector<std::unique_ptr<Native::Sql::Value>> sqlValues;
            int counter = 0;
            for (auto &columnName : pageTable->getColumnNames()) {
                sqlValues.push_back(Native::Sql::Value::castString(lastPageValues[counter],pageTable->getCI(columnName)->type));
                counter++;
            }
            Native::Sql::SqlTuple pageTuple(std::move(sqlValues));
            auto loadStart = std::chrono::high_resolution_clock::now();
            update_tuple(pageTID,pageTuple,*pageTable,secondctx);
            loadDuration += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - loadStart).count();
        }

        assert(std::getline(streamUser,userRowStr));
        lastUserValues = split(userRowStr,'|');
        assert(lastUserValues.size() == 2);
        assert(std::getline(streamContent,contentRowStr));
        lastContentValues = split(contentRowStr,'|');
        assert(lastContentValues.size() == 2);
        wasFirstRevision = false;
    }

    QueryCompiler::compileAndExecute("CREATE BRANCH branch2 FROM master;",*db);

    streamRevision.close();
    streamPage.close();
    streamContent.close();
    streamUser.close();
    streamRevision = std::ifstream(revisionFileName);
    if (!streamRevision) { throw std::runtime_error("file not found: tables/revision.tbl"); }
    streamPage = std::ifstream(pageFileName);
    if (!streamPage) { throw std::runtime_error("file not found: tables/page.tbl"); }
    streamContent = std::ifstream(contentFileName);
    if (!streamContent) { throw std::runtime_error("file not found: tables/content.tbl"); }
    streamUser = std::ifstream(userFileName);
    if (!streamUser) { throw std::runtime_error("file not found: tables/user.tbl"); }

    currentPageId = "";
    skippingPageId = "";
    pageTID = 0;
    bool isFirstRevisionOfPage = true;

    QueryContext thirdctx(*db);
    thirdctx.executionContext.branchId = 0;
    db->constructBranchLineage(0, thirdctx.executionContext);
    while(std::getline(streamRevision, revisionRowStr)) {
        std::vector<std::string> revisionValues = split(revisionRowStr,'|');

        if (std::stoi(revisionValues[2]) < lowerBound) {
            if (skippingPageId.compare("") == 0 || skippingPageId.compare(revisionValues[2]) != 0) {
                skippingPageId = revisionValues[2];
                assert(std::getline(streamPage,pageRowStr));
            }
            assert(std::getline(streamContent,contentRowStr));
            assert(std::getline(streamUser,userRowStr));
            continue;
        }
        if (std::stoi(revisionValues[2]) > upperBound) {
            break;
        }


        if (currentPageId.compare("") == 0) {
            currentPageId = revisionValues[2];
            isFirstRevisionOfPage = true;
            continue;
        }

        if (!isFirstRevisionOfPage && currentPageId.compare(revisionValues[2]) != 0) {
            assert(std::getline(streamPage,pageRowStr));
            std::vector<std::string> pageValues = split(pageRowStr,'|');
            assert(pageValues.size() == 2);
            assert(pageValues[0].compare(currentPageId) == 0);

            assert(std::getline(streamUser,userRowStr));
            std::vector<std::string> userValues = split(userRowStr,'|');
            assert(userValues.size() == 2);
            pageValues.push_back(userValues[0]);
            assert(std::getline(streamContent,contentRowStr));
            std::vector<std::string> contentValues = split(contentRowStr,'|');
            assert(contentValues.size() == 2);
            pageValues.push_back(contentValues[1]);

            std::vector<std::unique_ptr<Native::Sql::Value>> sqlvalues;
            int counter = 0;
            for (auto &columnName : pageTable->getColumnNames()) {
                sqlvalues.push_back(Native::Sql::Value::castString(pageValues[counter],pageTable->getCI(columnName)->type));
                counter++;
            }
            Native::Sql::SqlTuple pageTuple(std::move(sqlvalues));
            auto loadStart = std::chrono::high_resolution_clock::now();
            update_tuple(pageTID,pageTuple,*pageTable,thirdctx);
            loadDuration += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - loadStart).count();

            pageTID++;
            isFirstRevisionOfPage = true;
            currentPageId = revisionValues[2];
            continue;
        }

        if (currentPageId.compare(revisionValues[2]) != 0) {
            assert(std::getline(streamPage,pageRowStr));
            pageTID++;
            isFirstRevisionOfPage = true;
            currentPageId = revisionValues[2];
        } else {
            isFirstRevisionOfPage = false;
        }

        assert(std::getline(streamUser,userRowStr));
        assert(std::getline(streamContent,contentRowStr));
    }

    if (!isFirstRevisionOfPage) {
        assert(std::getline(streamPage,pageRowStr));
        std::vector<std::string> pageValues = split(pageRowStr,'|');
        assert(pageValues.size() == 2);
        assert(pageValues[0].compare(currentPageId) == 0);

        assert(std::getline(streamUser,userRowStr));
        std::vector<std::string> userValues = split(userRowStr,'|');
        assert(userValues.size() == 2);
        pageValues.push_back(userValues[0]);
        assert(std::getline(streamContent,contentRowStr));
        std::vector<std::string> contentValues = split(contentRowStr,'|');
        assert(contentValues.size() == 2);
        pageValues.push_back(contentValues[1]);

        std::vector<std::unique_ptr<Native::Sql::Value>> sqlvalues;
        int counter = 0;
        for (auto &columnName : pageTable->getColumnNames()) {
            sqlvalues.push_back(Native::Sql::Value::castString(pageValues[counter],pageTable->getCI(columnName)->type));
            counter++;
        }
        Native::Sql::SqlTuple pageTuple(std::move(sqlvalues));
        auto loadStart = std::chrono::high_resolution_clock::now();
        update_tuple(pageTID,pageTuple,*pageTable,thirdctx);
        loadDuration += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - loadStart).count();
    }

    streamRevision.close();
    streamPage.close();
    streamContent.close();
    streamUser.close();

    std::cout << "Table Sizes:\n";
    std::cout << "Page:\t" << db->getTable("page")->size() << "\n";
    std::cout << "User:\t" << db->getTable("user")->size() << "\n";
    std::cout << "LoadDuration:\t" << std::fixed << loadDuration / 1000 << std::endl;
}

void benchmarkQuery(std::string query, Database &db, unsigned runs) {
    std::vector<QueryCompiler::BenchmarkResult> results;

#ifdef PERF_AVAILABLE
    std::string header;
    std::string data;
    BenchmarkParameters params;

    {
        PerfEventBlock e(runs, params, false);
#endif

        for (int i = 0; i < runs; i++) {
            results.push_back(QueryCompiler::compileAndBenchmark(query, db));
        }

#ifdef PERF_AVAILABLE
    }
#endif

    double parsingTime = 0;
    double analsingTime = 0;
    double translationTime = 0;
    double compileTime = 0;
    double executionTime = 0;
    for (auto &result : results) {
        parsingTime += result.parsingTime;
        analsingTime += result.analysingTime;
        translationTime += result.translationTime;
        compileTime += result.llvmCompilationTime;
        executionTime += result.executionTime;
    }
    double sum = parsingTime + analsingTime + translationTime + compileTime + executionTime;
    std::cout << std::fixed << (parsingTime / runs) << " , " << (analsingTime / runs) << " , " << (translationTime / runs) << " , " << (compileTime / runs) << " , " << (executionTime / runs) << " , " << (sum / runs) << std::endl;
}

void prompt(Database &database, unsigned runs)
{
    while (true) {
        try {
            fflush(stdout);

            std::string input = readline();
            if (input == "quit\n") {
                break;
            }

            benchmarkQuery(input,database,runs);
        } catch (const std::exception & e) {
            fprintf(stderr, "Exception: %s\n", e.what());
        }
    }
}

int main(int argc, char * argv[]) {
    gflags::SetUsageMessage("semanticalBench [-b] [-l <Database Name>] [-d <Master Share>] [-r <Runs per Statement>]");
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    std::unique_ptr<Database> db = std::make_unique<Database>();

    loadWikiDb(db.get(),FLAGS_lowerBound,FLAGS_upperBound);

    prompt(*db,FLAGS_r);

    llvm::llvm_shutdown();
}
