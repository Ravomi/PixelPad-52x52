#include "ReceiveDeadline.h"
#include <cassert>
#include <cstdint>
#include <initializer_list>

int main() {
    // A 20-byte packet every 20 ms takes >8 s to deliver 8112 bytes. The
    // old 5 s total deadline rejects it even though the radio is progressing.
    for (uint32_t start : {uint32_t(0), UINT32_MAX - 4000}) {
        pixelpad::ReceiveDeadline deadline(start, 5000, 30000);
        for (uint32_t elapsed = 0; elapsed <= 8120; elapsed += 20) {
            assert(deadline.remainingMs(start + elapsed) > 0);
            deadline.madeProgress(start + elapsed);
        }
        assert(deadline.remainingMs(start + 13119) == 1);
        assert(deadline.remainingMs(start + 13120) == 0);
    }
    // Progress must not extend the absolute cap indefinitely.
    pixelpad::ReceiveDeadline total(100, 5000, 30000);
    for (uint32_t elapsed = 1000; elapsed < 30000; elapsed += 1000) {
        assert(total.remainingMs(100 + elapsed));
        total.madeProgress(100 + elapsed);
    }
    assert(total.remainingMs(30099) == 1);
    assert(total.remainingMs(30100) == 0);
    pixelpad::ReceiveDeadline stalled(1234, 5000, 30000);
    assert(stalled.remainingMs(6233) == 1);
    assert(stalled.remainingMs(6234) == 0);
}
