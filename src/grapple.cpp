#include "grapple.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "frame.h"
#include "hook.h"
#include "mouse.h"

namespace grapple {
namespace {

// LEGOBatman.exe addresses. The exe has no ASLR, so these are absolute.
constexpr uintptr_t kLevelPtr = 0x960894;
constexpr uintptr_t kGrappleTypeId = 0x95FC30;   // the "Grapple" object type, assigned at load
constexpr uintptr_t kGrappleAbility = 0xAB083C;  // the ability bit a character needs to grapple
constexpr uintptr_t kPlayerObjects = 0xAB3960;  // character pointers, player 1 first
constexpr uintptr_t kFindUsableThing = 0x5A9AB0;  // (level, position, character, 0, 0): what the use button would take
constexpr uintptr_t kFindUsableCallSite = 0x5AD6E2;  // its one call, in the use state's entry
constexpr uintptr_t kRayCast = 0x572D90;          // (start, ray, 0, 1, 0, flags): nonzero on a hit, ray shortened
constexpr uintptr_t kMenuState = 0x60CEE0;        // -1 while no menu is open
constexpr uintptr_t kFrameTime = 0xA95FE0;
constexpr uintptr_t kGamePads = 0xA96388;  // 96 bytes per port: +0 NUPAD, whose +112 = held, +120 = pressed

// The level keeps a list per object type (behind a pointer to a pointer): 20 bytes each, +4 count,
// +12 entries of 8 bytes whose first dword is the object.
constexpr int kThingListsOffset = 11020;
constexpr int kThingPosition = 16;  // x, y, z of the orange marker, at the top edge of what it climbs
constexpr int kThingYaw = 32;       // uint16; the character faces this + half a turn when using it

constexpr int kPositionOffset = 92;
constexpr int kVelocityOffset = 104;
constexpr int kYawOffset = 88;  // int16, 65536 per turn
constexpr int kFacingOffset = 90;  // int16 the character turns toward
constexpr int kHeadingOffset = 582;  // int16 the use state compares against
constexpr int kAbilitiesOffset = 1164;
constexpr int kAnimOffset = 2512;
constexpr int kUsePhaseOffset = 2521;
constexpr int kUseThingOffset = 2492;
constexpr int kClimbOffset = 2440;
constexpr int kContextOffset = 2523;
constexpr int kActiveFlagsOffset = 508;  // bit 7 = a human player's character
constexpr DWORD kUseButton = 0x20;
constexpr int kRayFlags = 31;  // what the batarang's line-of-sight test uses
constexpr float kPi = 3.14159265f;
constexpr float kEyeHeight = 0.5f;
constexpr int kLaunchFrames = 30;
constexpr int kWatchFrames = 400;

using FindThingFn = int(__cdecl*)(int level, float* position, int character, int, int);
using RayCastFn = int(__cdecl*)(float* start, float* ray, float, float, int, int flags);
using MenuFn = int(__cdecl*)();

float range = 4.0f;  // units; a minifigure stands about 0.6 tall

// The grapple type's draw paints a decal on the floor under every point (Grapple_Draw, the call
// after the rope). Skipped, so only grapple points lose their floor marker.
constexpr uintptr_t kDecalCallSite = 0x5A9F62;
constexpr uintptr_t kDrawDecal = 0x5B2480;
int __cdecl NoDecal(int, int, int, int, float, float, int, int) { return 0; }

enum class State { Idle, Launching, Watching };

// Everything a grapple in progress knows, one per player.
struct Grapple {
  State state = State::Idle;
  bool wasDown = false;
  int target = 0;  // the focused grapple object, 0 if none
  float landing[3] = {};
  int launched = 0;  // the object the game's use state is being handed
  int frames = 0;
  bool firePending = false;  // one fresh press of the use button, taken by the input hook
  bool buttonHeld = false;   // the grapple button, as the input hook last saw it
  bool ready = false;        // a grapple point is in reach, as of the last update
  // The line shows after this long in the climb: at once from the air, where the arm is already
  // out, and after the pose blend from the ground, so the gun is pointing first.
  float lineDelay = 0, lineTime = 0;
  // The zip along the line, once the game's climb has begun.
  bool climbing = false, finished = false;
  float climbStart = 0, launchRef[3] = {}, refOffset[3] = {}, gameStart[3] = {}, path[3] = {}, pathLength = 0,
        zipTime = 0, travel = 0;
};
Grapple players[2];

int PlayerObject(int player) { return *reinterpret_cast<int*>(kPlayerObjects + 4 * player); }

// The player whose character this is, or -1.
int PlayerOf(int character) {
  for (int player = 0; player < 2; ++player)
    if (character && PlayerObject(player) == character) return player;
  return -1;
}

template <class T>
T& At(uintptr_t address) {
  return *reinterpret_cast<T*>(address);
}

template <class T>
T& Field(int object, int offset) {
  return *reinterpret_cast<T*>(object + offset);
}

// Standing (255), moving, and the jump, so a grapple can start from midair too.
bool Roaming(int character) {
  uint8_t context = Field<uint8_t>(character, kContextOffset);
  return context == 255 || context == 0 || context == 1 || context == 79;
}

// The level's list of objects of one type, or 0.
int ThingList(int type) {
  int level = At<int>(kLevelPtr);
  if (!level || type < 0) return 0;
  int lists = At<int>(level + kThingListsOffset);
  return lists ? At<int>(lists) + 20 * type : 0;
}

bool CanGrapple(int character) {
  return (Field<DWORD>(character, kAbilitiesOffset) & At<DWORD>(kGrappleAbility)) != 0;
}

// Levers and other use points share the list; the level names its grapple points "grapple...".
bool IsGrapplePoint(int thing) {
  char name[17] = {};
  memcpy(name, reinterpret_cast<const char*>(thing), 16);
  for (char& c : name) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return strstr(name, "grapple") != nullptr;
}

bool Visible(const float* from, const float* to) {
  float ray[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
  return reinterpret_cast<RayCastFn>(kRayCast)(const_cast<float*>(from), ray, 0.0f, 1.0f, 0, kRayFlags) == 0;
}

// The nearest grapple point in range with a clear line from the character's eyes to its marker.
int NearestPoint(int character, float* markerOut) {
  int list = ThingList(At<int>(kGrappleTypeId));
  if (!list) return 0;
  int count = Field<int>(list, 4), entries = Field<int>(list, 12);
  auto pos = &Field<float>(character, kPositionOffset);
  float eyes[3] = {pos[0], pos[1] + kEyeHeight, pos[2]};
  int best = 0;
  float bestDistance = range;
  for (int i = 0; i < count; ++i) {
    int thing = At<int>(entries + 8 * i);
    if (!thing || !IsGrapplePoint(thing)) continue;
    auto marker = &Field<float>(thing, kThingPosition);
    float m[3] = {marker[0] - eyes[0], marker[1] - eyes[1], marker[2] - eyes[2]};
    float distance = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    if (distance < 1.0f || distance >= bestDistance) continue;
    // Sight is tested to the marker, pulled a little toward the character so the wall it sits
    // on doesn't count as blocking.
    float probe[3] = {marker[0] - m[0] / distance * 0.35f, marker[1] - m[1] / distance * 0.35f,
                      marker[2] - m[2] / distance * 0.35f};
    if (!Visible(eyes, probe)) continue;
    best = thing;
    bestDistance = distance;
    memcpy(markerOut, marker, 3 * sizeof(float));
  }
  return best;
}

void Face(int character, const float* point) {
  auto pos = &Field<float>(character, kPositionOffset);
  float yaw = std::atan2(point[0] - pos[0], point[2] - pos[2]);
  auto turns = static_cast<int16_t>(std::lround(yaw * 32768.0f / kPi));
  Field<int16_t>(character, kFacingOffset) = turns;
  Field<int16_t>(character, kYawOffset) = turns;
  Field<int16_t>(character, kHeadingOffset) = turns;
}

// The use state's entry asks what usable object is near the character; while launching it is
// told the focused grapple point instead, whatever the distance.
int __cdecl HookFindUsable(int level, float* position, int character, int a4, int a5) {
  int player = PlayerOf(character);
  if (player >= 0 && players[player].state == State::Launching) return players[player].launched;
  return reinterpret_cast<FindThingFn>(kFindUsableThing)(level, position, character, a4, a5);
}

// The use state's movement: a walk-in animation first pulls the character to the stand spot,
// then the climb steers toward "the top point, less the height still to climb" at 5 per second.
constexpr uintptr_t kUseMoveCallSite = 0x4D3C5B;
constexpr uintptr_t kUseMove = 0x5ADC70;
constexpr int16_t kWalkInAnim = 352;
constexpr int16_t kHangAnim = 187;
constexpr int16_t kClimbAnim = 188;
constexpr int16_t kDescendAnim = 189;  // picked while the stick is held down: the game backs down its rope
constexpr int kApproachTimerOffset = 2444;
constexpr int kReferenceOffset = 400;  // the point the climb steers, above the position
constexpr int kSteerOffset = 5184;     // the velocity the use state asks for
constexpr int kStateFlagsOffset = 2532;
constexpr int kGroundedOffset = 589;  // nonzero while the character stands on something
constexpr float kGroundLineDelay = 0.2f;  // the hang pose's blend-in, when the gun comes up
constexpr DWORD kLinePending = 0x400000;  // set on use-state entry; Grapple_DrawLine waits for it to clear
constexpr float kSteerRate = 5.0f;
using UseMoveFn = int(__cdecl*)(int character);

float zipSpeed = 5.0f;  // top speed along the line, units per second
constexpr float kZipStartSpeed = 1.0f;
constexpr float kZipAcceleration = 6.0f;   // units per second squared
constexpr float kZipBrakeDistance = 1.0f;  // slows over the last stretch below the ledge
constexpr float kZipEndSpeed = 2.0f;

// Skips the walk-in, and bends the climb so it starts from the launch spot: the game's path is
// shifted by the launch offset, scaled by the fraction of the height still to climb. The zip owns
// the character on every clip of the climb, the descend included, or the game's own rope
// movement and the zip would take turns placing them.
int __cdecl HookUseMove(int character) {
  int player = PlayerOf(character);
  Grapple& g = players[player >= 0 ? player : 0];
  bool mine = player >= 0 && g.launched && Field<int>(character, kUseThingOffset) == g.launched;
  auto& climbing = g.climbing;
  auto& finished = g.finished;
  auto& climbStart = g.climbStart;
  auto& launchRef = g.launchRef;
  auto& refOffset = g.refOffset;
  auto& gameStart = g.gameStart;
  auto& path = g.path;
  auto& pathLength = g.pathLength;
  auto& zipTime = g.zipTime;
  auto& travel = g.travel;
  if (mine && Field<int16_t>(character, kAnimOffset) == kWalkInAnim) {
    Field<int16_t>(character, kAnimOffset) = kHangAnim;
    Field<float>(character, kApproachTimerOffset) = 0.3f;
  }
  int result = reinterpret_cast<UseMoveFn>(kUseMove)(character);
  int16_t anim = Field<int16_t>(character, kAnimOffset);
  if (!mine || Field<uint8_t>(character, kUsePhaseOffset) != 0 ||
      (anim != kHangAnim && anim != kClimbAnim && anim != kDescendAnim))
    return result;
  // The game holds the line back until the animator plays the requested clip unblended, which
  // the skipped walk-in would have provided; it's released on the launch's own timer instead.
  g.lineTime += At<float>(kFrameTime);
  if (g.lineTime >= g.lineDelay) Field<DWORD>(character, kStateFlagsOffset) &= ~kLinePending;
  if (finished) return result;  // at the top; the climb over the edge is the game's own
  float remaining = Field<float>(character, kClimbOffset);
  auto pos = &Field<float>(character, kPositionOffset);
  auto ref = &Field<float>(character, kReferenceOffset);
  auto steer = &Field<float>(character, kSteerOffset);
  auto velocity = &Field<float>(character, kVelocityOffset);
  if (!climbing) {
    climbing = true;
    zipTime = travel = 0.0f;
    climbStart = std::max(remaining, 0.01f);
    for (int i = 0; i < 3; ++i) {
      launchRef[i] = ref[i];
      refOffset[i] = ref[i] - pos[i];
      gameStart[i] = ref[i] + steer[i] / kSteerRate;  // where the game's climb would begin
      path[i] = gameStart[i] - launchRef[i];
    }
    path[1] += climbStart;  // the climb ends the height still to climb above its start
    pathLength = std::max(std::sqrt(path[0] * path[0] + path[1] * path[1] + path[2] * path[2]), 0.1f);
  }
  // The zip pulls by itself: a slow start, accelerating along the line, braking under the ledge.
  // The character is placed on the line outright, and the game's climb value follows, so the
  // game sees the top reached and finishes with its own climb over the edge.
  float dt = At<float>(kFrameTime);
  zipTime += dt;
  float speed = std::min(kZipStartSpeed + kZipAcceleration * zipTime, zipSpeed);
  float left = (1.0f - travel) * pathLength;
  if (left < kZipBrakeDistance) speed = std::min(speed, kZipEndSpeed + (speed - kZipEndSpeed) * left / kZipBrakeDistance);
  travel = std::min(travel + dt * speed / pathLength, 1.0f);
  for (int i = 0; i < 3; ++i) {
    ref[i] = launchRef[i] + path[i] * travel;
    pos[i] = ref[i] - refOffset[i];
    velocity[i] = path[i] / pathLength * speed;
    steer[i] = 0.0f;
  }
  Field<float>(character, kClimbOffset) = climbStart * (1.0f - travel);
  float top[3] = {launchRef[0] + path[0], launchRef[1] + path[1], launchRef[2] + path[2]};
  Face(character, top);  // the use state keeps turning the character to the wall; face the line instead
  if (travel >= 1.0f) finished = true;
  return result;
}

// Hands the focused point to the game's use state, with one fresh press of the use button.
void Launch(Grapple& g, int character) {
  g.launched = g.target;
  g.firePending = true;
  g.climbing = g.finished = false;
  g.lineTime = 0.0f;
  g.lineDelay = Field<uint8_t>(character, kGroundedOffset) ? kGroundLineDelay : 0.0f;
  g.frames = 0;
  g.state = State::Launching;
  Face(character, g.landing);  // toward the marker, the way the line will run
}

void UpdatePlayer(Grapple& g, int character, bool inFront) {
  bool inPlay = character && inFront && reinterpret_cast<MenuFn>(kMenuState)() == -1 &&
                (Field<uint8_t>(character, kActiveFlagsOffset) & 0x80);
  bool down = inPlay && g.buttonHeld;
  bool pressed = down && !g.wasDown;
  g.wasDown = down;

  if (g.state == State::Launching) {
    ++g.frames;
    bool started = !Roaming(character);
    if (started || g.frames > kLaunchFrames || !inPlay) {
      if (!started) g.launched = 0;
      g.state = started ? State::Watching : State::Idle;
      g.frames = 0;
    }
    return;
  }
  if (g.state == State::Watching) {
    // The game's grapple is running; the movement hook bends it until the character is free.
    ++g.frames;
    if (Roaming(character) || g.frames > kWatchFrames || !inPlay) {
      g.launched = 0;
      g.state = State::Idle;
    }
    return;
  }

  // The grapple button takes the nearest point in sight, unless the use button has something to
  // take here: no building, no panels. Whether there is one is kept for the input hook, so the
  // press only leaves the game's hands when it's going to grapple.
  g.ready = false;
  if (!inPlay) return;
  auto pos = &Field<float>(character, kPositionOffset);
  int usable = reinterpret_cast<FindThingFn>(kFindUsableThing)(At<int>(kLevelPtr), pos, character, 0, 0);
  if (!Roaming(character) || !CanGrapple(character) || usable) return;
  int candidate = NearestPoint(character, g.landing);
  g.ready = candidate != 0;
  if (!pressed || !g.ready) return;
  g.target = candidate;
  Launch(g, character);
}

void Update() {
  bool inFront = mouse::InFront();
  for (int player = 0; player < 2; ++player) UpdatePlayer(players[player], PlayerObject(player), inFront);
}

}  // namespace

bool Armed(int player) { return players[player & 1].state != State::Idle; }

void SetButton(int player, bool held) { players[player & 1].buttonHeld = held; }

bool Ready(int player) {
  const Grapple& g = players[player & 1];
  return g.ready || g.state != State::Idle;
}

bool TakeFire(int player) {
  Grapple& g = players[player & 1];
  bool fire = g.firePending;
  g.firePending = false;
  return fire;
}

void Install(const std::string& ini) {
  int rangeUnits = GetPrivateProfileIntA("Controls", "GrappleRange", 4, ini.c_str());
  int speedUnits = GetPrivateProfileIntA("Controls", "GrappleSpeed", 5, ini.c_str());
  range = static_cast<float>(std::clamp(rangeUnits, 1, 100));
  zipSpeed = static_cast<float>(std::clamp(speedUnits, 2, 30));
  frame::OnLevelUpdate(Update);
  if (!GetPrivateProfileIntA("Controls", "GrapplePointDecal", 0, ini.c_str()))
    hook::PatchCall(reinterpret_cast<void*>(kDecalCallSite), reinterpret_cast<void*>(kDrawDecal),
                    reinterpret_cast<void*>(NoDecal));
  hook::PatchCall(reinterpret_cast<void*>(kFindUsableCallSite), reinterpret_cast<void*>(kFindUsableThing),
                  HookFindUsable);
  hook::PatchCall(reinterpret_cast<void*>(kUseMoveCallSite), reinterpret_cast<void*>(kUseMove), HookUseMove);
}

}  // namespace grapple
