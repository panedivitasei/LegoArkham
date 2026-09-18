#pragma once

namespace player2 {

// Keyboard and pad both drive player 1 until the "Player 2 toggle" button (F2 by default) drops
// player 2 in on the spare device. That button is the only way for player 2 to join or leave;
// the game's own any-button pad join for player 2 is disabled.
void Install();

// True while a second human is playing.
bool IsHuman();

// One press of the "Camera toggle" button (F1 by default) this frame, from either device.
bool CameraTogglePressed();

}  // namespace player2
