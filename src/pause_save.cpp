#include "pause_save.h"

#include <windows.h>

#include <cstdint>

#include "hook.h"

namespace pausesave {
namespace {

// LEGOBatman.exe addresses. The exe has no ASLR, so these are absolute.
// The pause menu (id 2) draws its entries in order each frame and the select handler dispatches
// by index, so a "Save Game" entry is slotted in right after "Options" (index 2).
constexpr uintptr_t kOptionsItemCall = 0x4B4149;  // call Menu_AddItem(menu, "Options") in the draw handler
constexpr uintptr_t kMenuAddItem = 0x60CEA0;
constexpr uintptr_t kPauseSelectSlot = 0x9410D4;  // the pause menu's select handler in the menu table
constexpr uintptr_t kPauseSelect = 0x4B3E80;
constexpr uintptr_t kRequestAutosave = 0x6C1D40;  // flags a save for the save manager, 0 if it can't
constexpr uintptr_t kUnpause = 0x5BDB90;
constexpr uintptr_t kMenuPush = 0x6BF620;
constexpr uintptr_t kSaveGameText = 0x99E42C;  // the localized "GEN_SAVEGAME" string
constexpr int kSaveFailedMenu = 1000;

constexpr int kSaveIndex = 2;
constexpr int kSelectedOffset = 52;  // the entry confirmed this frame
constexpr int kConfirmedOffset = 100;  // nonzero while a confirm is pending

using AddItemFn = int(__cdecl*)(int menu, const char* text);
using SelectFn = void(__cdecl*)(int menu);
using RequestSaveFn = int(__cdecl*)();
using UnpauseFn = int(__cdecl*)(int playSound);
using MenuPushFn = void*(__cdecl*)(int id, int cursor);

int __cdecl HookAddOptions(int menu, const char* text) {
  auto add = reinterpret_cast<AddItemFn>(kMenuAddItem);
  add(menu, text);
  auto save = *reinterpret_cast<const char**>(kSaveGameText);
  return add(menu, save ? save : "Save Game");
}

void __cdecl HookSelect(int menu) {
  auto selected = reinterpret_cast<int*>(menu + kSelectedOffset);
  if (!*reinterpret_cast<int*>(menu + kConfirmedOffset) || *selected < kSaveIndex) {
    reinterpret_cast<SelectFn>(kPauseSelect)(menu);
    return;
  }
  if (*selected == kSaveIndex) {
    // Same as the game's own hub exit: request the save and drop back to play, where the save
    // manager runs and shows its icon. No slot or autosave off gets the game's failure dialog.
    if (reinterpret_cast<RequestSaveFn>(kRequestAutosave)()) reinterpret_cast<UnpauseFn>(kUnpause)(1);
    else reinterpret_cast<MenuPushFn>(kMenuPush)(kSaveFailedMenu, -1);
    return;
  }
  // The game's entries below ours keep their original indices.
  int shown = *selected;
  *selected = shown - 1;
  reinterpret_cast<SelectFn>(kPauseSelect)(menu);
  if (*selected == shown - 1) *selected = shown;
}

}  // namespace

void Install() {
  hook::PatchCall(reinterpret_cast<void*>(kOptionsItemCall), reinterpret_cast<void*>(kMenuAddItem), HookAddOptions);
  auto detour = reinterpret_cast<uintptr_t>(&HookSelect);
  hook::PatchBytes(reinterpret_cast<void*>(kPauseSelectSlot), {0x80, 0x3E, 0x4B, 0x00},
                   {BYTE(detour), BYTE(detour >> 8), BYTE(detour >> 16), BYTE(detour >> 24)});
}

}  // namespace pausesave
