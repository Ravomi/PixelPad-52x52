#include "FrameTransition.h"
#include <cassert>
#include <cstdio>

int main() {
    pixelpad::FrameTransition<3> fade;
    uint8_t shown[3] = {200, 0, 0};
    fade.begin(shown, 100, 200);
    uint8_t target[3] = {0, 0, 200};
    fade.apply(target, 100);
    assert(target[0] == 200 && target[2] == 0 && fade.active());
    target[0] = 0; target[2] = 200;
    fade.apply(target, 200);
    assert(target[0] == 100 && target[2] == 100);
    // Interrupt from the last actually shown image, without an old-target jump.
    fade.begin(target, 200, 200);
    uint8_t newer[3] = {0, 200, 0};
    fade.apply(newer, 200);
    assert(newer[0] == 100 && newer[1] == 0 && newer[2] == 100);
    newer[0] = 0; newer[1] = 200; newer[2] = 0;
    fade.apply(newer, 400);
    assert(newer[0] == 0 && newer[1] == 200 && newer[2] == 0 && !fade.active());
    fade.begin(newer, 0xfffffff0u, 32);
    uint8_t black[3] = {};
    fade.apply(black, 0x10u);
    assert(black[0] == 0 && black[1] == 0 && black[2] == 0 && !fade.active());
    fade.begin(newer, 500, 0);
    uint8_t instant[3] = {10,20,30};
    fade.apply(instant, 500);
    assert(!fade.active() && instant[0] == 10 && instant[2] == 30);
    unsigned previous = 0;
    const uint8_t start[3] = {};
    fade.begin(start, 0, 200);
    for (unsigned t = 0; t <= 200; ++t) {
        uint8_t white[3] = {255,255,255};
        fade.apply(white, t);
        assert(white[0] >= previous && white[0] == white[1] && white[1] == white[2]);
        previous = white[0];
    }
    assert(previous == 255 && !fade.active());
    puts("Frame transition endpoints, easing, retargeting, cancel and time wrap PASS");
}
