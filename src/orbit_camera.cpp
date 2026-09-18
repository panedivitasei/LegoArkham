#include "orbit_camera.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "hook.h"
#include "mouse.h"
#include "mouse_controls.h"
#include "player2.h"
#include "vehicle.h"

namespace orbit {
namespace {

// LEGOBatman.exe addresses. The exe has no ASLR, so these are absolute.
constexpr uintptr_t kUpdateCallSite = 0x4AF7F8;  // call GameCamera_Update(g_GameCamera)
constexpr uintptr_t kUpdateThunk = 0x401BEF;
constexpr uintptr_t kNuCameraPtr = 0xA96128;
constexpr uintptr_t kNuCameraSet = 0x6F1E80;
constexpr uintptr_t kPlayer1Ptr = 0xAB3980;

constexpr int kCameraMatrixOffset = 184;  // row-major world matrix: right, up, forward, position
constexpr int kCameraYawOffset = 0x1FC;   // int16, 65536 per turn, same angle as atan2(forward.x, forward.z)
constexpr int kPlayerPosOffset = 92;

constexpr float kPi = 3.14159265f;

constexpr float kTargetHeight = 0.4f;  // roughly the middle of a minifigure
constexpr float kShoulder = 0.0f;
constexpr float kYawSpeed = 3.0f;  // radians per second at full stick
constexpr float kPitchSpeed = 2.0f;
constexpr float kMinPitch = -0.35f;
constexpr float kMaxPitch = 1.2f;

using UpdateFn = int(__cdecl*)(BYTE* camera);
using NuCameraSetFn = void(__cdecl*)(float* camera);

bool active;
// The "Camera toggle" button (F1 by default) hands player 1 between the orbit and the game's own
// camera. The game hardwires F1 to attack on keyboard slots, so that read is pointed at an
// unused scancode.
bool enabled = true;
constexpr uintptr_t kF1AttackRead = 0x5215E5;  // cmp byte_9CFB8B[edx], 0 in the hardwired-keys read
float yaw, pitch;
float shownYaw;  // of the blended camera on screen
float distance;
float zoom = 1.0f;                   // on the distance; vehicles get more room
constexpr float kDrivingZoom = 1.5625f;  // 25 percent out, then 25 percent more
LARGE_INTEGER lastTick;

template <class T>
T Read(uintptr_t address) {
  return *reinterpret_cast<T*>(address);
}

constexpr uintptr_t kMenuState = 0x60CEE0;  // -1 while no menu is open; the Batcomputer and panels are menus
using MenuFn = int(__cdecl*)();

// The game keeps its own camera for cutscenes, fades and scripted shots (the same early-outs
// GameCamera_Update takes), for open menus, and for co-op, etc.
constexpr int kContextOffset = 2523;  // the character's current action
std::string iniPath;
bool interactState[256];  // the actions that hand the camera back to the game, e.g. using a panel

// Re-read from the ini every second, so states can be added while the game runs.
void ReloadInteractStates() {
  char list[512];
  GetPrivateProfileStringA("Camera", "InteractStates", "81", list, sizeof(list), iniPath.c_str());
  memset(interactState, 0, sizeof(interactState));
  for (char* token = strtok(list, ", "); token; token = strtok(nullptr, ", ")) {
    int state = atoi(token);
    if (state >= 0 && state < 256) interactState[state] = true;
  }
}

bool UseGameCamera(const BYTE* player) {
  static DWORD lastReload;
  DWORD now = GetTickCount();
  if (now - lastReload > 1000) {
    lastReload = now;
    ReloadInteractStates();
  }
  return Read<int>(0xACB714) || (Read<BYTE>(0xAD7555) && Read<int>(0xACB760)) || Read<int>(0x9C5A10) ||
         Read<int>(0xACA89C) || Read<float>(0xA97C54) < 1.0f || reinterpret_cast<MenuFn>(kMenuState)() != -1 ||
         interactState[player[kContextOffset]] || player2::IsHuman();
}

float FrameSeconds() {
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  float dt = lastTick.QuadPart ? float(now.QuadPart - lastTick.QuadPart) / float(freq.QuadPart) : 0.0f;
  lastTick = now;
  return std::min(dt, 0.1f);
}

void ReadRightStick(float& x, float& y) {
  x = y = 0.0f;
  XINPUT_STATE state{};
  if (XInputGetState(0, &state) != ERROR_SUCCESS) return;

  float sx = state.Gamepad.sThumbRX / 32767.0f;
  float sy = state.Gamepad.sThumbRY / 32767.0f;
  float length = std::sqrt(sx * sx + sy * sy);
  float deadzone = XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE / 32767.0f;
  if (length <= deadzone) return;

  float scale = std::min((length - deadzone) / (1.0f - deadzone), 1.0f) / length;
  x = sx * scale;
  y = sy * scale;
}

// Mouse look. The cursor is hidden and kept inside the window while the orbit is in control.
bool mouseLook;
float mouseSensitivity = 1.0f;
constexpr float kRadiansPerCount = 0.0022f;  // at sensitivity 1, a full turn is about 2900 counts
mouse::Delta look;

// The mouse turns the camera when the game is in front and the mouse isn't aiming a batarang.
void ReadMouse(float& yawDelta, float& pitchDelta) {
  float dx, dy;
  look.Take(dx, dy);
  yawDelta = pitchDelta = 0.0f;
  if (!mouseLook) return;
  bool inFront = mouse::InFront();
  mouse::Capture(inFront && active);
  if (inFront && !mousectl::Aiming()) {
    yawDelta = dx * kRadiansPerCount * mouseSensitivity;
    pitchDelta = dy * kRadiansPerCount * mouseSensitivity;
  }
}

// The game's ray test against level collision; on a hit it shortens `ray` to the hit in place.
// The game's own camera calls it this way to keep itself out of walls.
constexpr uintptr_t kRayCast = 0x572D90;
using RayCastFn = int(__cdecl*)(float* start, float* ray, float, float, int, int flags);
constexpr float kWallMargin = 0.25f;  // how far in front of a wall the camera stops
constexpr float kMinDistance = 0.4f;
constexpr float kPullInRate = 10.0f;  // per second: eased, the wall surface itself is the hard stop
constexpr float kPushOutRate = 4.0f;  // per second: a gentle drift back out
constexpr float kSurfaceMargin = 0.05f;
bool collide;
float clearDistance;  // the eased distance the camera is allowed
float frameDt;

// Pulls the camera position `p` toward `from` until it clears the level, easing the change.
void KeepOutOfWalls(const float* from, float* p) {
  float ray[3] = {p[0] - from[0], p[1] - from[1], p[2] - from[2]};
  float wanted = std::sqrt(ray[0] * ray[0] + ray[1] * ray[1] + ray[2] * ray[2]);
  float allowed = wanted, surface = wanted;
  float probe[3] = {ray[0], ray[1], ray[2]};
  if (reinterpret_cast<RayCastFn>(kRayCast)(const_cast<float*>(from), probe, 0.0f, 1.0f, 0, 0)) {
    float hit = std::sqrt(probe[0] * probe[0] + probe[1] * probe[1] + probe[2] * probe[2]);
    allowed = std::clamp(hit - kWallMargin, kMinDistance, wanted);
    surface = std::max(hit - kSurfaceMargin, kMinDistance);
  }
  float rate = allowed < clearDistance ? kPullInRate : kPushOutRate;
  clearDistance += (allowed - clearDistance) * std::min(rate * frameDt, 1.0f);
  clearDistance = std::min(clearDistance, surface);  // eased in, but never inside the wall
  float scale = wanted > 1e-4f ? clearDistance / wanted : 0.0f;
  for (int i = 0; i < 3; ++i) p[i] = from[i] + ray[i] * scale;
}

void BuildMatrix(const float* target, float* m) {
  const float f[3] = {std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
  const float r[3] = {std::cos(yaw), 0.0f, -std::sin(yaw)};
  const float u[3] = {f[1] * r[2], f[2] * r[0] - f[0] * r[2], -f[1] * r[0]};
  const float from[3] = {target[0], target[1] + kTargetHeight, target[2]};
  float reach = distance * zoom;
  float p[3] = {from[0] - f[0] * reach + r[0] * kShoulder, from[1] - f[1] * reach,
                from[2] - f[2] * reach + r[2] * kShoulder};
  if (collide) KeepOutOfWalls(from, p);

  const float rows[16] = {r[0], r[1], r[2], 0, u[0], u[1], u[2], 0, f[0], f[1], f[2], 0, p[0], p[1], p[2], 1};
  memcpy(m, rows, sizeof(rows));
}

float ease = 0.5f;  // seconds to blend between the game's camera and the orbit
float blend;        // 0 = the game's camera, 1 = the orbit

bool focused;
float focus[3];
constexpr float kFocusRate = 5.0f;  // per second, how fast the orbit swings onto a focus point

// Mixes two camera matrices `t` of the way from `a` to `b`: positions lerp, the view direction
// lerps and the frame is rebuilt around it with world up, so the horizon stays level.
void BlendCamera(const float* a, const float* b, float t, float* out) {
  float f[3], p[3];
  for (int i = 0; i < 3; ++i) {
    f[i] = a[8 + i] + (b[8 + i] - a[8 + i]) * t;
    p[i] = a[12 + i] + (b[12 + i] - a[12 + i]) * t;
  }
  float size = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
  for (float& v : f) v /= size > 1e-6f ? size : 1.0f;
  float r[3] = {f[2], 0.0f, -f[0]};  // world up x forward
  size = std::sqrt(r[0] * r[0] + r[2] * r[2]);
  for (float& v : r) v /= size > 1e-6f ? size : 1.0f;
  const float u[3] = {f[1] * r[2], f[2] * r[0] - f[0] * r[2], -f[1] * r[0]};
  const float rows[16] = {r[0], r[1], r[2], 0, u[0], u[1], u[2], 0, f[0], f[1], f[2], 0, p[0], p[1], p[2], 1};
  memcpy(out, rows, sizeof(rows));
}

int __cdecl HookUpdate(BYTE* camera) {
  int result = reinterpret_cast<UpdateFn>(kUpdateThunk)(camera);  // the game's camera for this frame
  float dt = FrameSeconds();
  frameDt = dt;
  float mouseYaw, mousePitch;
  ReadMouse(mouseYaw, mousePitch);  // every frame, so the cursor is released when the orbit isn't in control

  auto player = Read<BYTE*>(kPlayer1Ptr);
  if (player2::CameraTogglePressed() && reinterpret_cast<MenuFn>(kMenuState)() == -1) enabled = !enabled;
  bool wantOrbit = enabled && player && !UseGameCamera(player);
  float step = dt / std::max(ease, 0.01f);
  blend = std::clamp(blend + (wantOrbit ? step : -step), 0.0f, 1.0f);
  if (blend <= 0.0f || !player) {
    active = false;
    return result;
  }

  auto matrix = reinterpret_cast<float*>(camera + kCameraMatrixOffset);
  if (!active) {
    // Pick up from wherever the game camera was looking so the handover doesn't jump.
    yaw = std::atan2(matrix[8], matrix[10]);
    pitch = std::asin(-matrix[9]);
    clearDistance = distance;
    active = true;
  }

  if (focused) {
    // Ease onto the line from the character to the focus point, looking a little down on it.
    auto pos = reinterpret_cast<float*>(player + kPlayerPosOffset);
    float d[3] = {focus[0] - pos[0], focus[1] - (pos[1] + kTargetHeight), focus[2] - pos[2]};
    float flat = std::sqrt(d[0] * d[0] + d[2] * d[2]);
    float wantYaw = std::atan2(d[0], d[2]);
    float wantPitch = std::clamp(-std::atan2(d[1], std::max(flat, 0.1f)) * 0.5f, kMinPitch, kMaxPitch);
    float turn = std::remainder(wantYaw - yaw, 2.0f * kPi);
    float rate = std::min(kFocusRate * dt, 1.0f);
    yaw += turn * rate;
    pitch += (wantPitch - pitch) * rate;
  } else {
    float x, y;
    ReadRightStick(x, y);
    yaw += x * kYawSpeed * dt + mouseYaw;
    pitch = std::clamp(pitch - y * kPitchSpeed * dt + mousePitch, kMinPitch, kMaxPitch);
  }

  float wantZoom = vehicle::Driving() ? kDrivingZoom * vehicle::ZoomScale() : 1.0f;
  zoom += (wantZoom - zoom) * std::min(3.0f * dt, 1.0f);
  float orbitMatrix[16];
  BuildMatrix(reinterpret_cast<float*>(player + kPlayerPosOffset), orbitMatrix);
  float t = blend * blend * (3.0f - 2.0f * blend);  // smoothstep
  if (t >= 1.0f) memcpy(matrix, orbitMatrix, sizeof(orbitMatrix));
  else BlendCamera(matrix, orbitMatrix, t, matrix);

  // Movement and other gameplay code steer by the game camera's yaw, so keep it pointing our way.
  // Not while driving: vehicles, and the chase levels especially, move along the game camera's
  // own forward, and that must stay the road however the orbit looks.
  shownYaw = std::atan2(matrix[8], matrix[10]);
  if (!vehicle::Driving())
    *reinterpret_cast<int16_t*>(camera + kCameraYawOffset) =
        static_cast<int16_t>(std::lround(shownYaw * 32768.0f / kPi));

  auto nuCamera = Read<float*>(kNuCameraPtr);
  memcpy(nuCamera, matrix, 16 * sizeof(float));
  reinterpret_cast<NuCameraSetFn>(kNuCameraSet)(nuCamera);
  return result;
}

}  // namespace

void Focus(const float* point) {
  memcpy(focus, point, sizeof(focus));
  focused = true;
}

void Unfocus() { focused = false; }

bool ViewYaw(float& out) {
  if (!active) return false;
  out = shownYaw;
  return true;
}

void Install(float cameraDistance, bool wallCollision, const std::string& ini) {
  distance = cameraDistance;
  collide = wallCollision;
  iniPath = ini;
  ease = GetPrivateProfileIntA("Camera", "Ease", 50, ini.c_str()) / 100.0f;
  mouseLook = GetPrivateProfileIntA("Camera", "MouseLook", 1, ini.c_str()) != 0;
  int sensitivity = GetPrivateProfileIntA("Camera", "MouseSensitivity", 100, ini.c_str());
  mouseSensitivity = std::clamp(sensitivity, 10, 1000) / 100.0f;
  mouse::Watch(&look);
  ReloadInteractStates();
  hook::PatchCall(reinterpret_cast<void*>(kUpdateCallSite), reinterpret_cast<void*>(kUpdateThunk), HookUpdate);
  hook::PatchBytes(reinterpret_cast<void*>(kF1AttackRead), {0x80, 0xBA, 0x8B, 0xFB, 0x9C, 0x00, 0x00},
                   {0x80, 0xBA, 0x4F, 0xFC, 0x9C, 0x00, 0x00});
}

}  // namespace orbit
