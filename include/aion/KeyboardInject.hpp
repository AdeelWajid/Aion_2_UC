// KeyboardInject.hpp — dodge key injection (SendInput + optional virtual HID drivers).
#pragma once
#include <windows.h>

namespace aion {

// Config::auto_dodge_input — 0=auto, 1=SendInput, 2=Interception, 3=FakerInput.
enum class DodgeInputMethod { Auto = 0, SendInput = 1, Interception = 2, FakerInput = 3 };

enum class DodgeInputBackend { None, SendInput, Interception, FakerInput };

const char* DodgeInputBackendName(DodgeInputBackend b);
DodgeInputBackend LastDodgeInputBackend();

bool InterceptionDllPresent();
bool InterceptionDriverAvailable();
bool FakerInputDllPresent();
bool FakerInputDriverAvailable();

void TapDodgeCombo(int dodge_vk, int move_vk, DodgeInputMethod method, HWND game_hwnd,
                   int attempts = 1);

}  // namespace aion
