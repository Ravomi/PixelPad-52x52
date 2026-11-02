#include "button.h"
#include "ColorMenuInput.h"
#include "InputNotifications.h"
#include <cassert>
#include <cstring>

uint32_t hostMillis = 0;
int64_t hostEncoderCount = 0;
puType ESP32Encoder::useInternalWeakPullResistors = puType::up;
static pixelpad::RotaryFrameCache cache;
static bool selector = false;
static pixelpad::ColorMenuAction action;
namespace pixelpad {
uint32_t inputSession() { return 1; }
bool postInput(Input input) {
    action = handleColorMenuInput(cache, input, hostMillis, selector);
    if (action == ColorMenuAction::Open) selector = true;
    if (action == ColorMenuAction::Confirm) selector = false;
    return true;
}
}
static void load(int region, int value) {
    const size_t size = pixelpad::RotaryFrameCache::FrameBytes * (region < 0 ? 1 : 2);
    auto* bytes = static_cast<uint8_t*>(malloc(size));
    memset(bytes, value, size);
    if (region >= 0) memset(bytes + size/2, value+1, size/2);
    assert(cache.store(1,region,bytes,size));
}
int main() {
    using pixelpad::ColorMenuAction;
    MyControl control;
    control.begin();
    auto* button = OneButton::instance();
    button->longStart(); control.loop();
    assert(action == ColorMenuAction::Unavailable && !cache.menuOpen());
    const int32_t order[] = {7,2,19};
    const char* labels[] = {"A7","F14","H2"};
    const uint8_t colors[][3] = {{120,0,0},{0,100,0},{0,0,90}};
    assert(cache.begin(1,order,3,true,labels,colors));
    load(-1,1); load(7,10); load(2,20); load(19,30);
    // Actual MyControl callbacks -> input queue sink -> production menu dispatcher.
    button->longStart(); control.loop();
    assert(action == ColorMenuAction::Open && cache.menuOpen() && cache.region()==7);
    button->longStop(); control.loop();
    assert(action == ColorMenuAction::Release && cache.menuOpen());
    cache.tick(10000); assert(cache.menuOpen() && *cache.target()==10);
    hostEncoderCount -= 2; control.loop();
    assert(action == ColorMenuAction::Step && cache.region()==2);
    hostEncoderCount -= 2; control.loop();
    assert(action == ColorMenuAction::Step && cache.region()==19);
    hostEncoderCount -= 2; control.loop();
    assert(action == ColorMenuAction::Step && cache.region()==7);
    hostEncoderCount += 2; control.loop();
    assert(cache.region()==19);
    button->click(); control.loop();
    assert(action == ColorMenuAction::Confirm && !cache.menuOpen() && *cache.target()==31);
    button->longStart(); control.loop();
    assert(cache.menuOpen() && cache.region()==19);
    button->longStop(); control.loop();
    button->longStart(); control.loop();
    assert(action == ColorMenuAction::Confirm && !cache.menuOpen());
    button->longStop(); control.loop();
    assert(!cache.menuOpen());
    button->longStart(); control.loop();
    button->doubleClick(); control.loop();
    assert(action == ColorMenuAction::Complete && cache.menuOpen());
    char label[16]; uint8_t rgb[3];
    assert(cache.menuItem(1,label,sizeof(label),rgb));
    assert(!strcmp(label,"F14") && rgb[1]==100);
    cache.clear();
    assert(!cache.menuOpen());
    assert(cache.begin(1,order,3,false,labels,colors));
    load(-1,1); load(7,10); load(2,20); load(19,30);
    selector = false;
    button->longStart(); control.loop();
    hostEncoderCount += 2; control.loop();
    assert(action == ColorMenuAction::Boundary && cache.region()==7);

    // With a stalled phone, process more physical turns than the notification
    // FIFO can hold. Every turn must still update the real resident cache.
    pixelpad::InputNotifications notifications;
    assert(cache.begin(1,order,3,true,labels,colors));
    load(-1,1); load(7,10); load(2,20); load(19,30);
    button->longStart(); control.loop();
    for (unsigned i = 1; i <= 200; ++i) {
        hostEncoderCount -= 2; control.loop();
        assert(action == ColorMenuAction::Step);
        assert(cache.region() == order[i % 3]);
        const unsigned char values[] = {10,20,30};
        assert(*cache.target() == values[i % 3]);
        notifications.push({pixelpad::Input::Minus, i, 1, 1, cache.region()});
        // Intentionally do not drain: the BLE sender is congested.
    }
    assert(notifications.size() == 64 && notifications.drops() == 136);
    for (unsigned seq = 137; seq <= 200; ++seq) {
        assert(notifications.front().sequence == seq);
        assert(notifications.front().region == order[seq % 3]);
        notifications.pop();
    }
    assert(notifications.size() == 0);
    notifications.push({pixelpad::Input::Click, 201, 1, 1, cache.region()});
    notifications.clear();
    assert(notifications.size() == 0); // Old-session events cannot be replayed.
}
