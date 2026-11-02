#ifndef SAND_TETRIS_H
#define SAND_TETRIS_H

#include <Arduino.h>
#include "led.h"

// 游戏区域：全屏 52x52
#define GAME_W 52      
#define GAME_H 52      
#define OFFSET_X 0
#define OFFSET_Y 0

// 方块放大倍数
#define BLOCK_SCALE 3

// 颜色定义
#define COLOR_EMPTY 0
#define COLOR_WALL  4 

struct Piece {
    int x, y;           
    int type;           
    int rotation;       
    uint8_t colorId;    
};

class SandTetrisGame {
public:
    void begin();
    void loop();
    void handleInput(String key);

private:
    bool isRunning = false;
    bool isGameOver = false;
    
    // 时间控制变量
    unsigned long lastDropTime = 0;
    unsigned long lastPhysicsTime = 0;
    unsigned long lastDrawTime = 0; 
    
    int dropInterval = 120; // 平滑下落间隔

    uint8_t grid[GAME_W][GAME_H];
    
    Piece currentPiece;
    Piece nextPiece;
    int score = 0;

    void initGrid();          
    void spawnPiece();        
    bool movePiece(int dx, int dy); 
    void rotatePiece();       
    void lockPiece();         
    
    void updateSandPhysics(); 
    void checkLines();        
    bool checkCollision(int px, int py, int pType, int pRot);
    void draw();
};

#endif
