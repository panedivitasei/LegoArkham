#pragma once

namespace devscan {

// Makes the game's controller rescans cheap. Windows broadcasts a device change on all
// sorts of events, and the game answers each one with three rescans that froze it for a
// second apiece: a WMI query per device plus a full DirectInput enumeration.
void Install();

// Hooks EnumDevices on the IDirectInput8 the game just created.
void Attach(void* directInput8);

}  // namespace devscan
