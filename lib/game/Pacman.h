#ifndef PACMAN_H
#define PACMAN_H

#include <Arduino.h>
#include <vector>
#include <queue>
#include "led.h"

#define TILE_EMPTY 0
#define TILE_WALL  1
#define TILE_DOT   2

struct Point {
    int x, y;
    bool operator==(const Point& other) const { return x == other.x && y == other.y; }
    bool operator!=(const Point& other) const { return !(*this == other); }
};

struct Entity {
    int x, y, prevX, prevY, dirX, dirY;
    uint8_t r, g, b;    
    int speedCounter, speedThreshold, id;
};

class PacmanGame {
public:
    void begin();
    void loop();
    void handleInput(String key);

private:
    bool deathPending = false;
    uint32_t deathAt = 0;
    bool isRunning = false;
    bool isIntro = false;
    float introRadius = 0;
    byte map[52][52]; 
    int score = 0;
    Entity player;
    static const int GHOST_COUNT = 3; 
    Entity ghosts[GHOST_COUNT];
    int _inputDirX = 0, _inputDirY = 0;
    bool _hasInput = false;

    void generateMap();
    void resetLevel();
    void updatePlayer();
    void updateGhost(Entity &ghost);
    bool canMove(int x, int y);
    Point solveBFS(Point start, Point target);
    bool isOpposite(int dx1, int dy1, int dx2, int dy2);
    void drawStaticMap();
    void drawBitmap(int cx, int cy, const byte* bmp, uint8_t r, uint8_t g, uint8_t b);
    void drawEntity(Entity &e);
    void clearEntityTrail(Entity &e);
};

#endif
