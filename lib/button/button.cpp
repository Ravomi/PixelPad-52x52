#include "button.h"
#include "Runtime.h"

MyControl* MyControl::activeControl_ = nullptr;
MyControl::MyControl() : button(ENC_SW, true), oldPosition(0) {}

void MyControl::begin() {
    activeControl_ = this;
    ESP32Encoder::useInternalWeakPullResistors = puType::up;
    encoder.attachHalfQuad(ENC_DT, ENC_CLK);
    encoder.setCount(0);
    lastSession_ = pixelpad::inputSession();
    // Retain OneButton's click/double-click/long-press semantics.
    button.attachClick(handleClick);
    button.attachDoubleClick(handleDoubleClick);
    button.attachLongPressStart(handleLongPressStart);
    button.attachLongPressStop(handleLongPressStop);
}

void MyControl::loop() {
    const int64_t position = encoder.getCount();
    const uint32_t session = pixelpad::inputSession();
    if (session != lastSession_) {
        pending_ = {};
        oldPosition = position;
        lastSession_ = session;
    }
    const int64_t steps = (position - oldPosition) / 2;
    if (steps) {
        const bool plus = steps > 0;
        const uint64_t magnitude = plus ? steps : -steps;
        const uint32_t count = magnitude > UINT32_MAX ? UINT32_MAX : magnitude;
        if (pending_.push(plus ? pixelpad::Input::Plus : pixelpad::Input::Minus, count)) {
            // Retain the incomplete detent. Never replace this with position.
            oldPosition += (plus ? int64_t(count) : -int64_t(count)) * 2;
        } else {
            ++backlogFull_; // Leave unqueued movement in the encoder counter.
        }
    }
    button.tick(); // Button events join the same FIFO after preceding motion.
    const uint64_t pending = pending_.pending();
    const uint32_t bounded = pending > UINT32_MAX ? UINT32_MAX : pending;
    if (bounded > backlogPeak_.load()) backlogPeak_ = bounded;
    // Bound sampler work, and leave failed enqueues at the head for retry.
    for (unsigned i = 0; i < 8 && !pending_.empty(); ++i) {
        if (!pixelpad::postInput(pending_.front())) break;
        pending_.pop();
    }
    const uint64_t remaining = pending_.pending();
    pendingCount_ = remaining > UINT32_MAX ? UINT32_MAX : remaining;
}

void MyControl::enqueueButton(pixelpad::Input input) {
    if (activeControl_ && !activeControl_->pending_.push(input))
        ++activeControl_->buttonDrops_;
}
void MyControl::handleClick() { enqueueButton(pixelpad::Input::Click); }
void MyControl::handleDoubleClick() { enqueueButton(pixelpad::Input::DoubleClick); }
void MyControl::handleLongPressStart() { enqueueButton(pixelpad::Input::WhiteOn); }
void MyControl::handleLongPressStop() { enqueueButton(pixelpad::Input::WhiteOff); }
