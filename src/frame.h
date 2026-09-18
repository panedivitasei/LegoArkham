#pragma once

namespace frame {

// Runs `fn` once per frame during play, just before the level's update.
void OnLevelUpdate(void (*fn)());

}  // namespace frame
