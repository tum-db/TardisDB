
#ifndef MEMORYPOOL_H
#define MEMORYPOOL_H

#include <vector>

#include <llvm/IR/Value.h>

#include "codegen/CodeGen.hpp"

//-----------------------------------------------------------------------------
// MemoryPool

/// Simple block allocator
class MemoryPool {
public:
    static const size_t initialBlockSize = 4092;

    MemoryPool();
    MemoryPool(size_t sizeHint);
    ~MemoryPool();

    /// \brief Allocates size bytes and returns a pointer to the allocated memory.
    /// The memory is set to zero.
    void * malloc(size_t size);

private:

    typedef struct Block {
        void * startAddr;
        void * currentAddr;
        size_t size;

        Block(void * startAddr, void * currentAddr, size_t size) :
                startAddr(startAddr), currentAddr(currentAddr), size(size)
        { }
    } block_t;

    void createNewBlock(size_t minimum = 0);

    size_t nextBlockSize;

    std::vector<block_t> blocks;
};

// wrapper functions
extern "C" {
MemoryPool * memoryPoolCreate();
void * memoryPoolMalloc(MemoryPool * memoryPool, size_t n);
void memoryPoolFree(MemoryPool * memoryPool);
}

// generator functions
cg_voidptr_t genMemoryPoolCreateCall();
cg_voidptr_t genMemoryPoolMallocCall(cg_voidptr_t memoryPool, cg_size_t n);
void genMemoryPoolFreeCall(cg_voidptr_t memoryPool);

#endif // MEMORYPOOL_H
