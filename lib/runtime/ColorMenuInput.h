#pragma once
#include "Runtime.h"
#include "RotaryFrameCache.h"

namespace pixelpad {
enum class ColorMenuAction { None, Open, Confirm, Step, Boundary, Complete, Release, Unavailable };

// Shared by the runtime and host input replay. WhiteOn/WhiteOff are legacy
// enum names for the physical press/release; neither requests white pixels.
inline ColorMenuAction handleColorMenuInput(RotaryFrameCache& cache, Input input,
                                            uint32_t now, bool selectorSession) {
    if (input == Input::WhiteOff) return ColorMenuAction::Release;
    if (input == Input::WhiteOn) {
        if (cache.menuOpen()) {
            cache.confirm(now);
            return ColorMenuAction::Confirm;
        }
        return cache.openMenu(now) ? ColorMenuAction::Open : ColorMenuAction::Unavailable;
    }
    if (input == Input::DoubleClick && cache.menuOpen()) return ColorMenuAction::Complete;
    if (input == Input::Click && cache.codeVisible() && cache.confirm(now))
        return ColorMenuAction::Confirm;
    if ((input == Input::Plus || input == Input::Minus) && cache.ready() &&
        (cache.menuOpen() || selectorSession)) {
        if (!cache.menuOpen()) cache.openMenu(now);
        const int32_t previous = cache.region();
        cache.turn(input == Input::Plus ? -1 : 1, now);
        return cache.region() == previous ? ColorMenuAction::Boundary : ColorMenuAction::Step;
    }
    return ColorMenuAction::None;
}
}
