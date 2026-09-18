#include "mouse.h"

#include <cstdint>
#include <cstdlib>

namespace mouse {
namespace {

// The game reads its own DirectInput mouse once a frame, exclusive, with the axes in absolute
// mode, so each poll holds a running position; it keeps the low 16 bits of this and the previous
// poll and takes the wrapped difference as the frame's movement. Raw input can't sit next to
// that (a second registration in the process starves DirectInput's), so movement and buttons
// come from the game's copy, each poll taken once.
constexpr uintptr_t kMouseBufferToggle = 0x9D02A8;  // flips on every poll
constexpr uintptr_t kInputBlock = 0x9CFAF4;         // the input manager
constexpr int kPositionOffset = 25340;              // int16 x, y, z of this poll
constexpr int kPreviousOffset = 25346;              // and of the one before
constexpr int kButtonsOffset = 25396;               // the polled button bytes

// The mouse is non-exclusive, so the first poll after an alt-tab carries everything the mouse did
// meanwhile. The game sleeps while inactive, so a gap between polls marks that return.
constexpr DWORD kResumeGapMs = 250;
constexpr int kMaxCountsPerPoll = 1500;  // a hard swipe at 60 fps is a few hundred

Delta* watchers[4];
int watcherCount;
bool cursorHidden;
uint8_t lastToggle = 0xFF;
DWORD lastPollTick;

template <class T>
T& At(uintptr_t address) {
  return *reinterpret_cast<T*>(address);
}

void Poll() {
  uint8_t toggle = At<uint8_t>(kMouseBufferToggle);
  if (toggle == lastToggle) return;
  lastToggle = toggle;
  DWORD tick = GetTickCount();
  bool resumed = tick - lastPollTick > kResumeGapMs;
  lastPollTick = tick;
  uintptr_t block = At<uintptr_t>(kInputBlock);
  if (!block || resumed) return;
  auto now = reinterpret_cast<const int16_t*>(block + kPositionOffset);
  auto before = reinterpret_cast<const int16_t*>(block + kPreviousOffset);
  auto dx = static_cast<int16_t>(now[0] - before[0]);  // wraps like the game's own difference
  auto dy = static_cast<int16_t>(now[1] - before[1]);
  if (abs(dx) > kMaxCountsPerPoll || abs(dy) > kMaxCountsPerPoll) return;
  for (int i = 0; i < watcherCount; ++i) {
    watchers[i]->x += dx;
    watchers[i]->y += dy;
  }
}

HWND GameWindow() {
  HWND front = GetForegroundWindow();
  DWORD pid = 0;
  if (!front || (GetWindowThreadProcessId(front, &pid), pid != GetCurrentProcessId())) return nullptr;
  return front;
}

}  // namespace

void Watch(Delta* delta) {
  if (watcherCount < 4) watchers[watcherCount++] = delta;
}

bool InFront() {
  Poll();
  return GameWindow() != nullptr;
}

void Capture(bool on) {
  HWND window = GameWindow();
  if (on && window) {
    RECT r;
    GetClientRect(window, &r);
    MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&r), 2);
    ClipCursor(&r);  // Windows drops the clip on focus changes, hence every frame
  } else if (cursorHidden) {
    ClipCursor(nullptr);
  }
  if (on != cursorHidden) ShowCursor(!on);
  cursorHidden = on;
}

bool ButtonDown(int virtualKey) {
  int index = virtualKey == VK_LBUTTON ? 0 : virtualKey == VK_RBUTTON ? 1 : virtualKey == VK_MBUTTON ? 2 : -1;
  uintptr_t block = At<uintptr_t>(kInputBlock);
  return index >= 0 && block && At<uint8_t>(block + kButtonsOffset + index) != 0;
}

}  // namespace mouse
