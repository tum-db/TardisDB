
#ifndef HASHTABLE_H
#define HASHTABLE_H

#include <vector>

#include <llvm/IR/Value.h>

#include "CodeGen.hpp"
#include "MemoryPool.hpp"

//-----------------------------------------------------------------------------
// Hashtable

typedef uint64_t hash_code_t;

/// A very simple Hashtable
class Hashtable {
public:
    struct Node {
        Node * next;
        hash_code_t hashCode;
        /* user data */
        uint8_t data[];
    };

    static const size_t defaultSize = 16;

    Hashtable();
    Hashtable(size_t sizeHint);

    /// \brief Input: Linked list of nodes
    /// Attention: The constructor doesn't preserve the input list's structure.
    /// The caller is responsible to free the list's memory.
    Hashtable(Node * first, size_t len);
    ~Hashtable();

    Node * first();

    Node * lookup(hash_code_t h);

    Node * next(Node * current);

    Node * next(Node * current, hash_code_t h);

    /// \brief Inserts one element with the hashcode h.
    /// \returns A Node with "size" bytes of user data.
    Node * insert(hash_code_t h, size_t size);

    Node ** table() const { return _table; }

    size_t tableSize() const { return _tableSize;}

private:
    void build(Node * first);

    void insert(Node * node);

    void rehash();

    size_t hash(hash_code_t h) { return h & _moduloMask; }

    Node ** _table;
    size_t _tableSize;
    size_t _moduloMask;

    size_t _uniqueCount = 0;
    size_t _uniqueCountThreshold;

    float _loadFactorThreshold = 0.75f; // usually a good default
    MemoryPool _memoryPool;
};

// generator functions
cg_voidptr_t genHashtableCreateCall();
cg_voidptr_t genHashtableCreateCall(cg_voidptr_t first, cg_size_t len);
void genHashtableFreeCall(cg_voidptr_t hashtable);
cg_voidptr_t genHashtableLookupCall(cg_voidptr_t hashtable, cg_hash_t h);
void genHashtableIter(cg_voidptr_t hashtable, std::function<void(cg_voidptr_t)> elementHandler);
void genHashtableIter(cg_voidptr_t hashtable, cg_hash_t h, std::function<void(cg_voidptr_t)> elementHandler);
cg_voidptr_t genHashtableInsertCall(cg_voidptr_t hashtable, cg_hash_t h, cg_size_t size);
cg_size_t genHashtableTableSizeCall(cg_voidptr_t hashtable);
cg_voidptr_t genHashtableGetUserDataPtr(cg_voidptr_t nodePtr);

#endif // HASHTABLE_H
