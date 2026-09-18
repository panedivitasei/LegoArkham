#pragma once

// Keeps studs from filling the screen: the collected-stud effect that flies to the counter is
// capped at the counter's size, and loose parts sitting on the lens are left out of the draw.
namespace studs {

void Install();

}  // namespace studs
