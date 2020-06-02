#include <llvm/ADT/STLExtras.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/TargetSelect.h>

#include "foundations/Database.hpp"
#include "query_compiler/compiler.hpp"
#include "utils/general.hpp"

void prompt()
{
    std::unique_ptr<Database> currentdb = std::make_unique<Database>();
    while (true) {
        try {
            printf(">>> ");
            fflush(stdout);

            std::string input = readline();
            if (input == "quit\n") {
                break;
            }

            QueryCompiler::compileAndExecute(input,*currentdb);
        } catch (const std::exception & e) {
            fprintf(stderr, "Exception: %s\n", e.what());
        }
    }
}

int main(int argc, char * argv[])
{
    // necessary for the LLVM JIT compiler
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    prompt();

    llvm::llvm_shutdown();

    return 0;
}