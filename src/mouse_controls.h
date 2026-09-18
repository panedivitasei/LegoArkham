#pragma once

#include <windows.h>

#include <string>

namespace mousectl {

// Arkham-style mouse and keyboard controls for player 1: space jumps, left click attacks (held,
// it brings up the batarang reticle, which the mouse then steers; releasing throws) and right
// click grabs; E switches character. Applies on top of player 1's input as the engine reads it.
void Install(const std::string& ini);
void Apply(BYTE* axes[8], DWORD* buttons);

// True while the mouse steers the reticle, so the camera leaves it alone.
bool Aiming();

}  // namespace mousectl
