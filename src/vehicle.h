#pragma once

#include <windows.h>

#include <string>

namespace vehicle {

// Driving for player 1: the stick steers relative to the view, and the vehicle's grip and turn
// rates can be scaled while driven.
void Install(const std::string& ini);

// True while player 1 drives in one of the game's vehicle levels: a vehicle they climbed into,
// or the level's own car, boat or aircraft. Vehicles in other levels are left to the game.
bool Driving();

// Extra distance for the driving camera: 1 for cars, more for boats and aircraft.
float ZoomScale();

// Turns the left stick (axes 0 and 1, resting at 0x80) so that "left" is screen-left while
// driving: the game reads the stick relative to its own camera, which stays on the road.
void SteerToView(BYTE* axes[8]);

}  // namespace vehicle
