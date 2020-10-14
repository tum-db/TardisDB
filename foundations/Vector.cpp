
#include "foundations/Vector.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cmath>

#include <llvm/IR/TypeBuilder.h>

#include "codegen/CodeGen.hpp"

Vector::Vector(size_type elementSize) :
        Vector(elementSize, 0)
{ }

Vector::Vector(size_type elementSize, size_type reserveCount) :
        _elementSize(elementSize),
        _elementCount(reserveCount)
{
    _capacity = defaultCount;

    if (reserveCount > defaultCount) {
        unsigned power = static_cast<unsigned>( std::ceil( std::log2(_capacity) ) );
        _capacity = 1ul << power;
    }

    _arraySize = elementSize*_capacity; // size in bytes
    _array = static_cast<uint8_t *>(std::malloc(_arraySize));
}

Vector::~Vector()
{
    std::free(_array);
}

void Vector::push_back(void * ptr)
{
    void * elemAddr = reserve_back();
    std::memcpy(elemAddr, ptr, _elementSize);
}

void Vector::remove_at(size_type index) {
    if (index < size() - 1) {
        void *elementPtr = at(index);
        void *nextElementPtr = at(index + 1);
        size_type restLength = (size() - index - 1) * _elementSize;
        std::memcpy(elementPtr,nextElementPtr,restLength);
    }
    pop_back();
}

void * Vector::reserve_back()
{
    if (_elementCount == _capacity) {
        _capacity <<= 1;
        _arraySize = _elementSize*_capacity; // size in bytes
        _array = static_cast<uint8_t *>(std::realloc(_array, _arraySize));
        assert(_array);
    }

    void * elemAddr = (_array + _elementSize*_elementCount);
    _elementCount += 1;
    return elemAddr;
}

void Vector::pop_back()
{
    assert(_elementCount > 0);

    _elementCount -= 1;
    // TODO hysteresis

    if (_elementCount <= defaultCount) {
        return;
    } else if (_elementCount <= (_capacity >> 1)) {
        _capacity >>= 1;
        _arraySize = _elementSize*_capacity; // size in bytes
        _array = static_cast<uint8_t *>(std::realloc(_array, _arraySize));
        assert(_array);
    }
}

void * Vector::operator[](size_type index)
{
    assert(index < _elementCount);
    return (_array + _elementSize*index);
}

const void * Vector::operator[](size_type index) const
{
    assert(index < _elementCount);
    return (_array + _elementSize*index);
}

void * Vector::at(size_type index)
{
    assert(index < _elementCount);
    return (_array + _elementSize*index);
}

const void * Vector::at(size_type index) const
{
    assert(index < _elementCount);
    return (_array + _elementSize*index);
}

void * Vector::front()
{
    return _array;
}

const void * Vector::front() const
{
    return _array;
}

void * Vector::back()
{
    return at(_elementCount - 1);
}

const void * Vector::back() const
{
    return at(_elementCount - 1);
}

Vector::size_type Vector::size()
{
    return _elementCount;
}

bool Vector::empty()
{
    return (_elementCount == 0);
}

// wrapper functions
static void * vectorReserveBack(Vector * vector)
{
    return vector->reserve_back();
}

static void * vectorBack(Vector * vector)
{
    return vector->back();
}

// generator functions
cg_voidptr_t genVectorReserveBackCall(cg_voidptr_t vector)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void * (void *), false>::get(context);
    llvm::CallInst * result = codeGen.CreateCall(&vectorReserveBack, funcTy, vector.llvmValue);

    return cg_voidptr_t( llvm::cast<llvm::Value>(result) );
}

cg_voidptr_t genVectoBackCall(cg_voidptr_t vector)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void * (void *), false>::get(context);
    llvm::CallInst * result = codeGen.CreateCall(&vectorBack, funcTy, vector.llvmValue);

    return cg_voidptr_t( llvm::cast<llvm::Value>(result) );
}
