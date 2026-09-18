#include "vsync_default.h"

#include "hook.h"

namespace vsync {

void DefaultOn(HMODULE game) {
  // The config defaults write `mov byte ptr [esi+0x428], 0` for VerticalSync. They only run when pcconfig.txt is missing.
  auto site = reinterpret_cast<BYTE*>(game) + 0x12419a;
  hook::PatchBytes(site, {0xC6, 0x86, 0x28, 0x04, 0x00, 0x00, 0x00}, {0xC6, 0x86, 0x28, 0x04, 0x00, 0x00, 0x01});
}

}  // namespace vsync
