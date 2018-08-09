//
// Created by josef on 19.12.16.
//

#include "StaticHashtable.hpp"

#include <cstdlib>
#include <cmath>

#include <llvm/IR/TypeBuilder.h>

#include "CodeGen.hpp"

//-----------------------------------------------------------------------------
// StaticHashtable

StaticHashtable::StaticHashtable(StaticHashtable::Node * first, size_t len)
{
    loadFactor = 0.75f; // usually a good default

//    printf("StaticHashtable ctor\n");
    double sizeHint = static_cast<double>(len) / loadFactor;
    double p = std::ceil( std::log2(sizeHint) ); // find p such that: 2^p >= sizeHint
    size_t n = static_cast<size_t>(p);
    tableSize = 1ul << n;
    moduloMask = tableSize - 1;

    table = new Node *[tableSize](); // initialised to zero

    build(first);
}

StaticHashtable::~StaticHashtable()
{
//    printf("StaticHashtable dtor %p\n", this);
    delete[] table;
}

StaticHashtable::Node * StaticHashtable::lookup(hash_code_t h)
{
    size_t i = hash(h);

    Node * node = table[i];
    while (node != nullptr) {
        if (node->hashCode == h) {
            return node;
        }

        node = node->next;
    }

    return nullptr;
}

StaticHashtable::Node * StaticHashtable::next(hash_code_t h, StaticHashtable::Node * start)
{
    Node * node = start;
    while (node != nullptr) {
        if (node->hashCode == h) {
            return node;
        }

        node = node->next;
    }

    return nullptr;
}

void StaticHashtable::build(StaticHashtable::Node * first)
{
    Node * current = first;
    while (current != nullptr) {
//        printf("insert node with hash: %lu\n", current->hashCode);
//        printf("next: %p\n", current->next);

        size_t i = hash(current->hashCode);

        // remember the next node
        Node * nextNode = current->next;

        // insert node into bucket
        current->next = table[i];
        table[i] = current;

        current = nextNode;
    }
}

// wrapper functions
StaticHashtable * staticHashtableCreate(StaticHashtable::Node * first, size_t len)
{
    return new StaticHashtable(first, len);
}

void staticHashtableFree(StaticHashtable * hashtable)
{
    delete hashtable;
}

// generator functions
cg_voidptr_t genStaticHashtableCreateCall(cg_voidptr_t first, cg_size_t len)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void * (void *, size_t), false>::get(context);
    llvm::CallInst * result = codeGen.CreateCall(&staticHashtableCreate, funcTy, {first, len});

    return cg_voidptr_t( llvm::cast<llvm::Value>(result) );
}

void genStaticHashtableFreeCall(cg_voidptr_t hashtable)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (void *), false>::get(context);
    codeGen.CreateCall(&staticHashtableFree, funcTy, hashtable.getValue());
}

static StaticHashtable::Node * staticHashtableLookup(StaticHashtable * hashtable, hash_code_t h)
{
    return hashtable->lookup(h);
}

cg_voidptr_t genStaticHashtableLookupCall(cg_voidptr_t hashtable, cg_hash_t h)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void * (void *, hash_code_t), false>::get(context);
    llvm::CallInst * result = codeGen.CreateCall(&staticHashtableLookup, funcTy, {hashtable, h});

    return cg_voidptr_t( llvm::cast<llvm::Value>(result) );
}

void genStaticHashtableIter(cg_voidptr_t hashtable, cg_hash_t h, std::function<void(cg_voidptr_t)> elementHandler)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & funcGen = codeGen.getCurrentFunctionGen();

    cg_voidptr_t first = genStaticHashtableLookupCall(hashtable, h);
    llvm::Type * i8Ptr = llvm::PointerType::getInt8PtrTy(codeGen.getLLVMContext() );

//    Functions::genPrintfCall("lookup result: %p\n", first);

    // iterate over the all members of the bucket
    LoopGen bucketIter( funcGen, !first.isNullPtr(), {{"current", first}} );
    cg_voidptr_t current(bucketIter.getLoopVar(0));
    {
        LoopBodyGen bodyGen(bucketIter);

        // the second field of the "Node"-struct contains the hashcode
        cg_voidptr_t hp = current + sizeof(void *);
//        Functions::genPrintfCall("hashcode ptr: %p\n", hp);

        cg_hash_t currentHash( codeGen->CreateLoad( cg_hash_t::getType(), hp ) );
//        Functions::genPrintfCall("hashcode: %lu\n", currentHash);

        IfGen compareHash( funcGen, h == currentHash );
        {
            // elementHandler is supposed to generate code that handles the current match
            elementHandler(current);
        }
    }
    // the first field of the struct represents the "next"-pointer
    cg_voidptr_t next( codeGen->CreateLoad(i8Ptr, current) );
    bucketIter.loopDone( !next.isNullPtr(), {next} );
}
