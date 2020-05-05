//
// Created by josef on 28.12.16.
//

#ifndef LLVM_PROTOTYPE_TYPECACHE_HPP
#define LLVM_PROTOTYPE_TYPECACHE_HPP

#include <string>
#include <unordered_map>

#include <llvm/IR/Type.h>
#ifndef __APPLE__
#include <bits/unordered_map.h>
#endif

/// A very simple type cache based on type signatures
class TypeCache {
public:
    /// \brief Add the given type to the cache. typeSignature has to be unique.
    void add(const std::string & typeSignature, llvm::Type * type)
    {
        if (cache.find(typeSignature) != cache.end()) {
            throw std::runtime_error("already cached");
        }
        cache[typeSignature] = type;
    }

    /// \brief Lookup the type for the given signature
    /// \returns The type or nullptr if no type with this signature is found
    llvm::Type * get(const std::string & typeSignature)
    {
        auto it = cache.find(typeSignature);
        if (it == cache.end()) {
            return nullptr;
        }
        return it->second;
    }

private:
    std::unordered_map<std::string, llvm::Type *> cache;
};

#endif //LLVM_PROTOTYPE_TYPECACHE_HPP
