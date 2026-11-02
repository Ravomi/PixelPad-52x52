#ifndef GAME_OF_LIFE_H
#define GAME_OF_LIFE_H

#include <Arduino.h>
#include "led.h"

class GameOfLife {
public:
    void begin();
    void loop(); 
    void handleInput(String key); // 处理手柄指令

private:
    int lastCursorPhase = -1;
    bool isRunning = false; // true=运行中, false=编辑中
    
    // 游戏地图 (52x52)
    bool grid[52][52];
    bool nextGrid[52][52];

    // 光标位置
    int cursorX = 26;
    int cursorY = 26;

    unsigned long lastUpdate = 0;
    unsigned long speed = 100; // 演化速度 (越小越快)

    // 辅助函数
    int countNeighbors(int x, int y);
    void computeNextGeneration();
    void draw();
};

#endif
