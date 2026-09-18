#include "hook.h"

#include <cstdint>
#include <cstring>

namespace hook {
namespace {

void* WriteSlot(void** slot, void* value) {
  DWORD old;
  VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
  void* previous = *slot;
  *slot = value;
  VirtualProtect(slot, sizeof(void*), old, &old);
  return previous;
}

}  // namespace

bool PatchBytes(void* address, std::initializer_list<BYTE> expected, std::initializer_list<BYTE> replacement) {
  if (expected.size() != replacement.size() || memcmp(address, expected.begin(), expected.size()) != 0) return false;

  DWORD old;
  VirtualProtect(address, replacement.size(), PAGE_EXECUTE_READWRITE, &old);
  memcpy(address, replacement.begin(), replacement.size());
  VirtualProtect(address, replacement.size(), old, &old);
  FlushInstructionCache(GetCurrentProcess(), address, replacement.size());
  return true;
}

bool PatchCall(void* site, void* expected, void* detour) {
  auto at = static_cast<BYTE*>(site);
  auto next = reinterpret_cast<intptr_t>(at + 5);
  bool callOrJump = at[0] == 0xE8 || at[0] == 0xE9;
  if (!callOrJump || next + *reinterpret_cast<int32_t*>(at + 1) != reinterpret_cast<intptr_t>(expected)) return false;

  auto rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(detour) - next);
  DWORD old;
  VirtualProtect(at + 1, sizeof(rel), PAGE_EXECUTE_READWRITE, &old);
  memcpy(at + 1, &rel, sizeof(rel));
  VirtualProtect(at + 1, sizeof(rel), old, &old);
  FlushInstructionCache(GetCurrentProcess(), at, 5);
  return true;
}

void* PatchImport(HMODULE module, const char* dll, const char* func, void* detour) {
  auto base = reinterpret_cast<BYTE*>(module);
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
  auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return nullptr;

  for (auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; desc++) {
    // Without the name table the slots are already bound and can't be matched by name.
    if (!desc->OriginalFirstThunk || _stricmp(reinterpret_cast<char*>(base + desc->Name), dll) != 0) continue;

    auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
    auto slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
    for (; names->u1.AddressOfData; names++, slots++) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
      auto import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
      if (strcmp(import->Name, func) == 0) return WriteSlot(reinterpret_cast<void**>(&slots->u1.Function), detour);
    }
  }
  return nullptr;
}

void* PatchVtable(void* object, int index, void* detour) {
  void** vtable = *static_cast<void***>(object);
  return WriteSlot(&vtable[index], detour);
}

}  // namespace hook
