#pragma once
class OneButton {
public:
    OneButton(int, bool) { instance() = this; }
    void attachClick(void (*fn)()) { click_ = fn; }
    void attachDoubleClick(void (*fn)()) { double_ = fn; }
    void attachLongPressStart(void (*fn)()) { start_ = fn; }
    void attachLongPressStop(void (*fn)()) { stop_ = fn; }
    void tick() { auto fn = pending_; pending_ = nullptr; if (fn) fn(); }
    static OneButton*& instance() { static OneButton* value = nullptr; return value; }
    void longStart() { pending_ = start_; }
    void longStop() { pending_ = stop_; }
    void click() { pending_ = click_; }
    void doubleClick() { pending_ = double_; }
private:
    void (*click_)() = nullptr;
    void (*double_)() = nullptr;
    void (*start_)() = nullptr;
    void (*stop_)() = nullptr;
    void (*pending_)() = nullptr;
};
