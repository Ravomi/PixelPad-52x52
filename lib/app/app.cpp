#include <esp_system.h>
#include "app.h"
#include "Assets.h" 
#include "GifAssets.h" 
#include "Bluetooth.h"
#include "StandbyStore.h"

extern LED ledDisplay;
extern Bluetooth ble;

static const uint8_t tinyFont3x5[][3] PROGMEM = {
  {0x00,0x00,0x00}, // space
  {0x00,0x17,0x00}, // !
  {0x03,0x00,0x03}, // "
  {0x1F,0x0A,0x1F}, // #
  {0x12,0x1F,0x09}, // $
  {0x19,0x04,0x13}, // %
  {0x16,0x15,0x0D}, // &
  {0x00,0x03,0x00}, // '
  {0x00,0x0E,0x11}, // (
  {0x11,0x0E,0x00}, // )
  {0x0A,0x04,0x0A}, // *
  {0x04,0x0E,0x04}, // +
  {0x00,0x10,0x08}, // ,
  {0x04,0x04,0x04}, // -
  {0x00,0x10,0x00}, // .
  {0x10,0x08,0x04}, // /
  {0x1F,0x11,0x1F}, // 0
  {0x00,0x1F,0x00}, // 1
  {0x1D,0x15,0x17}, // 2
  {0x15,0x15,0x1F}, // 3
  {0x07,0x04,0x1F}, // 4
  {0x17,0x15,0x1D}, // 5
  {0x1F,0x15,0x1D}, // 6
  {0x01,0x01,0x1F}, // 7
  {0x1F,0x15,0x1F}, // 8
  {0x17,0x15,0x1F}, // 9
  {0x00,0x14,0x00}, // :
  {0x00,0x14,0x08}, // ;
  {0x08,0x14,0x22}, // <
  {0x0A,0x0A,0x0A}, // =
  {0x22,0x14,0x08}, // >
  {0x01,0x15,0x07}, // ?
  {0x1F,0x11,0x17}, // @
  {0x1E,0x05,0x1E}, // A
  {0x1F,0x15,0x0A}, // B
  {0x0E,0x11,0x11}, // C
  {0x1F,0x11,0x0E}, // D
  {0x1F,0x15,0x11}, // E
  {0x1F,0x05,0x01}, // F
  {0x0E,0x15,0x1D}, // G
  {0x1F,0x04,0x1F}, // H
  {0x11,0x1F,0x11}, // I
  {0x08,0x10,0x0F}, // J
  {0x1F,0x04,0x1B}, // K
  {0x1F,0x10,0x10}, // L
  {0x1F,0x02,0x1F}, // M
  {0x1F,0x06,0x1F}, // N
  {0x0E,0x11,0x0E}, // O
  {0x1F,0x05,0x02}, // P
  {0x0E,0x15,0x1E}, // Q
  {0x1F,0x05,0x1A}, // R
  {0x12,0x15,0x09}, // S
  {0x01,0x1F,0x01}, // T
  {0x0F,0x10,0x0F}, // U
  {0x07,0x18,0x07}, // V
  {0x1F,0x10,0x1F}, // W
  {0x1B,0x04,0x1B}, // X
  {0x03,0x1C,0x03}, // Y
  {0x19,0x15,0x13}, // Z
  {0x00,0x1F,0x11}, // [
  {0x04,0x08,0x10}, // backslash
  {0x11,0x1F,0x00}, // ]
  {0x02,0x01,0x02}, // ^
  {0x10,0x10,0x10}, // _
  {0x00,0x01,0x02}, // `
};

static int tinyStringWidth(const String& str) {
    if (str.length() == 0) return 0;
    return str.length() * 4 - 1;
}

static String bootNameForDisplay(const String& rawName) {
    String name = rawName;
    name.trim();
    if (name.length() == 0) return "PixelPad";
    return name;
}

static void drawTinyCharReveal(int x, int y, char c, int revealRight,
                               uint8_t r, uint8_t g, uint8_t b) {
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c < 32 || c > 96) return;

    int idx = c - 32;
    for (int colIndex = 0; colIndex < 3; colIndex++) {
        int px = x + colIndex;
        if (px >= revealRight) continue;

        uint8_t col = pgm_read_byte(&tinyFont3x5[idx][colIndex]);
        for (int row = 0; row < 5; row++) {
            if ((col & (1 << row)) == 0) continue;
            ledDisplay.setPixel(px, y + row, r, g, b);
        }
    }
}

static void drawTinyStringReveal(int x, int y, const String& str,
                                 int revealColumns, uint8_t r, uint8_t g, uint8_t b) {
    const int revealRight = x + revealColumns;
    for (size_t i = 0; i < str.length(); i++) {
        int charX = x + i * 4;
        drawTinyCharReveal(charX, y, str[i], revealRight, r, g, b);
    }
}

static int typingRevealColumns(int progress, int limit, int totalProgress) {
    if (limit <= 0 || progress <= 0) return 0;
    if (progress >= totalProgress) return limit;

    uint32_t totalWeight = 0;
    for (int i = 0; i < limit; ++i) {
        totalWeight += 1 + ((i % 7 == 2 || i % 11 == 5) ? 2 : (i % 5 == 0));
    }
    const uint32_t elapsed = uint32_t(uint64_t(progress) * totalWeight / totalProgress);
    uint32_t spent = 0;
    int columns = 0;
    while (columns < limit) {
        const uint32_t weight = 1 + ((columns % 7 == 2 || columns % 11 == 5)
                                     ? 2 : (columns % 5 == 0));
        if (spent + weight > elapsed) break;
        spent += weight;
        ++columns;
    }
    return columns;
}

App::App() {
    _state = ANIM_IDLE;
    _lastFrameTime = 0;
    _frameInterval = 30;
    _lastActivityTime = millis();
    _bootStartTime = 0;
    
    // 申请内存
    _pixelIndices = new uint16_t[PIXEL_COUNT];
    _particles = new Particle[NUM_PARTICLES];
    _effectProgress = 0;
    _nameRevealColumns = 0;
    _bootDisplayName = "PixelPad";
    _customFrames = nullptr;
    _customFrameCount = 0;
    _customFps = 8;
}

App::~App() {
    if (_pixelIndices) delete[] _pixelIndices;
    if (_particles) delete[] _particles;
    if (_customFrames) free(_customFrames);
}

void App::begin() {
    randomSeed(esp_random());
    for (int i = 0; i < PIXEL_COUNT; i++) _pixelIndices[i] = i;

    pixelpad::mountStandbyStore();
    _customFrames = pixelpad::loadStandby(_customFrameCount, _customFps);

    Serial.println("✅ App 动画管理器已启动");
    updateActivity();
}

void App::_shuffleIndices() {
    for (int i = PIXEL_COUNT - 1; i > 0; i--) {
        int j = random(0, i + 1);
        uint16_t temp = _pixelIndices[i];
        _pixelIndices[i] = _pixelIndices[j];
        _pixelIndices[j] = temp;
    }
}

void App::updateActivity() {
    _lastActivityTime = millis();
    // 如果在待机模式（粒子或GIF），收到操作就停止并清屏
    if (_state == ANIM_STANDBY || _state == ANIM_GIF) {
        stop(); 
    }
    // 如果在Logo保持状态，收到操作就播放旋转唤醒
    else if (_state == ANIM_LOGO_HOLD) {
        playWakeupAnim(); 
    }
}

void App::loop() {
    unsigned long now = millis();

    // Keep the last displayed content on screen while idle. Standby playback
    // is now opt-in through the explicit `standby` command only.
    if (_state == ANIM_IDLE) {
        return;
    }

    // 2. 帧率控制
    if (now - _lastFrameTime >= _frameInterval) {
        _lastFrameTime = now;

        if (_state == ANIM_LOGO_HOLD) {
            ledDisplay.refreshDisplay();
            return;
        }

        switch (_state) {
            case ANIM_BOOT:    _updateParticleEffect(); break;
            case ANIM_NAME_REVEAL: break;
            case ANIM_WAKEUP:  _playRotationFrame(); break;
            case ANIM_STANDBY: _drawStandbyAnim(); break; // 粒子
            case ANIM_GIF:     _drawGifAnim(); break;     // GIF
            default: break;
        }
        
        ledDisplay.refreshDisplay();
    }
}

// ==========================================
// ★ 动画实现：GIF 待机
// ==========================================
void App::_drawGifAnim() {
    // ★ 优先使用自定义帧 (PSRAM)
    if (_customFrames != nullptr && _customFrameCount > 0) {
        const int frameSize = 52 * 52 * 3;
        const uint8_t* framePtr = _customFrames + (_currentFrame * frameSize);
        int pixelIdx = 0;
        for (int y = 0; y < 52; y++) {
            for (int x = 0; x < 52; x++) {
                uint8_t r = framePtr[pixelIdx++];
                uint8_t g = framePtr[pixelIdx++];
                uint8_t b = framePtr[pixelIdx++];
                ledDisplay.setPixel(x, y, r, g, b);
            }
        }
        _currentFrame++;
        if (_currentFrame >= _customFrameCount) {
            _currentFrame = 0;
        }
        return;
    }

    // 回退到内置 PROGMEM GIF
    const uint8_t* framePtr = (const uint8_t*)pgm_read_ptr(&(GIF_ANIM[_currentFrame]));

    int pixelIdx = 0;
    for (int y = 0; y < 52; y++) {
        for (int x = 0; x < 52; x++) {
            uint8_t r = pgm_read_byte(framePtr + pixelIdx++);
            uint8_t g = pgm_read_byte(framePtr + pixelIdx++);
            uint8_t b = pgm_read_byte(framePtr + pixelIdx++);
            ledDisplay.setPixel(x, y, r, g, b);
        }
    }

    _currentFrame++;
    if (_currentFrame >= GIF_TOTAL_FRAMES) {
        _currentFrame = 0;
    }
}

// ==========================================
// ★ 动画实现：粒子物理待机 (修复报错的关键)
// ==========================================
void App::_drawStandbyAnim() {
    ledDisplay.clearScreen(); 
    float t = millis() * 0.002;
    // 背景装饰波浪
    for (int x = 0; x < 52; x++) {
        int y1 = (int)(45 + sin(t + x * 0.1) * 3);
        if (y1 >= 0 && y1 < 52) ledDisplay.setPixel(x, y1, 0, 0, 40);
    }
    // 粒子物理计算
    for (int i = 0; i < NUM_PARTICLES; i++) {
        _particles[i].x += _particles[i].vx;
        _particles[i].y += _particles[i].vy;
        _particles[i].vy += 0.15; // 重力
        
        if (_particles[i].x <= 0 || _particles[i].x >= 51) {
            _particles[i].x = constrain(_particles[i].x, 0, 51);
            _particles[i].vx *= -0.7;
        }
        if (_particles[i].y >= 51 || _particles[i].y <= 0) {
            _particles[i].y = constrain(_particles[i].y, 0, 51);
            _particles[i].vy *= -0.7;
        }
        _particles[i].life--;
        if (_particles[i].life <= 0) _resetParticle(i);
        
        float fade = _particles[i].life / _particles[i].maxLife;
        ledDisplay.setPixel((int)_particles[i].x, (int)_particles[i].y, 
                            _particles[i].r * fade, _particles[i].g * fade, _particles[i].b * fade);
    }
}

void App::_resetParticle(int i) {
    _particles[i].x = 26; _particles[i].y = 20;
    float angle = random(360) * 0.01745;
    float speed = 0.8 + random(100) / 80.0;
    _particles[i].vx = cos(angle) * speed;
    _particles[i].vy = sin(angle) * speed;
    _particles[i].maxLife = 40 + random(60);
    _particles[i].life = _particles[i].maxLife;
    int h = random(360);
    _particles[i].r = 255 * (0.5 + 0.5 * cos(h * 0.0174));
    _particles[i].g = 255 * (0.5 + 0.5 * cos((h + 120) * 0.0174));
    _particles[i].b = 255 * (0.5 + 0.5 * cos((h + 240) * 0.0174));
}

// ==========================================
// 其他功能函数
// ==========================================

void App::playBootAnim() {
    ledDisplay.clearScreen(); 
    _shuffleIndices();        
    _state = ANIM_BOOT;
    _effectProgress = 0;
    _frameInterval = 22;
    _bootStartTime = millis();
    _nameRevealColumns = 0;
    _bootDisplayName = bootNameForDisplay(ble.getDeviceName());
}

void App::playStandbyAnim() {
    // 自动判断：有自定义帧放自定义GIF，否则放内置GIF
    _state = ANIM_GIF;
    _currentFrame = 0;
    if (_customFrames != nullptr && _customFrameCount > 0) {
        _frameInterval = 1000 / _customFps;
        _totalFrames = _customFrameCount;
    } else {
        _frameInterval = 80;
        _totalFrames = GIF_TOTAL_FRAMES;
    }
}

void App::_updateParticleEffect() {
    constexpr uint32_t kLogoDurationMs = 850;
    const uint32_t elapsed = millis() - _bootStartTime;
    const uint32_t clamped = elapsed >= kLogoDurationMs ? kLogoDurationMs : elapsed;
    const uint32_t t = clamped * 65535u / kLogoDurationMs;
    const uint32_t remainingT = 65535u - t;
    const uint32_t eased = 65535u - static_cast<uint32_t>(
        (static_cast<uint64_t>(remainingT) * remainingT * remainingT) >> 32);
    const int targetProgress = static_cast<int>(
        (static_cast<uint64_t>(PIXEL_COUNT) * eased + 32767u) / 65535u);

    while (_effectProgress < targetProgress) {
        int idx = _pixelIndices[_effectProgress];
        uint8_t r = pgm_read_byte(&LOGO_STATIC[idx * 3]);
        uint8_t g = pgm_read_byte(&LOGO_STATIC[idx * 3 + 1]);
        uint8_t b = pgm_read_byte(&LOGO_STATIC[idx * 3 + 2]);
        ledDisplay.setPixel(idx % 52, idx / 52, r, g, b);
        _effectProgress++;
    }

    const int textWidth = tinyStringWidth(_bootDisplayName);
    int startX = (52 - textWidth) / 2;
    if (startX < 0) startX = 0;
    for (int ty = 44; ty < 51; ++ty) {
        for (int tx = 0; tx < 52; ++tx) ledDisplay.setPixel(tx, ty, 0, 0, 0);
    }
    const int revealLimit = textWidth + 2;
    const int typingStart = PIXEL_COUNT / 8;
    _nameRevealColumns = typingRevealColumns(_effectProgress - typingStart,
                                             revealLimit,
                                             PIXEL_COUNT - typingStart);
    drawTinyStringReveal(startX, 45, _bootDisplayName,
                         _nameRevealColumns, 255, 255, 255);

    if (_effectProgress >= PIXEL_COUNT) {
        if (_state != ANIM_LOGO_HOLD) {
            _state = _nameRevealColumns >= textWidth + 2
                         ? ANIM_LOGO_HOLD : ANIM_NAME_REVEAL;
            if (_state == ANIM_NAME_REVEAL) _frameInterval = 75;
        }
    }
}

void App::playWakeupAnim() {
    if (_state == ANIM_WAKEUP) return;
    _state = ANIM_WAKEUP;
    _currentFrame = 0;
    _totalFrames = ROTATION_FRAME_COUNT; 
    _frameInterval = 30; 
}

void App::_playRotationFrame() {
    if (_currentFrame >= _totalFrames) { stop(); return; }
    const uint8_t* ptr = (const uint8_t*)pgm_read_ptr(&(ROTATION_ANIM[_currentFrame]));
    int idx = 0;
    for (int y = 0; y < 52; y++) {
        for (int x = 0; x < 52; x++) {
            ledDisplay.setPixel(x, y, pgm_read_byte(ptr + idx), pgm_read_byte(ptr + idx + 1), pgm_read_byte(ptr + idx + 2));
            idx += 3;
        }
    }
    _currentFrame++;
}

void App::stop() {
    _state = ANIM_IDLE;
    ledDisplay.clearScreen();
    ledDisplay.refreshDisplay();
}

bool App::isAnimating() { return _state != ANIM_IDLE; }
bool App::isLogoState() { return _state == ANIM_LOGO_HOLD; }

// ==========================================
// ★ 自定义待机帧管理
// ==========================================
void App::adoptCustomStandby(int frameCount, int fps, uint8_t* rgbData) {
    free(_customFrames);
    _customFrames = rgbData;
    _customFrameCount = frameCount;
    _customFps = fps;
    _currentFrame = 0;
}

void App::clearCustomStandby() {
    free(_customFrames);
    _customFrames = nullptr;
    _customFrameCount = 0;
    _customFps = 8;
    _currentFrame = 0;
}
