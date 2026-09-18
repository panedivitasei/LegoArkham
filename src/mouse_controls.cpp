#include "mouse_controls.h"

#include <algorithm>
#include <cstdint>

#include "grapple.h"
#include "hook.h"
#include "mouse.h"

namespace mousectl {
namespace {

// LEGOBatman.exe addresses. The exe has no ASLR, so these are absolute.
constexpr uintptr_t kMenuState = 0x60CEE0;  // -1 while no menu is open
constexpr uintptr_t kPlayer1Ptr = 0xAB3980;

// Engine button bits, the same on every device. The keys themselves come from the game's own
// bindings (see keybinds.cpp). A tap of attack punches; holding it brings up the batarang reticle.
constexpr DWORD kAttack = 0x80;
constexpr DWORD kGrab = 0x20;  // the special button: grab, use, suit abilities
constexpr int kHoldFrames = 12;  // a click shorter than this is a punch, not an aim

// The reticle. Character_UpdateBatarangAim runs while the character is in the aim action (77):
// it drifts the reticle by a smoothed stick velocity and clamps it to its ±0.85 range, but
// until the stick has moved once it snaps the reticle back to the character every frame.
constexpr uintptr_t kAimUpdateThunk = 0x401B45;  // jmp Character_UpdateBatarangAim(object), its only entry
constexpr uintptr_t kAimUpdate = 0x40FC20;
constexpr uintptr_t kClampReticle = 0x5A51E0;  // (position, velocity)
constexpr int kContextOffset = 2523;
constexpr int kAimContext = 77;
constexpr int kAimFlagsOffset = 2532;
constexpr DWORD kReticleMoved = 0x200;
constexpr int kBatarangOffset = 4424;  // on the character: its batarang state
constexpr int kReticlePosition = 224;  // on the batarang state: x, y in ±0.85
constexpr int kReticleVelocity = 332;
constexpr float kReticlePerCount = 0.0025f;  // at sensitivity 1, the whole range is about 700 counts

using MenuFn = int(__cdecl*)();
using ObjectFn = void(__cdecl*)(int object);
using ClampFn = void(__cdecl*)(float* position, float* velocity);

bool enabled;
float aimSensitivity = 1.0f;
mouse::Delta aim;
bool aiming;
int heldFrames;

template <class T>
T& Field(int object, int offset) {
  return *reinterpret_cast<T*>(object + offset);
}

void __cdecl HookAimUpdate(int object) {
  float dx, dy;
  aim.Take(dx, dy);
  bool mine = aiming && object == *reinterpret_cast<int*>(kPlayer1Ptr) && Field<uint8_t>(object, kContextOffset) == kAimContext;
  if (mine && (dx != 0.0f || dy != 0.0f)) Field<DWORD>(object, kAimFlagsOffset) |= kReticleMoved;
  reinterpret_cast<ObjectFn>(kAimUpdate)(object);
  if (!mine) return;
  int batarang = Field<int>(object, kBatarangOffset);
  if (!batarang || (dx == 0.0f && dy == 0.0f)) return;  // the stick's smoothed velocity carries on
  // The mouse places the reticle; the stick's velocity is dropped so it doesn't drift afterwards.
  auto position = &Field<float>(batarang, kReticlePosition);
  auto velocity = &Field<float>(batarang, kReticleVelocity);
  position[0] += dx * kReticlePerCount * aimSensitivity;
  position[1] -= dy * kReticlePerCount * aimSensitivity;  // the reticle's y grows upward
  velocity[0] = velocity[1] = 0.0f;
  reinterpret_cast<ClampFn>(kClampReticle)(position, velocity);
}

}  // namespace

bool Aiming() { return aiming; }

void Apply(BYTE*[8], DWORD* buttons) {
  aiming = false;
  if (!enabled || !mouse::InFront() || reinterpret_cast<MenuFn>(kMenuState)() != -1) return;

  // A grapple launch presses the use button once, on top of the game's own bindings.
  if (grapple::TakeFire(0)) *buttons |= kGrab;
  heldFrames = (*buttons & kAttack) ? heldFrames + 1 : 0;
  aiming = heldFrames >= kHoldFrames;
}

void Install(const std::string& ini) {
  enabled = true;
  int sensitivity = GetPrivateProfileIntA("Controls", "AimSensitivity", 100, ini.c_str());
  aimSensitivity = std::clamp(sensitivity, 10, 1000) / 100.0f;
  mouse::Watch(&aim);
  hook::PatchCall(reinterpret_cast<void*>(kAimUpdateThunk), reinterpret_cast<void*>(kAimUpdate), HookAimUpdate);
}

}  // namespace mousectl
