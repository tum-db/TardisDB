
#ifndef STATICHASHTABLE_H
#define STATICHASHTABLE_H

#include <vector>

#include <llvm/IR/Value.h>

#include "codegen/CodeGen.hpp"

//-----------------------------------------------------------------------------
// StaticHashtable

typedef uint64_t hash_code_t;

/// A very simple immutable Hashtable
class StaticHashtable {
public:
    typedef struct Node {
        Node * next;
        hash_code_t hashCode;
        /* payload */
    } node_t;

    /// \brief Input: Linked list of nodes
    /// Attention: The constructor doesn't preserve the input list's structure.
    /// The caller is responsible to free the list's memory.
    StaticHashtable(Node * first, size_t len);
    ~StaticHashtable();

    Node * lookup(hash_code_t h);
    Node * next(hash_code_t h, Node * start);

private:
    void build(Node * first);

    size_t hash(hash_code_t h) { return h & moduloMask; }

    node_t ** table;
    size_t tableSize;
    size_t moduloMask;
    float loadFactor;
};

// wrapper functions
extern "C" {
StaticHashtable * staticHashtableCreate(StaticHashtable::Node * first, size_t len);
void staticHashtableFree(StaticHashtable * hashtable);
}

// generator functions
cg_voidptr_t genStaticHashtableCreateCall(cg_voidptr_t first, cg_size_t len);
void genStaticHashtableFreeCall(cg_voidptr_t hashtable);
cg_voidptr_t genStaticHashtableLookupCall(cg_voidptr_t hashtable, cg_hash_t h);
void genStaticHashtableIter(cg_voidptr_t hashtable, cg_hash_t h, std::function<void(cg_voidptr_t)> elementHandler);

#endif // STATICHASHTABLE_H
