#include <esp_system.h>
#include "Pacman.h"
#include "Sprites.h" 

extern LED ledDisplay;

bool PacmanGame::isOpposite(int dx1, int dy1, int dx2, int dy2) {
    return (dx1 == -dx2 && dy1 == -dy2);
}

void PacmanGame::begin() {
    deathPending = false;
    randomSeed(esp_random());
    generateMap();
    ledDisplay.clearScreen(); 
    resetLevel();
    score = 0;
    isIntro = true;    
    introRadius = 0;   
    isRunning = true;
}

void PacmanGame::resetLevel() {
    player.x = 26; player.y = 26; player.prevX = 26; player.prevY = 26;
    player.dirX = 1; player.dirY = 0; 
    player.r = 255; player.g = 255; player.b = 0; player.speedThreshold = 0; 
    _inputDirX = 0; _inputDirY = 0; _hasInput = false;

    uint8_t colors[3][3] = {{255,0,0}, {255,100,100}, {0,255,255}};
    int startPos[3][2] = {{4,4}, {47,4}, {4,47}}; 

    for(int i=0; i<GHOST_COUNT; i++) {
        ghosts[i].id = i;
        ghosts[i].x = startPos[i][0]; ghosts[i].y = startPos[i][1];
        ghosts[i].prevX = ghosts[i].x; ghosts[i].prevY = ghosts[i].y;
        ghosts[i].dirX = 0; ghosts[i].dirY = 0;
        ghosts[i].r = colors[i][0]; ghosts[i].g = colors[i][1]; ghosts[i].b = colors[i][2];
        ghosts[i].speedCounter = 0; ghosts[i].speedThreshold = 2 + i; 
    }
}

void PacmanGame::generateMap() {
    for(int x=0; x<52; x++) for(int y=0; y<52; y++) map[x][y] = TILE_DOT;
    for(int i=0; i<52; i++) {
        map[i][0] = TILE_WALL; map[i][1] = TILE_WALL;
        map[i][50] = TILE_WALL; map[i][51] = TILE_WALL;
        map[0][i] = TILE_WALL; map[1][i] = TILE_WALL;
        map[50][i] = TILE_WALL; map[51][i] = TILE_WALL;
    }
    for(int x=6; x<46; x+=6) {
        for(int y=6; y<46; y+=6) {
            if (random(100) < 70) {
                bool horizontal = random(2);
                int len = random(3, 8); 
                for(int k=0; k<len; k++) {
                    int wx = x + (horizontal ? k : 0);
                    int wy = y + (horizontal ? 0 : k);
                    if (wx < 48 && wy < 48 && abs(wx-26) > 5 && abs(wy-26) > 5) {
                        map[wx][wy] = TILE_WALL; map[wx+1][wy] = TILE_WALL;
                        map[wx][wy+1] = TILE_WALL; map[wx+1][wy+1] = TILE_WALL;
                    }
                }
            }
        }
    }
    for(int x=22; x<=30; x++) for(int y=22; y<=30; y++) map[x][y] = TILE_EMPTY;
}

Point PacmanGame::solveBFS(Point start, Point target) {
    if (start.x == target.x && start.y == target.y) return start;
    static int16_t parent[2704]; 
    for(int i=0; i<2704; i++) parent[i] = -1;
    std::queue<int> q;
    int startIdx = start.y * 52 + start.x;
    int targetIdx = target.y * 52 + target.x;
    q.push(startIdx); parent[startIdx] = -2; 
    bool found = false;
    while (!q.empty()) {
        int curr = q.front(); q.pop();
        if (curr == targetIdx) { found = true; break; }
        int cx = curr % 52, cy = curr / 52;
        int d[4][2] = {{0,-1}, {0,1}, {-1,0}, {1,0}};
        for (int i=0; i<4; i++) {
            int nx = cx + d[i][0], ny = cy + d[i][1];
            int nIdx = ny * 52 + nx;
            if (nx >= 2 && nx < 50 && ny >= 2 && ny < 50) {
                if (parent[nIdx] == -1 && map[nx][ny] != TILE_WALL) {
                    parent[nIdx] = curr; q.push(nIdx);
                    if (nIdx == targetIdx) { found = true; goto end_bfs; }
                }
            }
        }
    }
    end_bfs:
    if (found) {
        int curr = targetIdx;
        while (parent[curr] != startIdx) {
            curr = parent[curr];
            if (curr == -2 || curr == -1) return start; 
        }
        return {curr % 52, curr / 52};
    }
    return start; 
}

void PacmanGame::handleInput(String key) {
    if (isIntro) return;
    _hasInput = true; 
    if (key == "UP")    { _inputDirX = 0;  _inputDirY = -1; }
    else if (key == "DOWN")  { _inputDirX = 0;  _inputDirY = 1;  }
    else if (key == "LEFT")  { _inputDirX = -1; _inputDirY = 0;  }
    else if (key == "RIGHT") { _inputDirX = 1;  _inputDirY = 0;  }
    else if (key == "B") begin(); 
}

bool PacmanGame::canMove(int x, int y) {
    int r = 1; // 缩小碰撞盒到 3x3，防卡死
    if (x-r < 0 || x+r >= 52 || y-r < 0 || y+r >= 52) return false;
    for (int i = -r; i <= r; i++) {
        for (int j = -r; j <= r; j++) {
            if (map[x+i][y+j] == TILE_WALL) return false; 
        }
    }
    return true;
}

void PacmanGame::updatePlayer() {
    if (!_hasInput) return; 
    if (_inputDirX != 0 || _inputDirY != 0) { player.dirX = _inputDirX; player.dirY = _inputDirY; }
    for (int s = 0; s < 3; s++) { // 微步移动
        int nx = player.x + _inputDirX, ny = player.y + _inputDirY;
        if (canMove(nx, ny)) {
            player.x = nx; player.y = ny;
            for(int i=-1; i<=1; i++) for(int j=-1; j<=1; j++) {
                if (map[player.x+i][player.y+j] == TILE_DOT) {
                    map[player.x+i][player.y+j] = TILE_EMPTY; score++;
                }
            }
        } else break; 
    }
    _hasInput = false; 
}

void PacmanGame::updateGhost(Entity &ghost) {
    ghost.speedCounter++;
    if (ghost.speedCounter < ghost.speedThreshold) return;
    ghost.speedCounter = 0;
    int dx = 0, dy = 0;
    if (ghost.id == 0) { // Hunter BFS
        Point next = solveBFS({ghost.x, ghost.y}, {player.x, player.y});
        dx = next.x - ghost.x; dy = next.y - ghost.y;
    } 
    else if (ghost.id == 1) { // Ambusher
        int tx = player.x + player.dirX * 8, ty = player.y + player.dirY * 8;
        if (tx > ghost.x) dx = 1; else if (tx < ghost.x) dx = -1;
        else if (ty > ghost.y) dy = 1; else if (ty < ghost.y) dy = -1;
        if (!canMove(ghost.x+dx, ghost.y+dy)) if(random(2)) dx=(dx==0)?1:-1; else dy=(dy==0)?1:-1;
    }
    else { // Random
        if (random(100) < 10) { ghost.dirX = random(-1, 2); ghost.dirY = random(-1, 2); }
        dx = ghost.dirX; dy = ghost.dirY;
        if ((dx==0&&dy==0) || !canMove(ghost.x+dx, ghost.y+dy)) {
             int r = random(4); dx=(r==0)?1:(r==1)?-1:0; dy=(r==2)?1:(r==3)?-1:0; ghost.dirX=dx; ghost.dirY=dy;
        }
    }
    if (canMove(ghost.x+dx, ghost.y+dy)) { ghost.x+=dx; ghost.y+=dy; ghost.dirX=dx; ghost.dirY=dy; }
    if (abs(ghost.x-player.x)<4 && abs(ghost.y-player.y)<4) { ledDisplay.turnOff(); deathPending = true; deathAt = millis(); }
}

void PacmanGame::drawBitmap(int cx, int cy, const byte* bmp, uint8_t r, uint8_t g, uint8_t b) {
    int s = 5, off = 2;     
    for (int i = 0; i < s; i++) for (int j = 0; j < s; j++) {
        if (bmp[j*s+i]) {
            int dx = cx-off+i, dy = cy-off+j;
            if (dx>=0 && dx<52 && dy>=0 && dy<52) ledDisplay.setPixel(dx, dy, r, g, b);
        }
    }
}

void PacmanGame::clearEntityTrail(Entity &e) {
    int s = 5, off = 2;
    for (int i = 0; i < s; i++) for (int j = 0; j < s; j++) {
        int ox = e.prevX-off+i, oy = e.prevY-off+j;
        if (ox>=0 && ox<52 && oy>=0 && oy<52) {
            if (map[ox][oy] == TILE_DOT) ledDisplay.setPixel(ox, oy, 40, 40, 40); 
            else if (map[ox][oy] == TILE_WALL) ledDisplay.setPixel(ox, oy, 0, 0, 80);   
            else ledDisplay.setPixel(ox, oy, 0, 0, 0);    
        }
    }
}

void PacmanGame::drawEntity(Entity &e) {
    if (e.r == 255 && e.g == 255 && e.b == 0) {
        if ((millis()/150)%2==0) drawBitmap(e.x, e.y, PACMAN_FULL, e.r, e.g, e.b);
        else {
            if (e.dirX==1) drawBitmap(e.x, e.y, PACMAN_OPEN_R, e.r, e.g, e.b);
            else if (e.dirX==-1) drawBitmap(e.x, e.y, PACMAN_OPEN_L, e.r, e.g, e.b);
            else if (e.dirY==-1) drawBitmap(e.x, e.y, PACMAN_OPEN_U, e.r, e.g, e.b);
            else drawBitmap(e.x, e.y, PACMAN_OPEN_D, e.r, e.g, e.b);
        }
    } else drawBitmap(e.x, e.y, GHOST_SPRITE, e.r, e.g, e.b);
    e.prevX = e.x; e.prevY = e.y;
}

void PacmanGame::drawStaticMap() {
    ledDisplay.clearScreen();
    for(int x=0; x<52; x++) for(int y=0; y<52; y++) {
        if (map[x][y] == TILE_WALL) ledDisplay.setPixel(x, y, 0, 0, 80); 
        else if (map[x][y] == TILE_DOT) ledDisplay.setPixel(x, y, 40, 40, 40); 
    }
}

void PacmanGame::loop() {
    if (!isRunning) return;
    if (deathPending) {
        if (millis() - deathAt >= 500) begin();
        return;
    }
    
    if (isIntro) {
        static unsigned long lastAnim = 0;
        if (millis() - lastAnim > 20) {
            lastAnim = millis();
            introRadius += 1.5; 
            int cx = 26, cy = 26, r2 = introRadius * introRadius; 
            for(int x=0; x<52; x++) for(int y=0; y<52; y++) {
                if ((x-cx)*(x-cx) + (y-cy)*(y-cy) < r2) {
                     if (map[x][y] == TILE_WALL) ledDisplay.setPixel(x, y, 0, 0, 80); 
                     else if (map[x][y] == TILE_DOT) ledDisplay.setPixel(x, y, 40, 40, 40); 
                }
            }
            drawEntity(player);
            ledDisplay.refreshDisplay();
            if (introRadius > 45) { isIntro = false; drawStaticMap(); drawEntity(player); }
        }
        return; 
    }

    static unsigned long lastFrame = 0;
    if (millis() - lastFrame > 33) { 
        lastFrame = millis();
        // 1. 全员擦除
        clearEntityTrail(player);
        for(int i=0; i<GHOST_COUNT; i++) clearEntityTrail(ghosts[i]);
        // 2. 逻辑更新
        updatePlayer();
        for(int i=0; i<GHOST_COUNT; i++) {
            updateGhost(ghosts[i]);
            if (deathPending) return;
        }
        // 3. 全员绘制 (层级: 鬼->人)
        for(int i=0; i<GHOST_COUNT; i++) drawEntity(ghosts[i]);
        drawEntity(player);
    }
    
    // ★★★ 屏幕刷新放在最外层，全速运行，防止闪烁 ★★★
    ledDisplay.refreshDisplay();
}