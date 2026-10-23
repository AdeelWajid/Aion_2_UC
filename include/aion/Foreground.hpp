// Foreground.hpp — Win32 foreground hand-off shared by the menu and auto-dodge test.
#pragma once
#include <windows.h>

namespace aion {

bool RaiseWindowForeground(HWND target, bool release_pointer, int* cursor_calls);
// Focus the game for SendInput while this overlay process is often foreground (menu open).
bool FocusGameForKeyboard(HWND game);

}  // namespace aion
