
#pragma once

#include <cstddef>
#include <cstdint>

#include "CodeGen.hpp"

// vector without type information
class Vector {
public:
    using size_type = size_t;

    static const size_type defaultCount = 2;

    Vector(size_type elementSize);

    Vector(size_type elementSize, size_type reserveCount);

    ~Vector();

    size_type getElementSize() const { return _elementSize; }

    void push_back(void * ptr);

    void * reserve_back();

    void pop_back();

    void * operator[](size_type index);
    const void * operator[](size_type index) const;

    void * at(size_type index);
    const void * at(size_type index) const;

    void * front();
    const void * front() const;

    void * back();
    const void * back() const;

    size_type size();

    bool empty();

private:
    size_type _elementSize;
    size_type _elementCount = 0;
    size_type _capacity;
    size_type _arraySize;
    uint8_t * _array;
};

// generator functions
cg_voidptr_t genVectorReserveBackCall(cg_voidptr_t vector);

cg_voidptr_t genVectoBackCall(cg_voidptr_t vector);
