#include "SandTetris.h"
#include <BLEDevice.h> // ★ 引入蓝牙库

// ★ 引入在 bluetooth.cpp 中定义好的蓝牙发送特征值
#include "Bluetooth.h"
extern Bluetooth ble;

extern LED ledDisplay;

// 方块形状数据
const uint8_t TETROMINOES[7][4][4][4] = {
    { {{0,0,0,0},{1,1,1,1},{0,0,0,0},{0,0,0,0}}, {{0,0,1,0},{0,0,1,0},{0,0,1,0},{0,0,1,0}}, {{0,0,0,0},{1,1,1,1},{0,0,0,0},{0,0,0,0}}, {{0,1,0,0},{0,1,0,0},{0,1,0,0},{0,1,0,0}} },
    { {{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}}, {{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}}, {{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}}, {{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}} },
    { {{0,1,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}}, {{0,1,0,0},{0,1,1,0},{0,1,0,0},{0,0,0,0}}, {{0,0,0,0},{1,1,1,0},{0,1,0,0},{0,0,0,0}}, {{0,1,0,0},{1,1,0,0},{0,1,0,0},{0,0,0,0}} },
    { {{0,1,1,0},{1,1,0,0},{0,0,0,0},{0,0,0,0}}, {{0,1,0,0},{0,1,1,0},{0,0,1,0},{0,0,0,0}}, {{0,0,0,0},{0,1,1,0},{1,1,0,0},{0,0,0,0}}, {{1,0,0,0},{1,1,0,0},{0,1,0,0},{0,0,0,0}} },
    { {{1,1,0,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}}, {{0,0,1,0},{0,1,1,0},{0,1,0,0},{0,0,0,0}}, {{0,0,0,0},{1,1,0,0},{0,1,1,0},{0,0,0,0}}, {{0,1,0,0},{1,1,0,0},{1,0,0,0},{0,0,0,0}} },
    { {{1,0,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}}, {{0,1,1,0},{0,1,0,0},{0,1,0,0},{0,0,0,0}}, {{0,0,0,0},{1,1,1,0},{0,0,1,0},{0,0,0,0}}, {{0,1,0,0},{0,1,0,0},{1,1,0,0},{0,0,0,0}} },
    { {{0,0,1,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}}, {{0,1,0,0},{0,1,0,0},{0,1,1,0},{0,0,0,0}}, {{0,0,0,0},{1,1,1,0},{1,0,0,0},{0,0,0,0}}, {{1,1,0,0},{0,0,1,0},{0,0,1,0},{0,0,0,0}} }
};

// 调色板 (3 种颜色)
const uint8_t COLORS[5][3] = {
    {0,0,0},       // 0: 空白
    {0,255,255},   // 1: 青色 
    {255,0,255},   // 2: 品红 
    {255,255,0},   // 3: 黄色 
    {50,50,50}     // 4: 墙壁 
};

void SandTetrisGame::begin() {
    randomSeed(millis());
    initGrid();
    score = 0;
    isGameOver = false;
    isRunning = true;
    dropInterval = 120; 
    spawnPiece();
    
    draw(); 
}

void SandTetrisGame::initGrid() {
    for (int x = 0; x < GAME_W; x++) {
        for (int y = 0; y < GAME_H; y++) {
            grid[x][y] = COLOR_EMPTY;
        }
    }
}

void SandTetrisGame::spawnPiece() {
    currentPiece.type = random(7);
    currentPiece.rotation = 0;
    currentPiece.x = (GAME_W / 2) - (2 * BLOCK_SCALE); 
    currentPiece.y = 0;
    
    // 只在 1, 2, 3 中随机
    currentPiece.colorId = random(1, 4); 

    if (checkCollision(currentPiece.x, currentPiece.y, currentPiece.type, currentPiece.rotation)) {
        isGameOver = true;
    }
}

bool SandTetrisGame::checkCollision(int px, int py, int pType, int pRot) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            if (TETROMINOES[pType][pRot][j][i]) {
                for (int sx = 0; sx < BLOCK_SCALE; sx++) {
                    for (int sy = 0; sy < BLOCK_SCALE; sy++) {
                        int worldX = px + i * BLOCK_SCALE + sx;
                        int worldY = py + j * BLOCK_SCALE + sy;
                        
                        if (worldX < 0 || worldX >= GAME_W || worldY >= GAME_H) return true;
                        if (worldY >= 0 && grid[worldX][worldY] != COLOR_EMPTY) return true;
                    }
                }
            }
        }
    }
    return false;
}

bool SandTetrisGame::movePiece(int dx, int dy) {
    if (isGameOver) return false;
    if (!checkCollision(currentPiece.x + dx, currentPiece.y + dy, currentPiece.type, currentPiece.rotation)) {
        currentPiece.x += dx;
        currentPiece.y += dy;
        return true;
    }
    return false;
}

void SandTetrisGame::rotatePiece() {
    if (isGameOver) return;
    int newRot = (currentPiece.rotation + 1) % 4;
    if (!checkCollision(currentPiece.x, currentPiece.y, currentPiece.type, newRot)) {
        currentPiece.rotation = newRot;
    }
}

void SandTetrisGame::lockPiece() {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            if (TETROMINOES[currentPiece.type][currentPiece.rotation][j][i]) {
                for (int sx = 0; sx < BLOCK_SCALE; sx++) {
                    for (int sy = 0; sy < BLOCK_SCALE; sy++) {
                        int worldX = currentPiece.x + i * BLOCK_SCALE + sx;
                        int worldY = currentPiece.y + j * BLOCK_SCALE + sy;
                        if (worldX >= 0 && worldX < GAME_W && worldY >= 0 && worldY < GAME_H) {
                            grid[worldX][worldY] = currentPiece.colorId; 
                        }
                    }
                }
            }
        }
    }
    checkLines(); 
    spawnPiece(); 
}

void SandTetrisGame::updateSandPhysics() {
    for (int y = GAME_H - 2; y >= 0; y--) {
        bool startLeft = random(2);
        for (int i = 0; i < GAME_W; i++) {
            int x = startLeft ? i : (GAME_W - 1 - i);
            
            if (grid[x][y] != COLOR_EMPTY) { 
                if (grid[x][y+1] == COLOR_EMPTY) {
                    grid[x][y+1] = grid[x][y];
                    grid[x][y] = COLOR_EMPTY;
                } 
                else {
                    bool canGoLeft = (x > 0 && grid[x-1][y+1] == COLOR_EMPTY);
                    bool canGoRight = (x < GAME_W - 1 && grid[x+1][y+1] == COLOR_EMPTY);
                    
                    if (canGoLeft && canGoRight) {
                        int dir = random(2) ? -1 : 1;
                        grid[x+dir][y+1] = grid[x][y];
                        grid[x][y] = COLOR_EMPTY;
                    } else if (canGoLeft) {
                        grid[x-1][y+1] = grid[x][y];
                        grid[x][y] = COLOR_EMPTY;
                    } else if (canGoRight) {
                        grid[x+1][y+1] = grid[x][y];
                        grid[x][y] = COLOR_EMPTY;
                    }
                }
            }
        }
    }
}

// ★★★ 终极规则：BFS 洪水连通图消除 + 震动触发 ★★★
void SandTetrisGame::checkLines() {
    // 动态申请内存进行连通图计算，防止撑爆 ESP32 栈内存
    bool* visited = new bool[GAME_W * GAME_H];
    memset(visited, 0, GAME_W * GAME_H);
    uint16_t* queue = new uint16_t[GAME_W * GAME_H];

    // 从屏幕最左侧 (x=0) 开始扫描
    for (int startY = 0; startY < GAME_H; startY++) {
        if (grid[0][startY] == COLOR_EMPTY) continue; // 如果没沙子，跳过
        if (visited[startY * GAME_W + 0]) continue;   // 如果已经检查过了，跳过

        uint8_t targetColor = grid[0][startY];
        int head = 0, tail = 0;

        // 起点入队
        queue[tail++] = startY * GAME_W + 0;
        visited[startY * GAME_W + 0] = true;

        bool touchesRight = false;

        // 8个方向：上下左右 + 4个对角斜向
        int dx[] = {-1, 1, 0, 0, -1, -1, 1, 1};
        int dy[] = {0, 0, -1, 1, -1, 1, -1, 1};

        // 广度优先搜索 (BFS)：寻找同色连通块
        while (head < tail) {
            uint16_t curr = queue[head++];
            int cx = curr % GAME_W;
            int cy = curr / GAME_W;

            // 只要有任何一粒沙子碰到了最右边缘，就标记为导通！
            if (cx == GAME_W - 1) {
                touchesRight = true;
            }

            for (int i = 0; i < 8; i++) {
                int nx = cx + dx[i];
                int ny = cy + dy[i];

                if (nx >= 0 && nx < GAME_W && ny >= 0 && ny < GAME_H) {
                    int nIdx = ny * GAME_W + nx;
                    // 如果颜色相同且未遍历过，加入阵营
                    if (!visited[nIdx] && grid[nx][ny] == targetColor) {
                        visited[nIdx] = true;
                        queue[tail++] = nIdx;
                    }
                }
            }
        }

        // 如果这个同色连通块贯穿了屏幕左右
        if (touchesRight) {
            // 把队列里记录的所有这块连通的沙子，全部消除！
            for (int i = 0; i < tail; i++) {
                uint16_t pos = queue[i];
                grid[pos % GAME_W][pos / GAME_W] = COLOR_EMPTY;
            }
            score += tail; // 连通的沙子越多，得分越高
            
            // ★★★ 核心新增：消除瞬间，触发手柄大震动！ ★★★
            if (ble.connected()) {
                // 如果一次性消除的沙子超过 100 粒（大范围崩塌），震动 500ms
                // 如果是一般的消除，震动 300ms
                int vibrationTime = (tail > 100) ? 500 : 300; 
                String cmd = "{\"cmd\":\"vibrate\", \"val\":" + String(vibrationTime) + "}\n";
                ble.notify(cmd.c_str());
            }
        }
    }

    // 释放内存
    delete[] visited;
    delete[] queue;
}

void SandTetrisGame::handleInput(String key) {
    if (!isRunning || isGameOver) {
        if (key == "B") begin(); 
        return;
    }
    
    bool moved = false; 
    
    if (key == "LEFT") {
        for(int i = 0; i < BLOCK_SCALE; i++) movePiece(-1, 0);
        moved = true;
    } 
    else if (key == "RIGHT") {
        for(int i = 0; i < BLOCK_SCALE; i++) movePiece(1, 0);
        moved = true;
    } 
    else if (key == "DOWN") {
        for(int i = 0; i < BLOCK_SCALE * 2; i++) movePiece(0, 1);
        moved = true;
    } 
    else if (key == "A") {
        rotatePiece(); 
        moved = true;
    }
    
    if (moved) draw();
}

void SandTetrisGame::draw() {
    lastDrawTime = millis(); 
    ledDisplay.clearScreen();

    for (int x = 0; x < GAME_W; x++) {
        for (int y = 0; y < GAME_H; y++) {
            uint8_t cid = grid[x][y];
            if (cid != COLOR_EMPTY) {
                ledDisplay.setPixel(x, y, COLORS[cid][0], COLORS[cid][1], COLORS[cid][2]);
            }
        }
    }

    if (!isGameOver) {
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                if (TETROMINOES[currentPiece.type][currentPiece.rotation][j][i]) {
                    for (int sx = 0; sx < BLOCK_SCALE; sx++) {
                        for (int sy = 0; sy < BLOCK_SCALE; sy++) {
                            int drawX = currentPiece.x + i * BLOCK_SCALE + sx;
                            int drawY = currentPiece.y + j * BLOCK_SCALE + sy;
                            if (drawY >= 0 && drawY < GAME_H && drawX >= 0 && drawX < GAME_W) {
                                ledDisplay.setPixel(drawX, drawY, COLORS[currentPiece.colorId][0], COLORS[currentPiece.colorId][1], COLORS[currentPiece.colorId][2]);
                            }
                        }
                    }
                }
            }
        }
    } else {
        for(int x = 0; x < GAME_W; x++) ledDisplay.setPixel(x, 0, 255, 0, 0);
    }

    ledDisplay.refreshDisplay();
}

void SandTetrisGame::loop() {
    if (!isRunning) return;

    unsigned long now = millis();
    bool needDraw = false; 

    if (!isGameOver && now - lastDropTime >= dropInterval) {
        lastDropTime = now;
        if (!movePiece(0, 1)) {
            lockPiece();
        }
        needDraw = true;
    }

    // ★ 维持 60 FPS (16ms) 的物理运算和绘制，保证最丝滑的沙子流动
    if (now - lastPhysicsTime >= 32) {
        lastPhysicsTime = now;
        updateSandPhysics();
        needDraw = true;
    }

    if (needDraw && (now - lastDrawTime >= 32)) {
        draw();
    }
}