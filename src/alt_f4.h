#pragma once

#include <windows.h>

namespace altf4 {

// Lets Alt+F4 close the game through its own WM_CLOSE shutdown.
void Install(HMODULE game);

}  // namespace altf4
