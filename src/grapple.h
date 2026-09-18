#pragma once

#include <string>

namespace grapple {

// Quick grapple for both players: the grapple button (F, or the right bumper), with nothing to
// build or use in reach, fires the game's own grapple at the nearest grapple point in sight and
// zips the character along the line to it.
void Install(const std::string& ini);

// The input hook reports a player's grapple button each frame, and asks whether a press would
// grapple, so a button shared with a game action only goes to the grapple when there's a point
// to take.
void SetButton(int player, bool held);
bool Ready(int player);

// While a player's grapple runs, the launch presses their use button once, which is what starts
// the game's grapple.
bool Armed(int player);
bool TakeFire(int player);

}  // namespace grapple
