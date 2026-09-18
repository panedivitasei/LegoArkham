#pragma once

#include <windows.h>

namespace mouse {

// Mouse movement since it was last taken, in the game's own DirectInput counts. Every registered
// delta receives all movement, so each user of the mouse drains its own at its own point in the
// frame.
struct Delta {
  float x = 0.0f, y = 0.0f;
  void Take(float& dx, float& dy) {
    dx = x, dy = y;
    x = y = 0.0f;
  }
};

void Watch(Delta* delta);

// True while a window of the game is in front. Also takes the frame's mouse movement.
bool InFront();

// Hides the cursor and keeps it inside the game window. Re-apply every frame while wanted.
void Capture(bool on);

bool ButtonDown(int virtualKey);

}  // namespace mouse
