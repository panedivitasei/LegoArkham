#include "vehicle.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "frame.h"
#include "hook.h"
#include "orbit_camera.h"

namespace vehicle {
namespace {

// LEGOBatman.exe addresses. The exe has no ASLR, so these are absolute.
constexpr uintptr_t kPlayer1Ptr = 0xAB3980;
// Apply custom handling in vehicle levels only, which levels.txt flags.
// A vehicle picked up in any other level keeps the game's handling.
constexpr uintptr_t kLevelPtr = 0x960894;       // the level: its name at +0, its levels.txt index at +292
constexpr int kLevelIndexOffset = 292;
constexpr uintptr_t kLevelTablePtr = 0xACA554;  // the levels.txt entries, 188 bytes each
constexpr uintptr_t kLevelCountPtr = 0xACA560;
constexpr int kLevelEntrySize = 188;
constexpr int kLevelFlagsOffset = 124;
constexpr DWORD kVehicleLevel = 0x1;
constexpr uintptr_t kMenuState = 0x60CEE0;  // -1 while no menu is open
constexpr uintptr_t kFrameTime = 0xA95FE0;
constexpr uintptr_t kGameCameraPtr = 0x95F624;
constexpr int kCameraYawOffset = 0x1FC;  // int16, 65536 per turn, atan2(forward.x, forward.z)

constexpr int kContextOffset = 2523;
constexpr uint8_t kDrivingContext = 60;
constexpr int kRiderVehicleOffset = 2472;  // on the rider: the vehicle object
constexpr int kYawOffset = 88;             // int16: current, +90 the target, +582 the heading
constexpr int kFacingOffset = 90;
constexpr int kHeadingOffset = 582;
constexpr int kVelocityOffset = 104;   // x, y, z
constexpr int kSteerOffset = 5184;     // x, y, z: the velocity the movement code asks for
// On a definition (object+84 -> +36).
constexpr int kDefAcceleration = 216;  // how fast the velocity follows the heading, per second
constexpr int kDefRunSpeed = 176;
constexpr int kDefTurnRate = 264;  // turns per second when slow
constexpr int kDefVehicleFlagsOffset = 328;
constexpr DWORD kIsVehicle = 0x80000000;  // the definition's `vehicle` property
constexpr int kDefFloatFlagsOffset = 332;
constexpr DWORD kIsFloating = 0x2000;    // `floating_vehicle` / `water_vehicle`: the boats and jetskis
constexpr DWORD kIsFlying = 0x4000;      // `flying_vehicle`: the game keeps altitude on the jump button
constexpr DWORD kIsHelicopter = 0x8000;  // `helicopter`: can hover, turn in place and back up
constexpr int kDefIdleSpeed = 164;       // the pace a human driver never drops below
// The speed streaks: up to four flat ribbons trailing from locator pairs (0xFF = none). Meant for
// the top-down view, from behind they cross the lens as a blurred sheet, so they're switched off.
// The booster flames are a separate particle per thrust locator and stay.
constexpr int kDefStreakLocators = 504;
constexpr int kStreakLocatorBytes = 8;
// The booster flame VEHICLE_G1..3 is a refraction particle (render type 7): it draws by bending
// what's behind it, so the whole plume reads as a heat haze over the vehicle. Retyped to a plain
// additive sprite it keeps the flame texture and stops distorting.
constexpr uintptr_t kParticleDefsPtr = 0xA28CB8;  // array of 1064-byte definitions, [0] unused
constexpr uintptr_t kParticleDefCount = 0xA28CB4;
constexpr int kParticleNameLength = 11;  // then the texture names
constexpr int kParticleTypeOffset = 46;  // 0 alpha, 2 additive, 7 refraction
constexpr uint8_t kRefractionType = 7;
// The drive animation (the wheels) runs at the forward speed, which the game clamps at zero; the
// animator plays a negative rate backwards, so the clamp's branch is made unconditional.
constexpr uintptr_t kWheelClampJump = 0x4241C6;  // jnz over "rate = 0" in Character_UpdateAnim
// A negative rate only turns into backwards play if the animation is flagged reversible, which
// the drive animations aren't; the flag test in the frame advance is skipped so any negative rate
// plays backwards once the loop wraps.
constexpr uintptr_t kReversibleFlagJump = 0x59AAD3;  // jz over "reverse = 1" in Anim_Advance
// The animator state sits at object+8; its backwards flags for the playing and the blended-in
// animation are only updated at a loop wrap, so the mod writes them from the pedal every frame.
constexpr int kAnimReverseOffset = 70;
constexpr int kAnimBlendReverseOffset = 69;
// The current area's `pickups_to_panel` flag: collected studs fly to the HUD panel, past the
// lens at full screen size from behind the vehicle. Cleared while driven: studs count on contact.
constexpr uintptr_t kAreaPtr = 0xACA828;
constexpr int kAreaFlagsOffset = 100;
constexpr DWORD kPickupsToPanel = 0x80000;
constexpr float kPi = 3.14159265f;

std::string iniPath;
float accel = 0.6f;    // of top speed, per second
float turn = 1.0f;     // multiplier on the cornering grip
float reverse = 0.5f;  // reverse top speed, of forward
float lookSteer = 0.35f;  // how much the view pulls the nose, as a fraction of full stick
int thrustType = 2;    // render type given to the booster flame particles
constexpr float kSteerResponse = 8.0f;              // per second, how fast the wheels follow the stick
constexpr float kWheelbase = 1.0f;                  // units; a minifigure car is about that long
constexpr float kMaxSteerAngle = 34.0f * kPi / 180.0f;
constexpr float kLateralGrip = 12.0f;               // units per second squared the tyres can hold
float wheels;  // the steering angle as a fraction of full lock, -1 .. 1
// Boats: the rudder turns the hull at the game's own rate for the definition, and the hull slides,
// so the velocity only follows the heading with a lag.
constexpr float kBoatTurnResponse = 4.0f;  // per second, how fast the rudder follows the stick
constexpr float kBoatGrip = 2.5f;          // per second, how fast the velocity swings to the heading
constexpr float kMinTurnSpeed = 0.25f;     // of top speed: below this the rudder does nothing
constexpr int kDefTurnRateFast = 268;      // turns per second at speed; +264 is the slow rate
// Aircraft turn like a boat, but a plane holds its line (little slide) and never stalls.
constexpr float kPlaneGrip = 5.0f;
constexpr float kHelicopterGrip = 2.0f;
float rudder;
float slideX, slideZ;  // the hull's velocity, kept separate from its heading
bool floating, flying, helicopter;

int tunedDefinition;  // the definition currently taken over, with its original acceleration
float originalAcceleration;
float stickSteer, stickThrottle;  // this frame's stick, relative to the view: right and forward
bool takingOver;
float speed;  // signed, along the nose

template <class T>
T& At(uintptr_t address) {
  return *reinterpret_cast<T*>(address);
}

template <class T>
T& Field(int object, int offset) {
  return *reinterpret_cast<T*>(object + offset);
}

int Definition(int object) {
  int instance = Field<int>(object, 84);
  return instance ? At<int>(instance + 36) : 0;
}

bool VehicleLevel() {
  int level = At<int>(kLevelPtr);
  int table = At<int>(kLevelTablePtr), count = At<int>(kLevelCountPtr);
  if (!level || !table) return false;
  int index = Field<int>(level, kLevelIndexOffset);
  if (index < 0 || index >= count) return false;
  return (Field<DWORD>(table + kLevelEntrySize * index, kLevelFlagsOffset) & kVehicleLevel) != 0;
}

// The two animation patches only matter while the mod drives, so they go in and out with the level.
bool animPatched;
void PatchAnimation(bool on) {
  if (on == animPatched) return;
  animPatched = on;
  if (on) {
    hook::PatchBytes(reinterpret_cast<void*>(kWheelClampJump), {0x75, 0x2E}, {0xEB, 0x2E});
    hook::PatchBytes(reinterpret_cast<void*>(kReversibleFlagJump), {0x74, 0x15}, {0x90, 0x90});
  } else {
    hook::PatchBytes(reinterpret_cast<void*>(kWheelClampJump), {0xEB, 0x2E}, {0x75, 0x2E});
    hook::PatchBytes(reinterpret_cast<void*>(kReversibleFlagJump), {0x90, 0x90}, {0x74, 0x15});
  }
}

// The vehicle player 1 is driving, or 0: a vehicle they climbed into, or, in the chase levels,
// their own character, which is the vehicle itself.
bool IsVehicle(int definition) {
  return definition && ((Field<DWORD>(definition, kDefVehicleFlagsOffset) & kIsVehicle) ||
                        (Field<DWORD>(definition, kDefFloatFlagsOffset) & (kIsFloating | kIsFlying)));
}

int Driven() {
  int player = At<int>(kPlayer1Ptr);
  if (!player || !VehicleLevel()) return 0;
  if (Field<uint8_t>(player, kContextOffset) == kDrivingContext) return Field<int>(player, kRiderVehicleOffset);
  return IsVehicle(Definition(player)) ? player : 0;
}

void Release() {
  if (tunedDefinition) Field<float>(tunedDefinition, kDefAcceleration) = originalAcceleration;
  tunedDefinition = 0;
  takingOver = false;
}

// The streaks are switched off on every vehicle character in the level, all the time, so a
// switch of character never creates them; the definitions get their locators back once no such
// character exists.
constexpr uintptr_t kCharactersPtr = 0xAB364C;  // the character array, 5704 bytes each
constexpr uintptr_t kCharacterCountPtr = 0xAB3648;
constexpr int kCharacterStride = 5704;
constexpr int kLiveFlagsOffset = 508;  // bit 0 = in use
constexpr int kMaxQuieted = 8;
struct Quieted {
  int definition;
  BYTE streaks[kStreakLocatorBytes];
} quieted[kMaxQuieted];
int quietedCount;

void QuietStreaks() {
  int characters = At<int>(kCharactersPtr), count = At<int>(kCharacterCountPtr);
  int present[kMaxQuieted];
  int presentCount = 0;
  for (int i = 0; characters && i < count; ++i) {
    int object = characters + kCharacterStride * i;
    if (!(Field<DWORD>(object, kLiveFlagsOffset) & 1)) continue;
    int definition = Definition(object);
    if (!IsVehicle(definition)) continue;
    bool listed = false;
    for (int k = 0; k < presentCount; ++k) listed |= present[k] == definition;
    if (!listed && presentCount < kMaxQuieted) present[presentCount++] = definition;
  }
  // Restore definitions that no longer have a vehicle character.
  for (int q = 0; q < quietedCount;) {
    bool still = false;
    for (int k = 0; k < presentCount; ++k) still |= present[k] == quieted[q].definition;
    if (still) {
      ++q;
      continue;
    }
    memcpy(&Field<BYTE>(quieted[q].definition, kDefStreakLocators), quieted[q].streaks, kStreakLocatorBytes);
    quieted[q] = quieted[--quietedCount];
  }
  // Quiet the ones that are new, keeping their originals; keep the rest quiet.
  for (int k = 0; k < presentCount; ++k) {
    int definition = present[k];
    bool kept = false;
    for (int q = 0; q < quietedCount; ++q) kept |= quieted[q].definition == definition;
    if (!kept && quietedCount < kMaxQuieted) {
      Quieted& q = quieted[quietedCount++];
      q.definition = definition;
      memcpy(q.streaks, &Field<BYTE>(definition, kDefStreakLocators), kStreakLocatorBytes);
    }
    memset(&Field<BYTE>(definition, kDefStreakLocators), 0xFF, kStreakLocatorBytes);
  }
}

void Reload() {
  int accelPercent = GetPrivateProfileIntA("Vehicle", "Accel", 60, iniPath.c_str());
  int turnPercent = GetPrivateProfileIntA("Vehicle", "Turn", 100, iniPath.c_str());
  int reversePercent = GetPrivateProfileIntA("Vehicle", "Reverse", 50, iniPath.c_str());
  accel = std::clamp(accelPercent, 10, 1000) / 100.0f;
  turn = std::clamp(turnPercent, 10, 1000) / 100.0f;
  reverse = std::clamp(reversePercent, 0, 100) / 100.0f;
  lookSteer = std::clamp(static_cast<int>(GetPrivateProfileIntA("Vehicle", "LookSteer", 35, iniPath.c_str())), 0, 100) / 100.0f;
  thrustType = std::clamp(static_cast<int>(GetPrivateProfileIntA("Vehicle", "ThrustType", 2, iniPath.c_str())), 0, 7);
}

// Every loaded VEHICLE_G* definition gets the chosen render type; definitions reload with levels.
void RetypeThrusters() {
  auto defs = At<uint8_t**>(kParticleDefsPtr);
  int count = At<int>(kParticleDefCount);
  for (int i = 1; defs && i < count; ++i) {
    uint8_t* def = defs[i];
    if (!def || memcmp(def, "VEHICLE_G", 9) != 0 || def[kParticleNameLength - 1] != 0) continue;
    def[kParticleTypeOffset] = static_cast<uint8_t>(thrustType);
  }
}

// Drives the vehicle: throttle along the nose, steering that scales with speed and flips in
// reverse, the heading and velocity written outright.
void Drive(int vehicle, int definition, float dt, float view) {
  float top = Field<float>(definition, kDefRunSpeed);
  float back = flying && !helicopter ? 0.0f : reverse;  // a plane has no reverse
  float target = stickThrottle >= 0.0f ? stickThrottle * top : stickThrottle * top * back;
  // The game's idle pace floor (only planes have one); reverse stays reverse.
  if (stickThrottle >= 0.0f || back == 0.0f) target = std::max(target, Field<float>(definition, kDefIdleSpeed));
  float step = accel * top * dt;
  if (std::abs(stickThrottle) < 0.05f) step *= 0.6f;  // coasting slows gently
  speed += std::clamp(target - speed, -step, step);
  if (std::abs(speed) > 0.01f) {
    uint8_t backwards = speed < 0.0f ? 1 : 0;
    Field<uint8_t>(vehicle, kAnimReverseOffset) = backwards;
    Field<uint8_t>(vehicle, kAnimBlendReverseOffset) = backwards;
  }

  float yaw = Field<int16_t>(vehicle, kYawOffset) * (2.0f * kPi / 65536.0f);
  // Looking off the nose pulls it that way a little, on top of the stick, never instead of it.
  float look = std::remainder(view - yaw, 2.0f * kPi);
  float bias = std::abs(look) > 0.05f ? std::sin(look) * lookSteer : 0.0f;
  float steerInput = std::clamp(stickSteer + bias * (1.0f - std::abs(stickSteer)), -1.0f, 1.0f);
  float yawRate;
  if (floating || flying) {
    // A hull or airframe: the rudder needs way on to bite (a helicopter doesn't), turns at the
    // definition's rate (the game halves it on water), and steers the other way in reverse.
    rudder += (steerInput - rudder) * std::min(kBoatTurnResponse * dt, 1.0f);
    float pace = std::clamp(std::abs(speed) / std::max(top, 0.01f), 0.0f, 1.0f);
    float slow = Field<float>(definition, kDefTurnRate), fast = Field<float>(definition, kDefTurnRateFast);
    float rate = (slow + (fast - slow) * pace) * (floating ? 0.5f : 1.0f) * 2.0f * kPi * turn;
    float bite = helicopter ? 1.0f : std::clamp(pace / kMinTurnSpeed, 0.0f, 1.0f);
    yawRate = rudder * rate * bite * (speed < 0.0f ? -1.0f : 1.0f);
  } else {
    // Steering like a car: the wheels take a moment to reach their angle, the yaw rate then comes
    // from speed over the wheelbase, and grip caps how hard it can corner at speed.
    wheels += (steerInput - wheels) * std::min(kSteerResponse * dt, 1.0f);
    yawRate = speed / kWheelbase * std::tan(wheels * kMaxSteerAngle);
    float lateral = kLateralGrip * turn;
    if (std::abs(speed) > 0.01f) yawRate = std::clamp(yawRate, -lateral / std::abs(speed), lateral / std::abs(speed));
  }
  yaw += yawRate * dt;
  auto turns = static_cast<int16_t>(std::lround(yaw * 32768.0f / kPi));
  Field<int16_t>(vehicle, kYawOffset) = turns;
  Field<int16_t>(vehicle, kFacingOffset) = turns;
  Field<int16_t>(vehicle, kHeadingOffset) = turns;

  auto velocity = &Field<float>(vehicle, kVelocityOffset);
  auto steer = &Field<float>(vehicle, kSteerOffset);
  float wantX = std::sin(yaw) * speed, wantZ = std::cos(yaw) * speed;
  if (floating || flying) {
    // The hull keeps sliding the way it was going while the bow comes round.
    float grip = std::min((floating ? kBoatGrip : helicopter ? kHelicopterGrip : kPlaneGrip) * dt, 1.0f);
    slideX += (wantX - slideX) * grip;
    slideZ += (wantZ - slideZ) * grip;
    wantX = slideX;
    wantZ = slideZ;
  }
  velocity[0] = steer[0] = wantX;
  velocity[2] = steer[2] = wantZ;
}

void Update() {
  static DWORD lastReload;
  if (GetTickCount() - lastReload > 1000) {
    lastReload = GetTickCount();
    Reload();
  }
  QuietStreaks();
  RetypeThrusters();
  PatchAnimation(VehicleLevel());
  int vehicle = Driven();
  int definition = vehicle ? Definition(vehicle) : 0;
  float view;
  bool takeOver = definition && orbit::ViewYaw(view);
  if (!takeOver || definition != tunedDefinition) {
    Release();
    if (takeOver) {
      tunedDefinition = definition;
      originalAcceleration = Field<float>(definition, kDefAcceleration);
      auto velocity = &Field<float>(vehicle, kVelocityOffset);
      float yaw = Field<int16_t>(vehicle, kYawOffset) * (2.0f * kPi / 65536.0f);
      speed = velocity[0] * std::sin(yaw) + velocity[2] * std::cos(yaw);  // carry the current pace
      wheels = rudder = 0.0f;
      slideX = velocity[0];
      slideZ = velocity[2];
      DWORD flags = Field<DWORD>(definition, kDefFloatFlagsOffset);
      floating = (flags & kIsFloating) != 0;
      flying = (flags & kIsFlying) != 0;
      helicopter = (flags & kIsHelicopter) != 0;
    }
  }
  takingOver = takeOver;
  // Studs count on contact while driving; the area's flag comes back when the drive ends.
  static int flaggedArea;
  int area = At<int>(kAreaPtr);
  if (takingOver && area && (Field<DWORD>(area, kAreaFlagsOffset) & kPickupsToPanel)) {
    Field<DWORD>(area, kAreaFlagsOffset) &= ~kPickupsToPanel;
    flaggedArea = area;
  } else if (!takingOver && flaggedArea) {
    if (flaggedArea == area) Field<DWORD>(area, kAreaFlagsOffset) |= kPickupsToPanel;
    flaggedArea = 0;
  }
  if (!takingOver) return;
  Field<float>(definition, kDefAcceleration) = 0.0f;  // the game's easing leaves our velocity alone
  Drive(vehicle, definition, At<float>(kFrameTime), view);
}

}  // namespace

bool Driving() { return Driven() != 0; }

// Boats and aircraft are bigger and faster than the cars, so the camera sits a fifth further out.
float ZoomScale() { return takingOver && (floating || flying) ? 1.2f : 1.0f; }

void SteerToView(BYTE* axes[8]) {
  stickSteer = stickThrottle = 0.0f;
  float view;
  // The level update that drives the car doesn't run under a menu, so the last frame's takeover
  // would keep centring the stick the menu is trying to read.
  if (reinterpret_cast<int(__cdecl*)()>(kMenuState)() != -1) return;
  if (!Driven() || !orbit::ViewYaw(view)) return;
  // Vehicle controls: up on the stick is the nose, down reverses, sideways steers the vehicle's
  // own way, whatever the camera is doing.
  stickSteer = std::clamp((*axes[0] - 128) / 127.0f, -1.0f, 1.0f);
  stickThrottle = std::clamp((128 - *axes[1]) / 127.0f, -1.0f, 1.0f);
  if (std::abs(stickSteer) < 0.05f) stickSteer = 0.0f;
  if (std::abs(stickThrottle) < 0.05f) stickThrottle = 0.0f;
  // The game's own steering sees a centred stick while the mod drives.
  if (takingOver) *axes[0] = *axes[1] = 0x80;
}

void Install(const std::string& ini) {
  iniPath = ini;
  Reload();
  frame::OnLevelUpdate(Update);
}

}  // namespace vehicle
