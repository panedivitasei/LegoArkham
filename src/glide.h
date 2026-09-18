#pragma once

#include <string>

namespace glide {

// Lets the listed player characters glide by holding jump, using the game's own Glide Suit
// mechanic, with `boost` extra forward speed (units per second) while gliding. The list comes
// from [Gameplay] GlideCharacters in `ini`.
void Install(float boost, const std::string& ini);

}  // namespace glide
