#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

namespace pixelpad {
// Render-task owned, one current artwork only. Payloads come from ps_malloc
// in the protocol task and transfer ownership after a complete READY/body.
class RotaryFrameCache {
public:
    static constexpr size_t FrameBytes = 52 * 52 * 3;
    static constexpr size_t MaxRegions = 64;
    ~RotaryFrameCache() { clear(); }
    bool begin(uint32_t session, const int32_t* order, size_t count, bool wrap,
               const char* const* labels = nullptr,
               const uint8_t (*colors)[3] = nullptr) {
        if (!session || !order || !count || count > MaxRegions) return false;
        for (size_t i = 0; i < count; ++i) {
            if (order[i] < 0) return false;
            for (size_t j = 0; j < i; ++j) if (order[i] == order[j]) return false;
        }
        clear();
        session_ = session; count_ = count; wrap_ = wrap;
        for (size_t i = 0; i < count; ++i) {
            entries_[i].id = order[i];
            snprintf(entries_[i].label, sizeof(entries_[i].label), "%ld",
                     static_cast<long>(order[i]));
            if (labels && labels[i] && labels[i][0]) {
                strncpy(entries_[i].label, labels[i], sizeof(entries_[i].label) - 1);
                entries_[i].label[sizeof(entries_[i].label) - 1] = 0;
            }
            if (colors) memcpy(entries_[i].color, colors[i], 3);
        }
        return true;
    }
    void clear() {
        pause();
        for (auto& entry : entries_) { free(entry.data); entry = {}; }
        free(base_); base_ = nullptr; session_ = 0; count_ = 0; cursor_ = -1;
    }
    bool store(uint32_t session, int32_t region, uint8_t*& data, size_t bytes) {
        if (!session_ || session != session_ || !data) return false;
        if (region == -1 && bytes == FrameBytes) {
            pause(); free(base_); base_ = data; data = nullptr; return true;
        }
        const int index = find(region);
        if (index < 0 || bytes != FrameBytes * 2) return false;
        pause(); free(entries_[index].data); entries_[index].data = data;
        data = nullptr;
        return true;
    }
    bool ready() const {
        if (!base_ || !session_) return false;
        for (size_t i = 0; i < count_; ++i) if (!entries_[i].data) return false;
        return true;
    }
    bool select(uint32_t session, int32_t region, bool showCode,
                uint32_t now, uint16_t hold, uint16_t fade) {
        if (session != session_ || !ready() || (region != -1 && find(region) < 0))
            return false;
        hold_ = hold > 2000 ? 2000 : hold;
        fade_ = fade > 500 ? 500 : fade;
        cursor_ = region == -1 ? -1 : find(region);
        armed_ = true;
        menuMode_ = false;
        display(showCode && cursor_ >= 0, now);
        return true;
    }
    // Board-local interaction: open the color menu without waiting for the
    // phone to echo a selection command back over BLE.
    bool openMenu(uint32_t now, uint16_t hold = 600, uint16_t fade = 200) {
        if (!ready()) return false;
        hold_ = hold > 2000 ? 2000 : hold;
        fade_ = fade > 500 ? 500 : fade;
        if (cursor_ < 0) cursor_ = 0;
        armed_ = true;
        menuMode_ = true;
        display(true, now);
        return true;
    }
    // A click confirms the currently previewed color and swaps to its region
    // frame. The cache remains armed so the next rotation can preview again.
    bool confirm(uint32_t now) {
        if (!armed_ || !ready() || cursor_ < 0) return false;
        display(false, now);
        armed_ = false;
        menuMode_ = false;
        return true;
    }
    bool turn(int delta, uint32_t now) {
        if (!armed_ || !ready()) return false;
        if (wrap_) {
            cursor_ = ((cursor_ < 0 ? 0 : cursor_) + delta + int(count_)) % int(count_);
        } else if (cursor_ < 0) cursor_ = 0;
        else {
            const int next = cursor_ + delta;
            if (next < 0 || next >= int(count_)) return true;
            cursor_ = next;
        }
        display(true, now);
        return true;
    }
    void tick(uint32_t now) {
        if (armed_ && code_ && !menuMode_ && uint32_t(now - codeAt_) >= hold_) display(false, now);
    }
    void pause() { armed_ = false; code_ = false; menuMode_ = false; target_ = nullptr; }
    const uint8_t* target() const { return target_; }
    uint32_t revision() const { return revision_; }
    uint16_t fade() const { return fade_; }
    uint32_t session() const { return session_; }
    int32_t region() const { return cursor_ < 0 ? -1 : entries_[cursor_].id; }
    bool armed() const { return armed_; }
    bool codeVisible() const { return armed_ && code_; }
    bool menuOpen() const { return armed_ && menuMode_; }
    size_t cursorIndex() const { return cursor_ < 0 ? 0 : static_cast<size_t>(cursor_); }
    bool menuItem(size_t index, char* label, size_t labelBytes, uint8_t color[3]) const {
        if (index >= count_ || !label || labelBytes == 0 || !color) return false;
        strncpy(label, entries_[index].label, labelBytes - 1);
        label[labelBytes - 1] = 0;
        memcpy(color, entries_[index].color, 3);
        return true;
    }
    size_t loaded() const {
        size_t n = 0;
        for (size_t i = 0; i < count_; ++i) if (entries_[i].data) ++n;
        return n;
    }
    size_t count() const { return count_; }
private:
    struct Entry {
        int32_t id = -1;
        char label[16] = {};
        uint8_t color[3] = {40, 40, 40};
        uint8_t* data = nullptr;
    } entries_[MaxRegions];
    uint8_t* base_ = nullptr;
    const uint8_t* target_ = nullptr;
    uint32_t session_ = 0, revision_ = 0, codeAt_ = 0;
    size_t count_ = 0;
    int cursor_ = -1;
    uint16_t hold_ = 600, fade_ = 200;
    bool wrap_ = true, armed_ = false, code_ = false, menuMode_ = false;
    int find(int32_t region) const {
        for (size_t i = 0; i < count_; ++i) if (entries_[i].id == region) return int(i);
        return -1;
    }
    void display(bool code, uint32_t now) {
        code_ = code; codeAt_ = now;
        target_ = cursor_ < 0 ? base_ : entries_[cursor_].data + (code ? 0 : FrameBytes);
        ++revision_;
    }
};
} // namespace pixelpad
