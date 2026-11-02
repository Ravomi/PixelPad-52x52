#include "PublishedFrame.h"
#include <assert.h>
#include <string.h>
int main() {
    pixelpad::PublishedFrame<52,52> frame;
    uint8_t scan[52*52*3]{};
    frame.setPixel(0,0,255,0,0);
    frame.snapshot(scan);
    assert(scan[0]==0); // A scan during drawing must retain the old full frame.
    frame.setPixel(51,51,0,255,0);
    assert(frame.commit());
    frame.snapshot(scan);
    assert(scan[0]==255 && scan[sizeof(scan)-2]==255);
    assert(!frame.commit());
    frame.setPixel(-1,0,1,2,3); frame.setPixel(52,51,1,2,3);
    assert(!frame.commit());
    frame.setPixel(0,0,255,0,0); assert(!frame.commit());
    frame.clear();
    frame.setPixel(1,0,0,0,127);
    frame.snapshot(scan); assert(scan[0]==255 && scan[sizeof(scan)-2]==255);
    assert(frame.commit()); frame.snapshot(scan);
    assert(scan[0]==0 && scan[5]==127 && scan[sizeof(scan)-2]==0);
}
