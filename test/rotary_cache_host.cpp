#include "RotaryFrameCache.h"
#include <cassert>
#include <cstring>
#include <cstdio>
using pixelpad::RotaryFrameCache;
static void load(RotaryFrameCache& cache, uint32_t session, int region, int value) {
    const size_t bytes = RotaryFrameCache::FrameBytes * (region < 0 ? 1 : 2);
    auto* data = static_cast<uint8_t*>(malloc(bytes));
    memset(data, value, bytes);
    if (region >= 0) memset(data + RotaryFrameCache::FrameBytes, value + 1, RotaryFrameCache::FrameBytes);
    assert(cache.store(session, region, data, bytes));
    assert(!data);
}
int main() {
    RotaryFrameCache cache;
    const int32_t ids[] = {7, 2, 19};
    assert(cache.begin(1, ids, 3, true));
    load(cache, 1, -1, 1);
    load(cache, 1, 7, 10); load(cache, 1, 2, 20);
    assert(!cache.ready());
    assert(!cache.select(1, 7, true, 0, 600, 200));
    load(cache, 1, 19, 30);
    assert(cache.ready());
    assert(cache.select(1, -1, false, 0, 600, 200));
    assert(*cache.target() == 1);
    assert(cache.turn(-1, 10) && cache.region() == 19 && *cache.target() == 30);
    assert(cache.turn(1, 20) && cache.region() == 7 && *cache.target() == 10);
    for (int i = 0; i < 10000; ++i) assert(cache.turn(1, 30));
    assert(cache.region() == 2);
    cache.tick(629); assert(*cache.target() == 20);
    cache.tick(630); assert(*cache.target() == 21);
    assert(cache.select(1, 7, true, UINT32_MAX - 100, 600, 200));
    cache.tick(498); assert(*cache.target() == 10);
    cache.tick(499); assert(*cache.target() == 11);
    cache.pause(); assert(!cache.turn(1, 1000));
    // Replacing the current artwork frees all old frames and invalidates IDs.
    assert(cache.begin(2, ids, 3, false));
    assert(!cache.ready() && !cache.armed() && !cache.target());
    load(cache, 2, -1, 1);
    for (int id : ids) load(cache, 2, id, id);
    assert(!cache.select(1, 7, true, 0, 600, 200));
    assert(cache.select(2, -1, false, 0, 600, 200));
    assert(cache.turn(-1, 0) && cache.region() == 7);
    assert(cache.turn(-1, 0) && cache.region() == 7);
    assert(cache.turn(1, 0) && cache.region() == 2);
    std::puts("PASS rotary cache: full readiness, local steps, order, hold, wrap, replacement");
}
