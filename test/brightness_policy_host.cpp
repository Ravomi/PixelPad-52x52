// Run: c++ -std=c++11 -Ilib/led test/brightness_policy_host.cpp -o /tmp/pixelpad-brightness-test && /tmp/pixelpad-brightness-test
#include <assert.h>
#include "brightness_policy.h"

int main() {
    static_assert(pixelpad::scaleBrightness(0) == 0, "off remains off");
    static_assert(pixelpad::scaleBrightness(255) == 229, "maximum is capped");
    static_assert(pixelpad::scaleBrightness(100) == 90, "linear scaling");
    unsigned previous = 0;
    for (unsigned value = 0; value <= 255; ++value) {
        const unsigned actual = pixelpad::scaleBrightness(value);
        assert(actual >= previous);
        assert(actual <= 229);
        assert(actual * 100 <= value * 90);
        previous = actual;
    }
}
