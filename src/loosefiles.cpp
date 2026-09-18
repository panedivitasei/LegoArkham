#include "loosefiles.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "hook.h"

namespace loosefiles {
namespace {

// The file layer tries the DAT directory before the disk, from four places: open, exists, size
// and stream offset. All four are told "not in the DAT" for any name present under the game folder.
constexpr uintptr_t kDatLookup = 0x6DCBC0;  // (directory, name): entry index, or -1
constexpr uintptr_t kDatLookupCallSites[] = {0x6DCD0B, 0x6DD9BC, 0x6DDA36, 0x6DDAD5};

using LookupFn = int(__cdecl*)(void* directory, const char* name);

bool OnDisk(const char* name) {
  if (!name || !*name) return false;
  if (*name == '@') name += 4;  // the layer's own prefix, skipped by the lookup too
  char path[MAX_PATH];
  if (strlen(name) >= sizeof(path)) return false;
  strcpy_s(path, name);
  DWORD attributes = GetFileAttributesA(path);
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// Animations are asked for as NAME.bsa first and NAME_pc.an3 only if that isn't found. A loose
// _pc.an3 for the clip hides the DAT's .bsa, so the game falls through to it.
bool ShadowedByLooseAn3(const char* name) {
  if (!name || *name == '@') name = name ? name + 4 : "";
  size_t n = strlen(name);
  if (n < 5 || n + 8 >= MAX_PATH || _stricmp(name + n - 4, ".bsa") != 0) return false;
  char path[MAX_PATH];
  memcpy(path, name, n - 4);
  strcpy_s(path + n - 4, sizeof(path) - (n - 4), "_pc.an3");
  return OnDisk(path);
}

int __cdecl HookLookup(void* directory, const char* name) {
  if (OnDisk(name) || ShadowedByLooseAn3(name)) return -1;
  return reinterpret_cast<LookupFn>(kDatLookup)(directory, name);
}

// Character clips and txts come from .fpk packs and never hit the DAT lookup, so the pack's find and
// fetch are hooked too and a loose file of the same name supplies the bytes. They must outlive the
// level, so each file is read once and kept.
constexpr uintptr_t kPackFind = 0x6DCF10;  // (pack, name): entry index + 1, or 0
constexpr uintptr_t kPackData = 0x6D3D10;  // (pack, index, &data, &size): 1 on success
constexpr uintptr_t kPackFindCallSites[] = {0x599847, 0x5998B3, 0x599976, 0x5999E2, 0x599B36, 0x599B94, 0x599C5F, 0x599CEB,  // animations
                                            0x6293D7, 0x6294B0, 0x6295FD};                                                  // character txt
constexpr uintptr_t kPackDataCallSites[] = {0x599B4E, 0x599BAC, 0x599C77, 0x599D03, 0x6293F8, 0x6294C8, 0x629619};
constexpr int kMaxLooseClips = 64;

using PackFindFn = int(__cdecl*)(void* pack, const char* name);
using PackDataFn = int(__cdecl*)(void* pack, int index, void** data, DWORD* size);

struct LooseClip {
  char name[MAX_PATH];
  void* data;
  DWORD size;
} clips[kMaxLooseClips];
int clipCount;
LooseClip* pending;  // the clip the next data fetch is for

LooseClip* LoadClip(const char* name) {
  for (int i = 0; i < clipCount; ++i)
    if (_stricmp(clips[i].name, name) == 0) return &clips[i];
  if (clipCount >= kMaxLooseClips) return nullptr;
  HANDLE file = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return nullptr;
  DWORD size = GetFileSize(file, nullptr), read = 0;
  void* data = size ? VirtualAlloc(nullptr, size + 16, MEM_COMMIT, PAGE_READWRITE) : nullptr;
  bool ok = data && ReadFile(file, data, size, &read, nullptr) && read == size;
  CloseHandle(file);
  if (!ok) return nullptr;
  LooseClip& clip = clips[clipCount++];
  strcpy_s(clip.name, name);
  clip.data = data;
  clip.size = size;
  return &clip;
}

int __cdecl HookPackFind(void* pack, const char* name) {
  int index = reinterpret_cast<PackFindFn>(kPackFind)(pack, name);
  pending = nullptr;
  if (OnDisk(name)) {
    pending = LoadClip(name);
    if (pending) return index ? index : 1;  // the pack needn't have it; the fetch is ours
  }
  return index;
}

int __cdecl HookPackData(void* pack, int index, void** data, DWORD* size) {
  LooseClip* clip = pending;
  pending = nullptr;
  if (clip) {
    if (data) *data = clip->data;
    if (size) *size = clip->size;
    return 1;
  }
  return reinterpret_cast<PackDataFn>(kPackData)(pack, index, data, size);
}

}  // namespace

void Install() {
  for (uintptr_t site : kPackFindCallSites)
    hook::PatchCall(reinterpret_cast<void*>(site), reinterpret_cast<void*>(kPackFind), HookPackFind);
  for (uintptr_t site : kPackDataCallSites)
    hook::PatchCall(reinterpret_cast<void*>(site), reinterpret_cast<void*>(kPackData), HookPackData);
  for (uintptr_t site : kDatLookupCallSites)
    hook::PatchCall(reinterpret_cast<void*>(site), reinterpret_cast<void*>(kDatLookup), HookLookup);
}

}  // namespace loosefiles
