#pragma once

#include <windows.h>

#include <initializer_list>

namespace hook {

// Overwrites code only if it still holds the expected bytes, so other game builds are left alone.
bool PatchBytes(void* address, std::initializer_list<BYTE> expected, std::initializer_list<BYTE> replacement);

// Retargets an E8 rel32 call (or an E9 jump, as in a thunk), only if it still targets `expected`.
bool PatchCall(void* site, void* expected, void* detour);

// Redirects one of the module's imports. Returns the original, or nullptr if it isn't imported by name.
void* PatchImport(HMODULE module, const char* dll, const char* func, void* detour);

// Swaps a COM vtable slot, so every object of that class is affected.
void* PatchVtable(void* object, int index, void* detour);

template <class Fn>
void Import(HMODULE module, const char* dll, const char* func, Fn detour, Fn& original) {
  original = reinterpret_cast<Fn>(PatchImport(module, dll, func, reinterpret_cast<void*>(detour)));
}

template <class Fn>
void Vtable(void* object, int index, Fn detour, Fn& original) {
  if (!original) original = reinterpret_cast<Fn>(PatchVtable(object, index, reinterpret_cast<void*>(detour)));
}

}  // namespace hook
