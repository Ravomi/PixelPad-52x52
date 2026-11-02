#include "Tween.h"
#include <assert.h>

int main() {
    pixelpad::Tween tween;
    tween.start(0, 100, 0xfffffff0u, 32);
    assert(tween.update(0u) >= 0 && tween.active());
    assert(tween.update(0x20u) == 100 && !tween.active());
    tween.start(10, 10, 100, 100);
    assert(tween.update(100) == 10 && !tween.active());
    tween.start(0, 100, 0, 100, pixelpad::Tween::Easing::EaseOut);
    const int first = tween.update(25);
    const int second = tween.update(50);
    assert(first > 0 && second > first && second < 100);

    float followed = 0.0f;
    assert(pixelpad::FollowTween::step(followed, 100.0f, 70.0f));
    assert(followed > 0.0f && followed < 100.0f);
    const float before = followed;
    pixelpad::FollowTween::step(followed, 100.0f, 70.0f);
    assert(followed > before);

    float wrapped = 3.0f;
    pixelpad::FollowTween::stepWrapped(wrapped, 0.0f, 4, 70.0f);
    assert(wrapped > 3.0f || wrapped < 1.0f);
}
