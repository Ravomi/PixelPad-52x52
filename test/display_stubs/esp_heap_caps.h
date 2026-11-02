#pragma once
#include <cstdlib>
#include <vector>
constexpr unsigned MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
extern std::vector<void*> allocations;
inline void* heap_caps_malloc(size_t bytes, unsigned) {
    void* p = std::malloc(bytes);
    allocations.push_back(p);
    return p;
}
