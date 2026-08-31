// The standard replaceable allocation functions: every new / delete goes through the heap router.
#include <new>
#include <new.h>
#include "RollbackHeap.h"

void* __CRTDECL operator new(size_t size) {
    for (;;) {
        if (void* block = GameAlloc(size)) return block;
        if (_callnewh(size) == 0) throw std::bad_alloc();
    }
}
void* __CRTDECL operator new[](size_t size) {
    return operator new(size);
}
void* __CRTDECL operator new(size_t size, const std::nothrow_t&) throw() {
    try {
        return operator new(size);
    } catch (...) {
        return nullptr;
    }
}
void* __CRTDECL operator new[](size_t size, const std::nothrow_t&) throw() {
    try {
        return operator new[](size);
    } catch (...) {
        return nullptr;
    }
}

void __CRTDECL operator delete(void* block) throw() { GameFree(block); }
void __CRTDECL operator delete[](void* block) throw() { GameFree(block); }
void __CRTDECL operator delete(void* block, size_t) throw() { GameFree(block); }
void __CRTDECL operator delete[](void* block, size_t) throw() { GameFree(block); }
void __CRTDECL operator delete(void* block, const std::nothrow_t&) throw() { GameFree(block); }
void __CRTDECL operator delete[](void* block, const std::nothrow_t&) throw() { GameFree(block); }
