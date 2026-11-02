#pragma once
class OneButton {
public:
    OneButton(int, bool) {}
    void attachClick(void (*)()) {}
    void attachDoubleClick(void (*)()) {}
    void attachLongPressStart(void (*)()) {}
    void attachLongPressStop(void (*)()) {}
    void tick() {}
};
