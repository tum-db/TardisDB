//
// Created by josef on 19.12.16.
//

#include "MemoryPool.hpp"

#include <cstdlib>
#include <cmath>

#include <llvm/IR/TypeBuilder.h>

#include "CodeGen.hpp"
#include "utils.hpp"

//-----------------------------------------------------------------------------
// MemoryPool

MemoryPool::MemoryPool() :
        MemoryPool(initialBlockSize)
{ }

MemoryPool::MemoryPool(size_t sizeHint) :
        nextBlockSize(0)
{
//    printf("MemoryPool ctor\n");
    // allocating one empty block before any call to malloc() actually saves one performance critical branch
    createNewBlock();
    nextBlockSize = sizeHint;
}

MemoryPool::~MemoryPool()
{
//    printf("MemoryPool dtor %p\n", this);

    for (auto & block : blocks) {
        std::free(block.startAddr);
    }
}

void * MemoryPool::malloc(size_t size)
{
    assert(size > 0);

    auto * currentBlock = &blocks.back();
    ptrdiff_t used = static_cast<char *>(currentBlock->currentAddr) - static_cast<char *>(currentBlock->startAddr);
    size_t remaining = currentBlock->size - used;

    // this small optimization gives the compiler a hint which branch will be most likely
    if (unlikely(remaining < size)) {
        createNewBlock(size);
        currentBlock = &blocks.back();
    }

    void * addr = currentBlock->currentAddr;
    currentBlock->currentAddr = static_cast<char *>(currentBlock->currentAddr) + size;

    return addr;
}

void MemoryPool::createNewBlock(size_t minimum)
{
    size_t blockSize = std::max(nextBlockSize, minimum);

    // we don't need continuous memory addresses
    // http://www.iso-9899.info/wiki/Why_not_realloc
    void * mem = std::calloc(blockSize, 1);
    if (unlikely(mem == nullptr)) {
        throw std::runtime_error("allocation failed");
    }

    blocks.emplace_back(mem, mem, blockSize);
    nextBlockSize = nextBlockSize << 1;
}

// wrapper functions
MemoryPool * memoryPoolCreate()
{
    return new MemoryPool();
}

void * memoryPoolMalloc(MemoryPool * memoryPool, size_t n)
{
    return memoryPool->malloc(n);
}

void memoryPoolFree(MemoryPool * memoryPool)
{
    delete memoryPool;
}

// generator functions
cg_voidptr_t genMemoryPoolCreateCall()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void * (), false>::get(context);
    llvm::CallInst * result = codeGen.CreateCall(&memoryPoolCreate, funcTy);

    return cg_voidptr_t( llvm::cast<llvm::Value>(result) );
}

cg_voidptr_t genMemoryPoolMallocCall(cg_voidptr_t memoryPool, cg_size_t n)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void * (void *, size_t), false>::get(context);
    llvm::CallInst * result = codeGen.CreateCall(&memoryPoolMalloc, funcTy, {memoryPool, n});

    return cg_voidptr_t( llvm::cast<llvm::Value>(result) );
}

void genMemoryPoolFreeCall(cg_voidptr_t memoryPool)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void  (void *), false>::get(context);
    codeGen.CreateCall(&memoryPoolFree, funcTy, memoryPool.getValue());
}
