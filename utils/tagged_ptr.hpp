#pragma once

#include <cstdint>

template<class T>
class tagged_ptr {
    union {
        T * ptr;
        uintptr_t ptrValue;
    };

public:
    explicit tagged_ptr(T * ptr)
        : ptr(ptr) { }

    T * get() const {
        return ptr;
    }

    tagged_ptr<T> & tag() {
        ptrValue |= 1;
        return *this;
    }

    tagged_ptr<T> & untag() {
        ptrValue &= ~1;
        return *this;
    }

    bool is_tagged() const {
        return (ptrValue & 1);
    }

    operator T *() const {
        return ptr;
    }
};
