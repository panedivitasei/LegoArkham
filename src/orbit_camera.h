#pragma once

#include <string>

namespace orbit {

// Third-person camera around player 1, steered with the right stick or mouse look. With
// `wallCollision` it stays in front of level geometry, like the game's own camera. It hands
// back to the game's camera for menus, cutscenes, co-op, and the interaction states listed in
// the ini's [Camera] section. F1 toggles it off and on for player 1.
void Install(float distance, bool wallCollision, const std::string& ini);

// Swings the orbit to frame `point` from behind the character, easing there and ignoring the
// stick and mouse until `Unfocus`.
void Focus(const float* point);
void Unfocus();

// The yaw the orbit is showing (radians, the game camera's convention), if it is in control.
bool ViewYaw(float& yaw);

}  // namespace orbit
