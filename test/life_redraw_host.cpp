#include "GameOfLife.h"
#include <cassert>
unsigned long hostMillis = 0;
SerialStub Serial;
LED ledDisplay;

int main() {
    GameOfLife game;
    game.begin();
    game.loop();
    assert(ledDisplay.clears == 1);
    for (hostMillis = 1; hostMillis < 200; ++hostMillis) game.loop();
    assert(ledDisplay.clears == 1); // No full-screen redraw while nothing changed.
    game.loop();
    assert(ledDisplay.clears == 2); // Blink still updates at 200 ms.
    game.handleInput("LEFT");
    assert(ledDisplay.clears == 3 && ledDisplay.lastX == 25 && ledDisplay.lastY == 26);
    game.loop();
    assert(ledDisplay.clears == 3); // Input already drew its frame.
    game.handleInput("A");
    assert(ledDisplay.clears == 4);
    game.handleInput("B");
    game.loop();
    assert(ledDisplay.clears == 5); // Simulation remains active at its original period.
    hostMillis += 50;
    game.loop();
    assert(ledDisplay.clears == 5);
    hostMillis += 51;
    game.loop();
    assert(ledDisplay.clears == 6);
    game.begin();
    game.loop();
    assert(ledDisplay.clears == 7); // Re-entering the same blink phase still draws.
}
