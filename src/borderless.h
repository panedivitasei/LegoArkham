#pragma once

#include <windows.h>

namespace borderless {

// Runs the game in a borderless window covering its monitor, instead of exclusive fullscreen.
void Install(HMODULE game);

}  // namespace borderless
