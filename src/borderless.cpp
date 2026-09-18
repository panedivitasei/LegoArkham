#include "borderless.h"

#include <d3d9.h>

#include "hook.h"

namespace borderless {
namespace {

constexpr int kCreateDeviceSlot = 16;  // IDirect3D9::CreateDevice
constexpr int kResetSlot = 16;         // IDirect3DDevice9::Reset

constexpr DWORD kFrameExStyles =
    WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_TOPMOST;

using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                   D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

decltype(&Direct3DCreate9) origDirect3DCreate9;
decltype(&CreateWindowExA) origCreateWindowExA;
decltype(&MoveWindow) origMoveWindow;
CreateDeviceFn origCreateDevice;
ResetFn origReset;

HWND gameWindow;

RECT MonitorRect(HMONITOR monitor) {
  MONITORINFO info{sizeof(info)};
  GetMonitorInfoA(monitor, &info);
  return info.rcMonitor;
}

void MakeWindowed(D3DPRESENT_PARAMETERS* pp) {
  if (!pp) return;
  pp->Windowed = TRUE;
  pp->FullScreen_RefreshRateInHz = 0;
}

HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp) {
  MakeWindowed(pp);
  return origReset(device, pp);
}

HRESULT STDMETHODCALLTYPE HookCreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                           D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** device) {
  MakeWindowed(pp);
  HRESULT hr = origCreateDevice(d3d, adapter, type, focus, flags, pp, device);
  if (FAILED(hr)) return hr;

  hook::Vtable(*device, kResetSlot, HookReset, origReset);

  // A fullscreen device shows the window itself, and the game relies on that.
  if (gameWindow && !IsWindowVisible(gameWindow)) {
    ShowWindow(gameWindow, SW_SHOW);
    SetForegroundWindow(gameWindow);
  }
  return hr;
}

IDirect3D9* WINAPI HookDirect3DCreate9(UINT sdkVersion) {
  IDirect3D9* d3d = origDirect3DCreate9(sdkVersion);
  if (d3d) hook::Vtable(d3d, kCreateDeviceSlot, HookCreateDevice, origCreateDevice);
  return d3d;
}

HWND WINAPI HookCreateWindowExA(DWORD exStyle, LPCSTR className, LPCSTR title, DWORD style, int x, int y, int width,
                                int height, HWND parent, HMENU menu, HINSTANCE instance, LPVOID param) {
  if (gameWindow || parent || (style & WS_CHILD))
    return origCreateWindowExA(exStyle, className, title, style, x, y, width, height, parent, menu, instance, param);

  // The exe isn't DPI aware, so Windows would otherwise upscale the window on scaled displays.
  SetProcessDPIAware();

  RECT r = MonitorRect(MonitorFromPoint({x, y}, MONITOR_DEFAULTTOPRIMARY));
  gameWindow = origCreateWindowExA(exStyle & ~kFrameExStyles, className, title, (style & WS_VISIBLE) | WS_POPUP,
                                   r.left, r.top, r.right - r.left, r.bottom - r.top, parent, menu, instance, param);
  return gameWindow;
}

// The game resizes its window to the render resolution; keep it covering the monitor instead.
BOOL WINAPI HookMoveWindow(HWND window, int x, int y, int width, int height, BOOL repaint) {
  if (window != gameWindow) return origMoveWindow(window, x, y, width, height, repaint);

  RECT r = MonitorRect(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY));
  return origMoveWindow(window, r.left, r.top, r.right - r.left, r.bottom - r.top, repaint);
}

}  // namespace

void Install(HMODULE game) {
  hook::Import(game, "d3d9.dll", "Direct3DCreate9", HookDirect3DCreate9, origDirect3DCreate9);
  hook::Import(game, "user32.dll", "CreateWindowExA", HookCreateWindowExA, origCreateWindowExA);
  hook::Import(game, "user32.dll", "MoveWindow", HookMoveWindow, origMoveWindow);
}

}  // namespace borderless
