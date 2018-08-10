
#include "Hashtable.hpp"

#include <cstdlib>
#include <cmath>

#include <llvm/IR/TypeBuilder.h>

#include "codegen/CodeGen.hpp"
#include "foundations/utils.hpp"

//-----------------------------------------------------------------------------
// Hashtable

Hashtable::Hashtable() :
        Hashtable(defaultSize)
{ }

Hashtable::Hashtable(size_t sizeHint)
{
    double p = std::ceil( std::log2(sizeHint) ); // find p such that: 2^p >= sizeHint
    size_t n = static_cast<size_t>(p);
    _tableSize = 1ul << n;
    _moduloMask = _tableSize - 1;
    _uniqueCountThreshold = static_cast<size_t>(_loadFactorThreshold*_tableSize);

    _table = new Node *[_tableSize](); // initialised to zero
}

Hashtable::Hashtable(Hashtable::Node * first, size_t len)
{
    double sizeHint = static_cast<double>(len) / _loadFactorThreshold;
    double p = std::ceil( std::log2(sizeHint) ); // find p such that: 2^p >= sizeHint
    size_t n = static_cast<size_t>(p);
    _tableSize = 1ul << n;
    _moduloMask = _tableSize - 1;
    _uniqueCountThreshold = static_cast<size_t>(_loadFactorThreshold*_tableSize);

    _table = new Node *[_tableSize](); // initialised to zero

    build(first);
}

Hashtable::~Hashtable()
{
//    printf("Hashtable dtor %p\n", this);
    delete[] _table;
}

Hashtable::Node * Hashtable::first()
{
    for (size_t i = 0; i < _tableSize; ++i) {
        Node * node = _table[i];
        if (node != nullptr) {
            return node;
        }
    }

    return nullptr;
}

Hashtable::Node * Hashtable::lookup(hash_code_t h)
{
    size_t i = hash(h);

    Node * node = _table[i];
    while (node != nullptr) {
        if (node->hashCode == h) {
//            printf("Hashtable::lookup(h: %lu) -> %p\n", h, node);
            return node;
        }

        node = node->next;
    }
//    printf("Hashtable::lookup(h: %lu) -> %p)\n", h, nullptr);
    return nullptr;
}

Hashtable::Node * Hashtable::next(Hashtable::Node * current)
{
    Node * nextNode = current->next;
    if (nextNode != nullptr) {
        return nextNode;
    }

    // start the search in the following bucket
    size_t i = hash(current->hashCode) + 1;
    for (; i < _tableSize; ++i) {
        Node * node = _table[i];
        if (node != nullptr) {
            return node;
        }
    }

    return nullptr;
}

Hashtable::Node * Hashtable::next(Hashtable::Node * current, hash_code_t h)
{
    assert(current->hashCode == h);

    Node * next = current->next;
    if (next != nullptr && next->hashCode == h) {
        return next;
    } else {
        return nullptr;
    }
}

void Hashtable::build(Hashtable::Node * first)
{
    Node * current = first;
    while (current != nullptr) {
//        printf("insert node with hash: %lu\n", current->hashCode);
//        printf("next: %p\n", current->next);

        // remember the next node
        Node * nextNode = current->next;
        insert(current);
        current = nextNode;
    }
}

Hashtable::Node * Hashtable::insert(hash_code_t h, size_t size)
{
    Node * newNode = static_cast<Node *>( _memoryPool.malloc(sizeof(Node) + size) );
//    printf("Hashtable::insert(h: %lu, size: %lu) -> %p\n", h, size, newNode);
    newNode->hashCode = h;
    insert(newNode);
    return newNode;
}

void Hashtable::insert(Node * node)
{
    assert(node != nullptr);

    hash_code_t nodeHash = node->hashCode;

    size_t i = hash(nodeHash);
    Node ** dest = &_table[i];
    Node * current = _table[i];

    _uniqueCount += 1;

    // lookup insert place
    while (current != nullptr) {
        if (current->hashCode == nodeHash) {
            _uniqueCount -= 1; // there is at least one node with the same hash
            break;
        } else {
            dest = &current->next;
            current = current->next;
        }
    }

    // insert node into bucket
    *dest = node;
    node->next = current;

    if (unlikely(_uniqueCount >= _uniqueCountThreshold)) {
        rehash();
    }
}

void Hashtable::rehash()
{
    // new table size and mask
    size_t oldTableSize = _tableSize;
    _tableSize <<= 1ul;
    _moduloMask = _tableSize - 1;
    _uniqueCountThreshold = static_cast<size_t>(_loadFactorThreshold*_tableSize);

    // create a new table
    Node ** oldTable = _table;
    _table = new Node *[_tableSize]();
    _uniqueCount = 0;

    // rebuild the hash table
    for (size_t i = 0; i < oldTableSize; ++i) {
        Node * current = oldTable[i];
        while (current != nullptr) {
            Node * next = current->next;
            insert(current);
            current = next;
        }
    }

    delete[] oldTable;
}

// wrapper functions
static Hashtable * hashtableCreate()
{
    return new Hashtable();
}

static Hashtable * hashtableCreate(Hashtable::Node * first, size_t len)
{
    return new Hashtable(first, len);
}

static void hashtableFree(Hashtable * hashtable)
{
    delete hashtable;
}

static Hashtable::Node * hashtableLookup(Hashtable * hashtable, hash_code_t h)
{
    return hashtable->lookup(h);
}

static Hashtable::Node * hashtableInsert(Hashtable * hashtable, hash_code_t h, size_t size)
{
    return hashtable->insert(h, size);
}

static Hashtable::Node ** hashtableTable(Hashtable * hashtable)
{
    return hashtable->table();
}

static size_t hashtableTableSize(Hashtable * hashtable)
{
    return hashtable->tableSize();
}

// generator functions
cg_voidptr_t genHashtableCreateCall()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void * (), false>::get(context);
    llvm::CallInst * result = codeGen.CreateCall(
            static_cast<Hashtable * (*)()>(&hashtableCreate), funcTy
    );

    return cg_voidptr_t( llvm::cast<llvm::Value>(result) );
}

cg_voidptr_t genHashtableCreateCall(cg_voidptr_t first, cg_size_t len)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void * (void *, size_t), false>::get(context);
    llvm::CallInst * result = codeGen.CreateCall(
            static_cast<Hashtable * (*)(Hashtable::Node *, size_t)>(&hashtableCreate), funcTy, {first, len}
    );

    return cg_voidptr_t( llvm::cast<llvm::Value>(result) );
}

void genHashtableFreeCall(cg_voidptr_t hashtable)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (void *), false>::get(context);
    codeGen.CreateCall(&hashtableFree, funcTy, hashtable.getValue());
}

cg_voidptr_t genHashtableLookupCall(cg_voidptr_t hashtable, cg_hash_t h)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void * (void *, hash_code_t), false>::get(context);
    llvm::CallInst * result = codeGen.CreateCall(&hashtableLookup, funcTy, {hashtable, h});

    return cg_voidptr_t( llvm::cast<llvm::Value>(result) );
}

cg_voidptr_t genHashtableInsertCall(cg_voidptr_t hashtable, cg_hash_t h, cg_size_t size)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void * (void *, hash_code_t, size_t), false>::get(context);
    llvm::CallInst * result = codeGen.CreateCall(&hashtableInsert, funcTy, {hashtable, h, size});

    return cg_voidptr_t( llvm::cast<llvm::Value>(result) );
}

cg_voidptr_t genHashtableTableCall(cg_voidptr_t hashtable)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void * (void *), false>::get(context);
    llvm::CallInst * result = codeGen.CreateCall(&hashtableTable, funcTy, hashtable.getValue());

    return cg_voidptr_t( llvm::cast<llvm::Value>(result) );
}

cg_size_t genHashtableTableSizeCall(cg_voidptr_t hashtable)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<size_t (void *), false>::get(context);
    llvm::CallInst * result = codeGen.CreateCall(&hashtableTableSize, funcTy, hashtable.getValue());

    return cg_size_t( llvm::cast<llvm::Value>(result) );
}

static void genBucketIter(cg_voidptr_t first, std::function<void(cg_voidptr_t)> elementHandler)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & funcGen = codeGen.getCurrentFunctionGen();

    // iterate over the all members of the bucket
    LoopGen bucketIter(funcGen, !first.isNullPtr(), {{"node", first}});
    cg_voidptr_t current(bucketIter.getLoopVar(0));
    {
        LoopBodyGen bodyGen(bucketIter);
//        Functions::genPrintfCall("node ptr: %p\n", current);

        elementHandler(current);
    }
    // the first field of the struct represents the "next"-pointer
    cg_voidptr_t next(codeGen->CreateLoad(cg_voidptr_t::getType(), current));
    bucketIter.loopDone(!next.isNullPtr(), {next});
}

void genHashtableIter(cg_voidptr_t hashtable, std::function<void(cg_voidptr_t)> elementHandler)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & funcGen = codeGen.getCurrentFunctionGen();

    cg_voidptr_t table = genHashtableTableCall(hashtable);
    cg_size_t tableSize = genHashtableTableSizeCall(hashtable);

    cg_size_t offset = cg_size_t(sizeof(Hashtable::Node *))*tableSize;
    cg_voidptr_t end = table + offset.llvmValue;

    LoopGen tableIter(funcGen, !table.isNullPtr(), {{"bucket", table}});
    cg_voidptr_t current(tableIter.getLoopVar(0));
    {
        LoopBodyGen bodyGen(tableIter);
//        Functions::genPrintfCall("bucket ptr: %p\n", current);

        cg_voidptr_t node( codeGen->CreateLoad(cg_voidptr_t::getType(), current) );
        IfGen nullCheck(funcGen, !node.isNullPtr());
        {
            genBucketIter(node, elementHandler);
        }
        nullCheck.EndIf();
    }
    // calculate the address of the next element within the array
    cg_voidptr_t next(current + sizeof(Hashtable::Node *));
    tableIter.loopDone(next != end, {next});
}


void genHashtableIter(cg_voidptr_t hashtable, cg_hash_t h, std::function<void(cg_voidptr_t)> elementHandler)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & funcGen = codeGen.getCurrentFunctionGen();

    llvm::Type * i8Ptr = llvm::PointerType::getInt8PtrTy(codeGen.getLLVMContext() );

//    Functions::genPrintfCall("lookup result: %p\n", first);

    cg_voidptr_t match;

    {
        cg_voidptr_t first = genHashtableLookupCall(hashtable, h);

        // iterate over the all members of the bucket
        LoopGen bucketIter(funcGen, !first.isNullPtr(), {{"current", first}});
        cg_voidptr_t current(bucketIter.getLoopVar(0));
        {
            LoopBodyGen bodyGen(bucketIter);

            // the second field of the "Node"-struct contains the hashcode
            cg_voidptr_t hp = current + sizeof(Hashtable::Node *);
//        Functions::genPrintfCall("hashcode ptr: %p\n", hp);

            cg_hash_t currentHash(codeGen->CreateLoad(cg_hash_t::getType(), hp));
//        Functions::genPrintfCall("hashcode: %lu\n", currentHash);

            IfGen compareHash(funcGen, h == currentHash);
            {
                // elementHandler is supposed to generate code that handles the current match
                elementHandler(current);
                match = current;
                bucketIter.loopBreak();
            }
        }
        // the first field of the struct represents the "next"-pointer
        cg_voidptr_t next(codeGen->CreateLoad(i8Ptr, current));
        bucketIter.loopDone(!next.isNullPtr(), {next});
    }

    {
        // iterate over the current sequence
        LoopGen sequenceIter(funcGen, !match.isNullPtr(), {{"current", match}});
        cg_voidptr_t current(sequenceIter.getLoopVar(0));
        {
            LoopBodyGen bodyGen(sequenceIter);

            cg_voidptr_t hp = current + sizeof(Hashtable::Node *);
            cg_hash_t currentHash(codeGen->CreateLoad(cg_hash_t::getType(), hp));

            IfGen compareHash(funcGen, h == currentHash);
            {
                elementHandler(current);
            }
            compareHash.Else();
            {
                sequenceIter.loopBreak();
            }
        }
        // the first field of the struct represents the "next"-pointer
        cg_voidptr_t next(codeGen->CreateLoad(i8Ptr, current));
        sequenceIter.loopDone(!next.isNullPtr(), {next});
    }
}

cg_voidptr_t genHashtableGetUserDataPtr(cg_voidptr_t nodePtr)
{
    static constexpr ptr_int_rep_t offset = offsetof(Hashtable::Node, data);
    return (nodePtr + offset);
}
