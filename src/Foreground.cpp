// Foreground.cpp — see Foreground.hpp.
#include "aion/Foreground.hpp"

namespace aion {
namespace {

void Raise(HWND target) {
    BringWindowToTop(target);
    SetForegroundWindow(target);
    SetActiveWindow(target);
    SetFocus(target);
}

}  // namespace

bool RaiseWindowForeground(HWND target, bool release_pointer, int* cursor_calls) {
    if (!target) return false;
    HWND fg = GetForegroundWindow();
    const DWORD fg_thread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD our_thread = GetCurrentThreadId();
    const bool attached =
        fg_thread && fg_thread != our_thread && AttachThreadInput(our_thread, fg_thread, TRUE);
    if (release_pointer) {
        ClipCursor(nullptr);
        int calls = 0;
        while (calls < 16 && ShowCursor(TRUE) < 0) ++calls;
        if (cursor_calls) *cursor_calls = calls;
    }
    Raise(target);
    if (GetForegroundWindow() != target) {
        keybd_event(VK_MENU, 0, KEYEVENTF_EXTENDEDKEY, 0);
        keybd_event(VK_MENU, 0, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, 0);
        Raise(target);
    }
    if (attached) AttachThreadInput(our_thread, fg_thread, FALSE);
    return GetForegroundWindow() == target;
}

bool FocusGameForKeyboard(HWND game) {
    if (!game || !IsWindow(game)) return false;
    if (GetForegroundWindow() == game) return true;

    DWORD game_pid = 0;
    GetWindowThreadProcessId(game, &game_pid);
    HWND fg = GetForegroundWindow();
    DWORD fg_pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &fg_pid);
    if (fg_pid == GetCurrentProcessId() && game_pid) AllowSetForegroundWindow(game_pid);

    const DWORD game_thread = GetWindowThreadProcessId(game, nullptr);
    const DWORD our_thread = GetCurrentThreadId();
    const DWORD fg_thread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const bool att_fg =
        fg_thread && fg_thread != our_thread && AttachThreadInput(our_thread, fg_thread, TRUE);
    const bool att_game =
        game_thread && game_thread != our_thread && AttachThreadInput(our_thread, game_thread, TRUE);

    RaiseWindowForeground(game, false, nullptr);
    if (GetForegroundWindow() != game) {
        keybd_event(VK_MENU, 0, KEYEVENTF_EXTENDEDKEY, 0);
        keybd_event(VK_MENU, 0, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, 0);
        RaiseWindowForeground(game, false, nullptr);
    }

    if (att_game) AttachThreadInput(our_thread, game_thread, FALSE);
    if (att_fg) AttachThreadInput(our_thread, fg_thread, FALSE);
    return GetForegroundWindow() == game;
}

}  // namespace aion
