#include "alt_f4.h"

#include "hook.h"

namespace altf4 {
namespace {

constexpr LPARAM kKeyRepeat = 1 << 30;

decltype(&RegisterClassExA) origRegisterClassExA;
WNDPROC gameWndProc;

// The game returns early on every WM_SYSKEYDOWN so Alt can't open the system menu, which also eats Alt+F4.
LRESULT CALLBACK WndProc(HWND window, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == WM_SYSKEYDOWN && wParam == VK_F4) {
    if (!(lParam & kKeyRepeat)) PostMessageA(window, WM_CLOSE, 0, 0);
    return 0;
  }
  return gameWndProc(window, msg, wParam, lParam);
}

ATOM WINAPI HookRegisterClassExA(const WNDCLASSEXA* wc) {
  if (gameWndProc || !wc) return origRegisterClassExA(wc);

  WNDCLASSEXA wrapped = *wc;
  gameWndProc = wrapped.lpfnWndProc;
  wrapped.lpfnWndProc = WndProc;
  return origRegisterClassExA(&wrapped);
}

}  // namespace

void Install(HMODULE game) {
  hook::Import(game, "user32.dll", "RegisterClassExA", HookRegisterClassExA, origRegisterClassExA);
}

}  // namespace altf4
