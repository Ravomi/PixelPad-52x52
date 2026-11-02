#include "GameOfLife.h"

extern LED ledDisplay; // 引用全局屏幕对象

void GameOfLife::begin() {
    lastCursorPhase = -1;
    // 初始化清空
    memset(grid, 0, sizeof(grid));
    cursorX = 26;
    cursorY = 26;
    isRunning = false; // 默认暂停，方便你画图
}

// 处理手柄指令
void GameOfLife::handleInput(String key) {
    if (key == "B") {
        // B键：播放/暂停
        isRunning = !isRunning;
        Serial.println(isRunning ? "▶️ 开始演化" : "⏸️ 进入编辑模式");
        return;
    }

    // 如果正在运行，屏蔽其他按键（或者你可以做加速减速）
    if (isRunning) return;

    // --- 编辑模式下的操作 ---
    if (key == "UP")    cursorY--;
    if (key == "DOWN")  cursorY++;
    if (key == "LEFT")  cursorX--;
    if (key == "RIGHT") cursorX++;

    // 循环边界 (从左边出去，从右边回来)
    if (cursorX < 0) cursorX = 51;
    if (cursorX > 51) cursorX = 0;
    if (cursorY < 0) cursorY = 51;
    if (cursorY > 51) cursorY = 0;

    // A键：放置/擦除细胞
    if (key == "A") {
        grid[cursorX][cursorY] = !grid[cursorX][cursorY];
    }

    // 操作后立即重绘，保证手感顺滑
    draw();
}

void GameOfLife::computeNextGeneration() {
    for (int x = 0; x < 52; x++) {
        for (int y = 0; y < 52; y++) {
            int neighbors = countNeighbors(x, y);
            bool alive = grid[x][y];

            if (alive && (neighbors < 2 || neighbors > 3)) {
                nextGrid[x][y] = false; // 死
            } else if (!alive && neighbors == 3) {
                nextGrid[x][y] = true;  // 生
            } else {
                nextGrid[x][y] = alive; // 保持
            }
        }
    }
    // 复制回去
    memcpy(grid, nextGrid, sizeof(grid));
}

int GameOfLife::countNeighbors(int x, int y) {
    int sum = 0;
    for (int i = -1; i <= 1; i++) {
        for (int j = -1; j <= 1; j++) {
            if (i == 0 && j == 0) continue;
            int col = (x + i + 52) % 52;
            int row = (y + j + 52) % 52;
            if (grid[col][row]) sum++;
        }
    }
    return sum;
}

void GameOfLife::draw() {
    lastCursorPhase = (millis() / 200) % 2;
    ledDisplay.clearScreen();

    // 1. 先画所有的活细胞 (绿色)
    for (int x = 0; x < 52; x++) {
        for (int y = 0; y < 52; y++) {
            if (grid[x][y]) {
                ledDisplay.setPixel(x, y, 0, 255, 0); 
            }
        }
    }

    // 2. 如果是编辑模式，画光标 (红色闪烁)
    if (!isRunning) {
        // 让光标每500ms闪烁一次
        if ((millis() / 200) % 2 == 0) {
            ledDisplay.setPixel(cursorX, cursorY, 255, 0, 0);
        } else {
            // 闪烁间隙显示白色，防止找不到光标
             ledDisplay.setPixel(cursorX, cursorY, 100, 100, 100);
        }
    }
    
    // 注意：有些库需要 ledDisplay.show();
}

void GameOfLife::loop() {
    // 运行模式下，定时演化
    if (isRunning) {
        if (millis() - lastUpdate > speed) {
            lastUpdate = millis();
            computeNextGeneration();
            draw();
        }
    } else {
        // The image only changes when the cursor blink changes (or on input).
        if (lastCursorPhase != static_cast<int>((millis() / 200) % 2)) draw();
    }
}