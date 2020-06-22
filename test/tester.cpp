#include <gtest/gtest.h>

#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/TargetSelect.h>

int main(int argc, char *argv[]) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    testing::InitGoogleTest(&argc, argv);

    int result = RUN_ALL_TESTS();

    llvm::llvm_shutdown();

    return result;
}
