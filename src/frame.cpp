#include "frame.h"

#include <cstdint>

#include "hook.h"

namespace frame {
namespace {

// LEGOBatman.exe addresses. The exe has no ASLR, so these are absolute.
constexpr uintptr_t kLevelUpdateCallSite = 0x4AEEEA;  // the level's once-per-frame update
constexpr uintptr_t kLevelUpdate = 0x5A3510;

using UpdateFn = int(__cdecl*)(int, int, int, int);

void (*callbacks[8])();
int count;

int __cdecl HookLevelUpdate(int a, int b, int c, int d) {
  for (int i = 0; i < count; ++i) callbacks[i]();
  return reinterpret_cast<UpdateFn>(kLevelUpdate)(a, b, c, d);
}

}  // namespace

void OnLevelUpdate(void (*fn)()) {
  if (count == 0)
    hook::PatchCall(reinterpret_cast<void*>(kLevelUpdateCallSite), reinterpret_cast<void*>(kLevelUpdate),
                    HookLevelUpdate);
  if (count < 8) callbacks[count++] = fn;
}

}  // namespace frame
