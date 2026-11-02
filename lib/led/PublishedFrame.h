#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace pixelpad {
// Caller holds the display mutex. Scan snapshots never observe partial drawing.
template<size_t Width, size_t Height> class PublishedFrame {
public:
    static constexpr size_t Bytes=Width*Height*3;
    void setPixel(int x,int y,uint8_t r,uint8_t g,uint8_t b) {
        if (x<0 || y<0 || size_t(x)>=Width || size_t(y)>=Height) return;
        uint8_t* p=drawing_+(y*Width+x)*3;
        if (p[0]!=r || p[1]!=g || p[2]!=b) {
            p[0]=r; p[1]=g; p[2]=b; dirty_=true;
        }
    }
    void clear() { memset(drawing_,0,Bytes); dirty_=true; }
    void copy(const uint8_t* source) {
        if (!source) return;
        if (memcmp(drawing_, source, Bytes) != 0) {
            memcpy(drawing_, source, Bytes);
            dirty_ = true;
        }
    }
    bool commit() {
        if (!dirty_) return false;
        memcpy(published_,drawing_,Bytes); dirty_=false; return true;
    }
    void snapshot(uint8_t* destination) const { memcpy(destination,published_,Bytes); }
private:
    uint8_t drawing_[Bytes]{}, published_[Bytes]{};
    bool dirty_=false;
};
}
