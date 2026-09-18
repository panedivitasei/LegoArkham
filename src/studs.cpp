#include "studs.h"

#include <windows.h>

#include <cmath>
#include <cstdint>

#include "hook.h"

namespace studs {
namespace {

// LEGOBatman.exe addresses. The exe has no ASLR, so these are absolute.
constexpr uintptr_t kDrawCallSite = 0x4AFE0B;  // call Parts_Draw(pass) from the render pass
constexpr uintptr_t kDrawFn = 0x5886A0;
constexpr uintptr_t kPartsPoolPtr = 0xA361B0;
constexpr uintptr_t kPartsCount = 0x95E148;
constexpr uintptr_t kNuCameraPtr = 0xA96128;  // world matrix first, so the eye sits at float 12

constexpr int kPartStride = 592;
constexpr int kPartPosOffset = 48;    // translation row of the part's matrix
constexpr int kPartFlagsOffset = 328;  // bit0 live; the draw skips anything else
constexpr int kMaxHidden = 64;

constexpr float kHideRadius = 1.0f;  // from the eye; a stud this close covers most of the screen

// A collected stud is a screen effect: the model flies from where it was to the counter, starting
// at the size it had on screen and shrinking to the counter's. Picked up right in front of the
// lens, that start size is the whole screen. The HUD projects the effects once, then draws them.
constexpr uintptr_t kProjectCallSite = 0x4E0E15;  // call ScreenEffects_Project(eye, right, forward)
constexpr uintptr_t kProjectFn = 0x5D39A0;
constexpr uintptr_t kEffectPool = 0xABE2F8;
constexpr int kEffectCount = 128;
constexpr int kEffectStride = 280;
constexpr int kEffectLiveOffset = 252;
constexpr int kEffectStartScaleOffset = 184;  // from the projection, once, when flags bit2 is set
constexpr int kEffectFinishOffset = 276;      // callback when it arrives
constexpr uintptr_t kCounterAddThunk = 0x4020B3;  // the stud effect's: adds it to the counter

constexpr float kMaxStartScale = 0.5f;  // the counter's own size, so a stud never grows past it

using DrawFn = void(__cdecl*)(int pass);
using ProjectFn = void(__cdecl*)(float* eye, float* right, float* forward);

template <class T>
T& At(uintptr_t address) {
  return *reinterpret_cast<T*>(address);
}

void __cdecl HookDraw(int pass) {
  uint8_t* hidden[kMaxHidden];
  int count = 0;
  auto eye = At<float*>(kNuCameraPtr) + 12;
  auto part = At<uint8_t*>(kPartsPoolPtr);
  int total = At<int>(kPartsCount);
  for (int i = 0; part && eye && i < total && count < kMaxHidden; ++i, part += kPartStride) {
    auto& flags = At<uint8_t>(reinterpret_cast<uintptr_t>(part) + kPartFlagsOffset);
    if (!(flags & 1)) continue;
    auto pos = reinterpret_cast<float*>(part + kPartPosOffset);
    float dx = pos[0] - eye[0], dy = pos[1] - eye[1], dz = pos[2] - eye[2];
    if (dx * dx + dy * dy + dz * dz > kHideRadius * kHideRadius) continue;
    flags &= ~1;  // the draw takes it for a free slot; it's live again before anyone else looks
    hidden[count++] = part;
  }
  reinterpret_cast<DrawFn>(kDrawFn)(pass);
  for (int i = 0; i < count; ++i) At<uint8_t>(reinterpret_cast<uintptr_t>(hidden[i]) + kPartFlagsOffset) |= 1;
}

void __cdecl HookProject(float* eye, float* right, float* forward) {
  reinterpret_cast<ProjectFn>(kProjectFn)(eye, right, forward);
  uintptr_t effect = kEffectPool;
  for (int i = 0; i < kEffectCount; ++i, effect += kEffectStride) {
    if (!At<uint8_t>(effect + kEffectLiveOffset) || At<uintptr_t>(effect + kEffectFinishOffset) != kCounterAddThunk)
      continue;
    auto& scale = At<float>(effect + kEffectStartScaleOffset);
    if (scale > kMaxStartScale) scale = kMaxStartScale;
  }
}

}  // namespace

void Install() {
  hook::PatchCall(reinterpret_cast<void*>(kDrawCallSite), reinterpret_cast<void*>(kDrawFn), HookDraw);
  hook::PatchCall(reinterpret_cast<void*>(kProjectCallSite), reinterpret_cast<void*>(kProjectFn), HookProject);
}

}  // namespace studs
