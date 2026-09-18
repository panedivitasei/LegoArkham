#include "keybinds.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "hook.h"

namespace keybinds {
namespace {

// LEGOBatman.exe addresses. The exe has no ASLR, so these are absolute.
constexpr uintptr_t kInputManagerPtr = 0x9CFAF4;      // Block: the input manager
constexpr uintptr_t kLoadMappingsCallSite = 0x5237EE;  // call Input_LoadMappings(this) in Input_Manager_Init
constexpr uintptr_t kLoadMappings = 0x51CCB0;          // reads Mappings.dat into the 64 saved records
constexpr uintptr_t kSaveMappings = 0x522D00;          // writes them back
constexpr uintptr_t kDefaultRecords = 0x94AAC0;        // the built-in profiles, 156 bytes each

// A mapping record: the profile name, its type at +64 (1 = KeyboardMouse1), and 16 button
// bindings at +124 as {kind, code}, one per engine button bit. Kind 7 is a keyboard scancode,
// kind 4 a mouse button (0 left, 1 right, 2 middle).
constexpr int kRecordSize = 156;
constexpr int kRecordCount = 64;
constexpr int kRecordType = 64;
constexpr int kRecordButtons = 124;
constexpr int kKeyboardMouse1 = 1;
constexpr int kDefaultKeyboardMouse1 = 7;
constexpr uint8_t kKind_Key = 7;
constexpr uint8_t kKind_MouseButton = 4;

// Engine button bits, as record indices.
constexpr int kToggleUp = 3;  // the right bumper by default: "Character toggle up"
constexpr int kTag = 4;
constexpr int kGrab = 5;
constexpr int kJump = 6;
constexpr int kAttack = 7;
constexpr int kGrapple = 10;  // a stick click by default, and nothing in the game reads it
constexpr int kPlayer2Toggle = 8;  // two more the game never reads: player 2's join/leave key
constexpr int kCameraToggle = 9;   // and player 1's orbit/game camera switch
constexpr int kStart = 11;  // display only on a keyboard: the game clears it there and pauses on Escape itself

constexpr uint8_t kDikSpace = 0x39;
constexpr uint8_t kDikE = 0x12;
constexpr uint8_t kDikF = 0x21;
constexpr uint8_t kDik2 = 0x03;  // next to the game's "1" for toggle down
constexpr uint8_t kDikRControl = 0x9D;
constexpr uint8_t kDikF1 = 0x3B;
constexpr uint8_t kDikF2 = 0x3C;
constexpr uint8_t kDikLControl = 0x1D;
constexpr uint8_t kDikEscape = 0x01;

// The built-in default profiles, which "Reset to Defaults" hands out. The game rewrites the
// plain keyboard ones from layout tables during its own init, so the Arkham layout goes in
// right after that: the mouse profiles get the whole layout, the plain ones F for the grapple,
// pads whatever their bumper is.
constexpr uintptr_t kInstallDefaultsCallSite = 0x408B22;  // call Input_InstallKeyboardDefaults(...) in PC init
constexpr uintptr_t kInstallDefaults = 0x51F490;
constexpr int kDefaultMouseRecords[] = {7, 8};
constexpr int kDefaultKeyboardRecords[] = {7, 8, 9, 10};
constexpr int kDefaultPadRecords[] = {0, 1, 2, 3, 4, 5, 6, 11};
constexpr uint8_t kOldToggleUpKeys[] = {55, 4};  // the game's own: Numpad * on the mouse profiles, 3 on the rest

// The controls screen: its row labels are looked up once, at game start, from the game's text,
// and its rows come from a static list the descriptor points at. The grapple and the two
// toggles get rows of their own after "Character toggle up", named through label slots 45-47
// in the unused memory past the game's 45. Each time the screen opens, the game also hides the left/right movement rows
// of a keyboard-and-mouse profile and renames up/down "Walk"/"Run", for its own mouse-steering
// scheme; the mod steers with the keys, so those rows stay as they are on any profile.
constexpr uintptr_t kMenuInitThunk = 0x402937;  // jmp ControlsMenu_Init(desc, heap, heapEnd)
constexpr uintptr_t kMenuInit = 0x405B60;
constexpr uintptr_t kLabelTable = 0x9BD9E0;  // 45 char* entries
constexpr uintptr_t kGameRows = 0x9340B8;    // 8-byte rows {type, output entry, label, flags, float}, label 0xFF ends
constexpr uintptr_t kGameRowLists = 0x934180;  // the NULL-terminated array of row lists the init falls back to
constexpr int kDescRowLists = 8;             // in the descriptor: that array, left null by the caller
// Also in the descriptor: the keys the binding screen refuses, as a string of scancodes. The
// game's are F1, F2 and Escape; only Escape stays out, the other two are the toggles' defaults.
constexpr int kDescReservedKeys = 16;
constexpr char kReservedKeys[] = "\x01";
constexpr int kToggleUpEntry = 18;           // output entry of bit 3
constexpr int kGrappleEntry = 21;            // output entry of bit 10
constexpr int kPlayer2ToggleEntry = 22;      // of bit 8
constexpr int kCameraToggleEntry = 20;       // of bit 9
constexpr int kGrappleLabel = 45;            // the three labels past the game's 45 sit in zeroed, unreferenced memory
constexpr int kPlayer2ToggleLabel = 46;
constexpr int kCameraToggleLabel = 47;
constexpr int kMaxRows = 40;
constexpr char kGrappleName[] = "Grapple";
constexpr char kPlayer2ToggleName[] = "Player 2 toggle";
constexpr char kCameraToggleName[] = "Camera toggle";
constexpr uintptr_t kIsMouseSlotCallSites[] = {0x405E37, 0x405E4B, 0x405E63};  // in ControlsMenu_SetupRows
constexpr uintptr_t kIsMouseSlot = 0x51F9A0;                                    // slot == 10 || slot == 12
// The same scheme in play: the slot reader turns a keyboard-and-mouse slot's input into a stick
// pointing where the mouse has turned, at walking pace for W and running pace for S. Skipped,
// so the keys move the character like any other keyboard.
constexpr uintptr_t kMouseSteerCallSite = 0x523D7B;  // call Input_MouseSteer(slotRecord) in Input_ReadSlot
constexpr uintptr_t kMouseSteer = 0x51F650;
// The English input-name table's cells for the three mouse buttons, reworded the usual way round.
constexpr uintptr_t kMouseButtonNameCells[] = {0x94602C, 0x946030, 0x946034};
constexpr const char* kMouseButtonNames[] = {"LMB", "RMB", "MMB"};

struct Row {
  uint8_t type, entry, label, flags;
  float value;
};
Row rows[kMaxRows];
Row* rowLists[2] = {rows, nullptr};
bool rowsBuilt;

using ManagerFn = void*(__fastcall*)(uint8_t* manager, void* unused);
using MenuInitFn = int(__cdecl*)(int desc, int a2, int a3);
using InstallDefaultsFn = int(__cdecl*)(void* a1, void* a2, void* a3, void* a4, void* a5);

template <class T>
T& At(uintptr_t address) {
  return *reinterpret_cast<T*>(address);
}

uint8_t* RecordOfType(uint8_t* records, int type) {
  for (int i = 0; i < kRecordCount; ++i) {
    uint8_t* record = records + kRecordSize * i;
    if (record[0] && *reinterpret_cast<int*>(record + kRecordType) == type) return record;
  }
  return nullptr;
}

uint8_t* FreeRecord(uint8_t* records) {
  for (int i = 0; i < kRecordCount; ++i) {
    uint8_t* record = records + kRecordSize * i;
    if (!record[0]) return record;
  }
  return nullptr;
}

void Bind(uint8_t* record, int button, uint8_t kind, uint8_t code) {
  record[kRecordButtons + 2 * button] = kind;
  record[kRecordButtons + 2 * button + 1] = code;
}

bool Bound(const uint8_t* record, int button, uint8_t kind, uint8_t code) {
  const uint8_t* binding = record + kRecordButtons + 2 * button;
  return binding[0] == kind && binding[1] == code;
}

bool IsKeyboardRecord(const uint8_t* record) {
  int type = *reinterpret_cast<const int*>(record + kRecordType);
  return type >= 1 && type <= 4;
}

// A saved profile still on the game's default for the grapple's slot (a stick click, or right
// control on a keyboard) moves to the bumper or F. An earlier build put F on the toggle-up
// slot instead; that goes back to the game's own key. The two toggles get F2 and F1 on a
// keyboard when unbound or on the game's own leftovers; pads keep their default slots.
bool Migrate(uint8_t* record) {
  bool changed = false;
  if (IsKeyboardRecord(record)) {
    int type = *reinterpret_cast<const int*>(record + kRecordType);
    if (Bound(record, kToggleUp, kKind_Key, kDikF)) {
      Bind(record, kToggleUp, kKind_Key, type <= 2 ? kOldToggleUpKeys[0] : kOldToggleUpKeys[1]);
      changed = true;
    }
    // The mouse profiles' Numpad * for toggle up moves next to the "1" of toggle down.
    if (type <= 2 && Bound(record, kToggleUp, kKind_Key, kOldToggleUpKeys[0])) {
      Bind(record, kToggleUp, kKind_Key, kDik2);
      changed = true;
    }
    if (Bound(record, kGrapple, kKind_Key, kDikRControl) || Bound(record, kGrapple, 0, 0))
      Bind(record, kGrapple, kKind_Key, kDikF), changed = true;
    // The mouse profiles come with F1 and left control in these slots, plain keyboards with nothing.
    if (Bound(record, kPlayer2Toggle, 0, 0) || Bound(record, kPlayer2Toggle, kKind_Key, kDikF1))
      Bind(record, kPlayer2Toggle, kKind_Key, kDikF2), changed = true;
    if (Bound(record, kCameraToggle, 0, 0) || Bound(record, kCameraToggle, kKind_Key, kDikLControl))
      Bind(record, kCameraToggle, kKind_Key, kDikF1), changed = true;
    if (Bound(record, kStart, kKind_Key, kDikF2)) Bind(record, kStart, kKind_Key, kDikEscape), changed = true;
  } else {
    uint8_t kind = record[kRecordButtons + 2 * kGrapple], code = record[kRecordButtons + 2 * kGrapple + 1];
    bool stickClick = kind == 1 && (code == 8 || code == 9);  // on every default pad profile
    if (stickClick || kind == 0) {
      const uint8_t* bumper = record + kRecordButtons + 2 * kToggleUp;
      if (bumper[0]) Bind(record, kGrapple, bumper[0], bumper[1]), changed = true;
    }
  }
  return changed;
}

// Once the saved mappings are in, add the Arkham profile if the player has never saved one of
// their own; from then on the game's menu owns it.
void* __fastcall HookLoadMappings(uint8_t* manager, void* unused) {
  void* result = reinterpret_cast<ManagerFn>(kLoadMappings)(manager, unused);
  bool changed = false;
  for (int i = 0; i < kRecordCount; ++i) {
    uint8_t* record = manager + kRecordSize * i;
    if (record[0] && Migrate(record)) changed = true;
  }
  if (!RecordOfType(manager, kKeyboardMouse1)) {
    if (uint8_t* record = FreeRecord(manager)) {
      memcpy(record, reinterpret_cast<const void*>(kDefaultRecords + kRecordSize * kDefaultKeyboardMouse1),
             kRecordSize);
      Bind(record, kJump, kKind_Key, kDikSpace);
      Bind(record, kTag, kKind_Key, kDikE);
      Bind(record, kAttack, kKind_MouseButton, 0);
      Bind(record, kGrab, kKind_MouseButton, 1);
      Bind(record, kGrapple, kKind_Key, kDikF);
      Bind(record, kToggleUp, kKind_Key, kDik2);
      Bind(record, kPlayer2Toggle, kKind_Key, kDikF2);
      Bind(record, kCameraToggle, kKind_Key, kDikF1);
      Bind(record, kStart, kKind_Key, kDikEscape);
      changed = true;
    }
  }
  if (changed) reinterpret_cast<ManagerFn>(kSaveMappings)(manager, nullptr);
  return result;
}

// The game's rows with the Grapple and the two toggle rows slipped in after "Character toggle up".
void BuildRows() {
  auto game = reinterpret_cast<const Row*>(kGameRows);
  int n = 0;
  for (int i = 0; n < kMaxRows - 4; ++i) {
    rows[n++] = game[i];
    if (game[i].label == 0xFF) break;
    if (game[i].type == 0 && game[i].entry == kToggleUpEntry) {
      rows[n++] = Row{0, kGrappleEntry, kGrappleLabel, 0, 0.0f};
      rows[n++] = Row{0, kPlayer2ToggleEntry, kPlayer2ToggleLabel, 0, 0.0f};
      rows[n++] = Row{0, kCameraToggleEntry, kCameraToggleLabel, 0, 0.0f};
    }
  }
  rowsBuilt = true;
}

// The caller leaves the descriptor's row lists null and the init fills in the game's; ours go in
// first, so the init keeps them.
int __cdecl HookMenuInit(int desc, int a2, int a3) {
  auto& lists = At<Row**>(desc + kDescRowLists);
  if (!lists || lists == reinterpret_cast<Row**>(kGameRowLists)) {
    if (!rowsBuilt) BuildRows();
    lists = rowLists;
  }
  At<const char*>(desc + kDescReservedKeys) = kReservedKeys;
  int result = reinterpret_cast<MenuInitFn>(kMenuInit)(desc, a2, a3);
  At<const char*>(kLabelTable + 4 * kGrappleLabel) = kGrappleName;
  At<const char*>(kLabelTable + 4 * kPlayer2ToggleLabel) = kPlayer2ToggleName;
  At<const char*>(kLabelTable + 4 * kCameraToggleLabel) = kCameraToggleName;
  return result;
}

uint8_t* DefaultRecord(int index) { return reinterpret_cast<uint8_t*>(kDefaultRecords + kRecordSize * index); }

void ApplyArkhamDefaults() {
  for (int index : kDefaultMouseRecords) {
    uint8_t* record = DefaultRecord(index);
    Bind(record, kJump, kKind_Key, kDikSpace);
    Bind(record, kTag, kKind_Key, kDikE);
    Bind(record, kAttack, kKind_MouseButton, 0);
    Bind(record, kGrab, kKind_MouseButton, 1);
    Bind(record, kToggleUp, kKind_Key, kDik2);
  }
  for (int index : kDefaultKeyboardRecords) {
    Bind(DefaultRecord(index), kGrapple, kKind_Key, kDikF);
    Bind(DefaultRecord(index), kPlayer2Toggle, kKind_Key, kDikF2);
    Bind(DefaultRecord(index), kCameraToggle, kKind_Key, kDikF1);
    Bind(DefaultRecord(index), kStart, kKind_Key, kDikEscape);
  }
  for (int index : kDefaultPadRecords) {
    uint8_t* record = DefaultRecord(index);
    const uint8_t* bumper = record + kRecordButtons + 2 * kToggleUp;
    Bind(record, kGrapple, bumper[0], bumper[1]);
  }
}

int __cdecl NotAMouseSlotForRows(int) { return 0; }
int __stdcall NoMouseSteer(int) { return 0; }

int __cdecl HookInstallDefaults(void* a1, void* a2, void* a3, void* a4, void* a5) {
  int result = reinterpret_cast<InstallDefaultsFn>(kInstallDefaults)(a1, a2, a3, a4, a5);
  ApplyArkhamDefaults();
  return result;
}

}  // namespace

void Install() {
  ApplyArkhamDefaults();  // and again once the game has rewritten the keyboard ones
  hook::PatchCall(reinterpret_cast<void*>(kInstallDefaultsCallSite), reinterpret_cast<void*>(kInstallDefaults),
                  HookInstallDefaults);
  hook::PatchCall(reinterpret_cast<void*>(kLoadMappingsCallSite), reinterpret_cast<void*>(kLoadMappings),
                  HookLoadMappings);
  hook::PatchCall(reinterpret_cast<void*>(kMenuInitThunk), reinterpret_cast<void*>(kMenuInit), HookMenuInit);
  for (uintptr_t site : kIsMouseSlotCallSites)
    hook::PatchCall(reinterpret_cast<void*>(site), reinterpret_cast<void*>(kIsMouseSlot), NotAMouseSlotForRows);
  hook::PatchCall(reinterpret_cast<void*>(kMouseSteerCallSite), reinterpret_cast<void*>(kMouseSteer), NoMouseSteer);
  for (int i = 0; i < 3; ++i) At<const char*>(kMouseButtonNameCells[i]) = kMouseButtonNames[i];
}

}  // namespace keybinds
