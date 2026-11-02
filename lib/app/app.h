#ifndef APP_H
#define APP_H

#include <Arduino.h>
#include "led.h"

extern LED ledDisplay;

class App {
public:
    enum AnimState {
        ANIM_IDLE,      // 空闲
        ANIM_BOOT,      // 开机(粒子凝聚)
        ANIM_NAME_REVEAL, // 设备名从左到右打出
        ANIM_LOGO_HOLD, // Logo保持
        ANIM_WAKEUP,    // 唤醒(3D旋转)
        ANIM_STANDBY,   // 待机(物理粒子)
        ANIM_GIF        // ★ 新增：GIF 待机模式
    };

private:
    AnimState _state;
    unsigned long _lastFrameTime;
    int _frameInterval;
    
    unsigned long _lastActivityTime;

    // 开机粒子
    static const int PIXEL_COUNT = 52 * 52;
    uint16_t* _pixelIndices; 
    int _effectProgress;     
    int _pixelsPerFrame;     
    int _nameRevealColumns;
    String _bootDisplayName;

    // 唤醒动画/GIF通用
    int _currentFrame;
    int _totalFrames;

    // ★ 自定义待机帧 (PSRAM)
    uint8_t* _customFrames;     // PSRAM 中的帧数据 (每帧 52*52*3 字节 RGB)
    int _customFrameCount;      // 自定义帧总数 (0=使用内置GIF)
    int _customFps;             // 自定义帧率

    // 待机物理粒子结构
    struct Particle {
        float x, y;
        float vx, vy;
        float life, maxLife;
        uint8_t r, g, b;
    };
    static const int NUM_PARTICLES = 60; 
    Particle* _particles;

    // 内部动画更新函数
    void _updateParticleEffect(); 
    void _playRotationFrame();    
    void _drawStandbyAnim();      // 粒子待机
    void _drawGifAnim();          // ★ GIF待机
    void _shuffleIndices();       
    void _resetParticle(int index); 

public:
    App();
    ~App(); 
    
    void begin();
    void loop(); 
    void updateActivity(); 

    void playBootAnim();    
    void playWakeupAnim();  
    void playStandbyAnim(); // 这里会自动判断去放 GIF 还是粒子
    
    void stop();            
    bool isAnimating();     
    bool isLogoState();

    // ★ 从 BLE 设置自定义待机帧数据
    // Takes ownership of validated PSRAM data. Called only by the render task.
    void adoptCustomStandby(int frameCount, int fps, uint8_t* rgbData);
    void clearCustomStandby();
    bool hasCustomStandby() const { return _customFrameCount > 0 && _customFrames != nullptr; }     
};

#endif
