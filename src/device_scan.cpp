#include "device_scan.h"

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <xinput.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>

#include "hook.h"

namespace devscan {
namespace {

constexpr uintptr_t kIsXInputCallSite = 0x523001;  // the enum callback's call to the WMI check
constexpr uintptr_t kIsXInputDevice = 0x51E280;
constexpr int kEnumDevicesSlot = 4;  // IDirectInput8::EnumDevices

// The game's device rescan; it runs three times, two seconds apart, after any device-change notification.
constexpr uintptr_t kRescanCallSite = 0x523CC3;  // Input_ReadSlot's call to it
constexpr uintptr_t kRescan = 0x5233F0;
constexpr uintptr_t kEnumCallback = 0x522FE0;  // the game's DirectInput enumeration callback
constexpr uintptr_t kPadCapabilities = 0x51D9B0;  // (XINPUT_CAPABILITIES*, record): fills a pad record
constexpr uintptr_t kInputBlock = 0x9CFAF4;
constexpr uintptr_t kKeyboardDevice = 0x9D08AC;  // the one DirectInput keyboard, shared by slots 10-13
constexpr uintptr_t kNextPadPort = 0x9CFF8C;     // which XInput port the next enumerated pad gets
constexpr int kRecords = 20020;                   // slot records in the block, 380 bytes each
constexpr int kRecordStride = 380;
constexpr int kRecordDevice = 4;    // IDirectInputDevice8*
constexpr int kRecordFlag = 42;     // set on a live XInput pad
constexpr int kRecordPresent = 45;
constexpr int kDeviceCount = 12;    // on the input object: live pads

using EnumDevicesFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInput8A*, DWORD, LPDIENUMDEVICESCALLBACKA, void*, DWORD);
using PadCapabilitiesFn = int(__stdcall*)(const XINPUT_CAPABILITIES*, uint8_t* record);
EnumDevicesFn origEnumDevices;

template <class T>
T& At(uintptr_t address) {
  return *reinterpret_cast<T*>(address);
}

// A DirectInput enumeration costs 100+ ms here and the game's rescan also recreates its keyboard,
// so each class is only redone when Raw Input shows a device of that class came or went. A device
// waking from sleep isn't new: its instance is the cached one.
struct RawDevice {
  DWORD type;  // RIM_TYPEKEYBOARD, RIM_TYPEMOUSE, or RIM_TYPEHID for a pad
  std::wstring path;
  bool operator==(const RawDevice& o) const { return type == o.type && path == o.path; }
  bool operator<(const RawDevice& o) const { return type != o.type ? type < o.type : path < o.path; }
};
std::vector<RawDevice> rawDevices;

struct DeviceClass {
  DWORD rawType;
  DWORD diClass;
  std::vector<RawDevice> current;    // this type's devices as of the last read
  std::vector<RawDevice> rescanned;  // and as of the game's last rescan of it
  std::vector<std::wstring> everSeen;
  bool cached = false;
  std::vector<DIDEVICEINSTANCEA> instances;

  bool Changed() const { return !cached || current != rescanned; }
  bool NewDevice() const {
    for (const auto& d : current)
      if (std::find(everSeen.begin(), everSeen.end(), d.path) == everSeen.end()) return true;
    return false;
  }
};
DeviceClass keyboards{RIM_TYPEKEYBOARD, DI8DEVCLASS_KEYBOARD}, pointers{RIM_TYPEMOUSE, DI8DEVCLASS_POINTER},
    controllers{RIM_TYPEHID, DI8DEVCLASS_GAMECTRL};

DeviceClass* ClassFor(DWORD diClass) {
  return diClass == DI8DEVCLASS_KEYBOARD  ? &keyboards
         : diClass == DI8DEVCLASS_POINTER ? &pointers
         : diClass == DI8DEVCLASS_GAMECTRL ? &controllers
                                            : nullptr;
}

void ReadRawDevices() {
  UINT count = 0;
  GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST));
  std::vector<RAWINPUTDEVICELIST> list(count);
  count = GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST));
  if (count == UINT(-1)) return;

  std::vector<RawDevice> devices;
  for (UINT i = 0; i < count; ++i) {
    wchar_t path[512];
    UINT size = 512;
    if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, path, &size) <= 0) continue;
    if (list[i].dwType == RIM_TYPEHID) {
      // Only joysticks and gamepads count as pads; a wireless keyboard's extra collections don't.
      RID_DEVICE_INFO info{};
      info.cbSize = size = sizeof(info);
      if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICEINFO, &info, &size) <= 0) continue;
      bool pad = info.hid.usUsagePage == 1 && (info.hid.usUsage == 4 || info.hid.usUsage == 5);
      if (!pad && !wcsstr(path, L"IG_")) continue;
    }
    devices.push_back({list[i].dwType, path});
  }
  std::sort(devices.begin(), devices.end());
  rawDevices = std::move(devices);
  for (DeviceClass* c : {&keyboards, &pointers, &controllers}) {
    c->current.clear();
    for (const auto& d : rawDevices)
      if (d.type == c->rawType) c->current.push_back(d);
  }
}

DWORD HexAfter(const std::wstring& path, const wchar_t* key) {
  size_t at = path.find(key);
  return at == std::wstring::npos ? 0 : wcstoul(path.c_str() + at + wcslen(key), nullptr, 16);
}

// Stands in for the game's WMI query: XInput devices carry "IG_" plus the VID/PID DirectInput
// packs into guidProduct. Steam Input's virtual pad exists only in-process, so Microsoft-vendor
// pads count as XInput outright or the game registers them twice.
int __cdecl IsXInputDevice(const GUID* product) {
  if ((product->Data1 & 0xFFFF) == 0x045E) return 1;
  for (const auto& d : rawDevices) {
    if (d.path.find(L"IG_") == std::wstring::npos) continue;
    if ((HexAfter(d.path, L"VID_") | (HexAfter(d.path, L"PID_") << 16)) == product->Data1) return 1;
  }
  return 0;
}

BOOL CALLBACK Capture(const DIDEVICEINSTANCEA* instance, void* ref) {
  static_cast<std::vector<DIDEVICEINSTANCEA>*>(ref)->push_back(*instance);
  return DIENUM_CONTINUE;
}

HRESULT STDMETHODCALLTYPE HookEnumDevices(IDirectInput8A* dinput, DWORD devClass, LPDIENUMDEVICESCALLBACKA callback,
                                          void* ref, DWORD flags) {
  DeviceClass* c = ClassFor(devClass);
  if (!c || flags != DIEDFL_ATTACHEDONLY) return origEnumDevices(dinput, devClass, callback, ref, flags);

  ReadRawDevices();
  if (!c->cached || c->NewDevice()) {
    c->instances.clear();
    HRESULT hr = origEnumDevices(dinput, devClass, Capture, &c->instances, flags);
    if (FAILED(hr)) return hr;
    for (const auto& d : c->current)
      if (std::find(c->everSeen.begin(), c->everSeen.end(), d.path) == c->everSeen.end()) c->everSeen.push_back(d.path);
    c->cached = true;
  }
  for (const auto& instance : c->instances)
    if (callback(&instance, ref) == DIENUM_STOP) break;
  return DI_OK;
}

// The game's rescan with unchanged classes skipped; same steps and order as the original otherwise.
void __fastcall Rescan(uint32_t* input, void*) {
  auto block = At<uint8_t*>(kInputBlock);
  auto dinput = reinterpret_cast<IDirectInput8A*>(input[kDeviceCount]);
  auto callback = reinterpret_cast<LPDIENUMDEVICESCALLBACKA>(kEnumCallback);
  ReadRawDevices();
  bool pads = controllers.Changed(), keys = keyboards.Changed(), mice = pointers.Changed();
  if (!pads && !keys && !mice) return;
  auto record = [&](int slot) { return block + kRecords + kRecordStride * slot; };
  auto drop = [](IDirectInputDevice8A*& device) {
    if (!device) return;
    device->Unacquire();
    device->Release();
    device = nullptr;
  };

  if (pads) {
    input[3] = 0;
    for (int port = 0; port < 4; ++port) {
      XINPUT_CAPABILITIES caps;
      uint8_t* r = record(port);
      if (XInputGetCapabilities(port, XINPUT_FLAG_GAMEPAD, &caps) != ERROR_SUCCESS) {
        At<uint32_t>(uintptr_t(r)) = 0;
        r[kRecordPresent] = 0;
        At<uint32_t>(uintptr_t(r + kRecordDevice)) = 0;
      } else {
        r[kRecordPresent] = 1;
        reinterpret_cast<PadCapabilitiesFn>(kPadCapabilities)(&caps, r);
        r[kRecordFlag] = 1;
        ++input[3];
      }
    }
    for (int slot = 4; slot < 10; ++slot) {
      uint8_t* r = record(slot);
      drop(At<IDirectInputDevice8A*>(uintptr_t(r + kRecordDevice)));
      At<uint32_t>(uintptr_t(r)) = 0;
      r[kRecordPresent] = 0;
    }
  }
  if (keys) {
    auto& keyboard = At<IDirectInputDevice8A*>(kKeyboardDevice);
    if (keyboard) {
      drop(keyboard);
      for (int slot = 10; slot < 14; ++slot) {
        uint8_t* r = record(slot);
        At<uint32_t>(uintptr_t(r + kRecordDevice)) = 0;
        At<uint32_t>(uintptr_t(r)) = 0;
        r[kRecordPresent] = 0;
      }
    }
  }
  if (pads) {
    input[3] = 0;
    At<DWORD>(kNextPadPort) = 0;
  }
  if (keys) dinput->EnumDevices(DI8DEVCLASS_KEYBOARD, callback, nullptr, DIEDFL_ATTACHEDONLY), keyboards.rescanned = keyboards.current;
  if (mice) dinput->EnumDevices(DI8DEVCLASS_POINTER, callback, nullptr, DIEDFL_ATTACHEDONLY), pointers.rescanned = pointers.current;
  if (pads) dinput->EnumDevices(DI8DEVCLASS_GAMECTRL, callback, nullptr, DIEDFL_ATTACHEDONLY), controllers.rescanned = controllers.current;
}

}  // namespace

void Install() {
  hook::PatchCall(reinterpret_cast<void*>(kIsXInputCallSite), reinterpret_cast<void*>(kIsXInputDevice),
                  reinterpret_cast<void*>(IsXInputDevice));
  hook::PatchCall(reinterpret_cast<void*>(kRescanCallSite), reinterpret_cast<void*>(kRescan),
                  reinterpret_cast<void*>(Rescan));
}

void Attach(void* directInput8) {
  hook::Vtable(directInput8, kEnumDevicesSlot, HookEnumDevices, origEnumDevices);
}

}  // namespace devscan
