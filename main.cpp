
#include <chrono>

#include <llvm/ADT/STLExtras.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/TargetSelect.h>

#include "codegen/CodeGen.hpp"
#include "foundations/loader.hpp"
#include "foundations/exceptions.hpp"
#include "tests/tests.hpp"
#include "queries/queries.hpp"
#include "query_compiler/queryparser.hpp"
#include "query_compiler/compiler.hpp"
#include "utils/general.hpp"

using namespace llvm;

void usage(const char * name)
{
    printf("Usage:\n"
           "\t%s\n"
           "\t%s -q query\n"
           "\t%s -b query runs\n"
           "\t%s -t test\n",
           name, name, name, name
    );
}

void prompt()
{
    std::unique_ptr<Database> currentdb = loadDatabase();
    while (true) {
        try {
            printf(">>> ");
            fflush(stdout);

            std::string input = readline();
            if (input == "quit\n") {
                break;
            }

            QueryCompiler::compileAndExecute(input,*currentdb);
        } catch (const QueryParser::parser_error & e) {
            fprintf(stderr, "Failed to parse query, reason: %s\n", e.what());
        } catch (const std::exception & e) {
            fprintf(stderr, "Exception: %s\n", e.what());
        }
    }
}

int main(int argc, char * argv[])
{
    // necessary for the LLVM JIT compiler
    InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    if (argc == 1) {
        prompt();
    } else if (argc == 3) {
        if (std::strcmp(argv[1], "-q") == 0) {
            runNamedQuery(argv[2]);
        } else if(std::strcmp(argv[1], "-t") == 0) {
            runNamedTest(argv[2]);
        } else {
            usage(argv[0]);
        }
    } else if (argc == 4) {
        if (std::strcmp(argv[1], "-b") == 0) {
            unsigned runs = atoi(argv[3]);
            benchmarkNamedQuery(argv[2], runs);
        } else {
            usage(argv[0]);
        }
    } else {
        usage(argv[0]);
    }

    llvm_shutdown();
    return 0;
}
