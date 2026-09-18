#pragma once

#include <windows.h>

namespace vsync {

// Makes Vertical Sync default to on. A choice saved in pcconfig.txt still wins.
void DefaultOn(HMODULE game);

}  // namespace vsync
