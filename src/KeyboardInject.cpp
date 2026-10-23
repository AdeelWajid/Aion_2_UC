// KeyboardInject.cpp — see KeyboardInject.hpp.
#include "aion/KeyboardInject.hpp"

#include "aion/AutoDodge.hpp"
#include "aion/Logger.hpp"

#include <functional>
#include <thread>

namespace aion {
namespace {

DodgeInputBackend g_last_backend = DodgeInputBackend::None;

// --- Interception (dynamic load; driver must be installed separately) -----------------
using InterceptionContext = void*;
using InterceptionDevice = int;

struct InterceptionKeyStroke {
    unsigned short code;
    unsigned short state;
};

constexpr unsigned short IC_KEY_DOWN = 0x00;
constexpr unsigned short IC_KEY_UP = 0x01;
constexpr unsigned short IC_KEY_E0 = 0x02;

#define IC_KEYBOARD(index) ((InterceptionDevice)((index) + 1))

using IcCreateContextFn = InterceptionContext (*)();
using IcDestroyContextFn = void (*)(InterceptionContext);
using IcSendFn = int (*)(InterceptionContext, InterceptionDevice, const InterceptionKeyStroke*,
                           unsigned int);

struct InterceptionApi {
    HMODULE mod = nullptr;
    IcCreateContextFn create = nullptr;
    IcDestroyContextFn destroy = nullptr;
    IcSendFn send = nullptr;

    bool EnsureLoaded() {
        if (mod) return create && send;
        wchar_t exe[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
            wchar_t* slash = exe;
            for (wchar_t* p = exe; *p; ++p)
                if (*p == L'\\' || *p == L'/') slash = p;
            if (slash > exe) *(slash + 1) = L'\0';
            wchar_t dll_path[MAX_PATH]{};
            wsprintfW(dll_path, L"%sinterception.dll", exe);
            mod = LoadLibraryW(dll_path);
        }
        if (!mod) mod = LoadLibraryW(L"interception.dll");
        if (!mod) return false;
        create = (IcCreateContextFn)(void*)GetProcAddress(mod, "interception_create_context");
        destroy = (IcDestroyContextFn)(void*)GetProcAddress(mod, "interception_destroy_context");
        send = (IcSendFn)(void*)GetProcAddress(mod, "interception_send");
        return create && send;
    }
};

InterceptionApi& IcApi() {
    static InterceptionApi api;
    return api;
}

unsigned short VkScan(WORD vk) {
    vk = (WORD)NormalizeDodgeVk(vk);
    if (vk == VK_LSHIFT) return 0x2A;
    if (vk == VK_RSHIFT) return 0x36;
    return (unsigned short)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
}

bool ScanExtended(WORD vk) {
    vk = (WORD)NormalizeDodgeVk(vk);
    switch (vk) {
        case VK_RMENU:
        case VK_RCONTROL:
        case VK_RSHIFT:
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_DIVIDE:
            return true;
        default:
            return false;
    }
}

bool IcSendStroke(InterceptionContext ctx, const InterceptionKeyStroke& k) {
    for (int i = 0; i < 10; ++i) {
        if (IcApi().send(ctx, IC_KEYBOARD(i), &k, 1) > 0) return true;
    }
    return false;
}

bool IcKey(InterceptionContext ctx, unsigned short scan, bool key_up, bool e0) {
    InterceptionKeyStroke k{};
    k.code = scan;
    k.state = key_up ? IC_KEY_UP : IC_KEY_DOWN;
    if (e0) k.state = (unsigned short)(k.state | IC_KEY_E0);
    return IcSendStroke(ctx, k);
}

void WithGameInputQueue(HWND game, const std::function<void()>& fn) {
    if (!game || !IsWindow(game)) {
        fn();
        return;
    }
    const DWORD game_tid = GetWindowThreadProcessId(game, nullptr);
    const DWORD our_tid = GetCurrentThreadId();
    const bool attached =
        game_tid && game_tid != our_tid && AttachThreadInput(our_tid, game_tid, TRUE);
    fn();
    if (attached) AttachThreadInput(our_tid, game_tid, FALSE);
}

void SendInputKey(WORD vk, bool key_up) {
    vk = (WORD)NormalizeDodgeVk(vk);
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = VkScan(vk);
    if (key_up) in.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));
}

bool TapInterception(int dodge_vk, int move_vk) {
    if (!IcApi().EnsureLoaded()) return false;
    InterceptionContext ctx = IcApi().create();
    if (!ctx) {
        AION_WARN("Interception: interception_create_context failed (run install-interception "
                  "as admin, reboot)");
        return false;
    }

    const WORD dodge = (WORD)NormalizeDodgeVk(dodge_vk);
    const bool move = move_vk > 0 && move_vk <= 0xFE;
    int ok = 0;
    auto ic_down = [&](WORD vk) {
        const unsigned short sc = VkScan(vk);
        if (IcKey(ctx, sc, false, ScanExtended(vk))) ++ok;
    };
    auto ic_up = [&](WORD vk) {
        const unsigned short sc = VkScan(vk);
        if (IcKey(ctx, sc, true, ScanExtended(vk))) ++ok;
    };
    if (move) ic_down((WORD)move_vk);
    if (move) std::this_thread::sleep_for(std::chrono::milliseconds(55));
    ic_down(dodge);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ic_up(dodge);
    if (move) ic_up((WORD)move_vk);

    if (IcApi().destroy) IcApi().destroy(ctx);
    if (ok == 0) {
        AION_WARN("Interception: interception_send returned 0 on all keyboard devices");
        return false;
    }
    return true;
}

void TapSendInput(int dodge_vk, int move_vk, HWND game_hwnd) {
    const WORD dodge = (WORD)NormalizeDodgeVk(dodge_vk);
    const bool move = move_vk > 0 && move_vk <= 0xFE;

    WithGameInputQueue(game_hwnd, [&] {
        if (move) SendInputKey((WORD)move_vk, false);
        if (move) std::this_thread::sleep_for(std::chrono::milliseconds(55));
        SendInputKey(dodge, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        SendInputKey(dodge, true);
        if (move) SendInputKey((WORD)move_vk, true);
    });
}

// --- FakerInput virtual HID (DS4Windows driver; works on current Windows) ------------
constexpr BYTE KBD_LSHIFT_BIT = 2;

using FkClient = void*;
using FkAllocFn = FkClient (*)();
using FkFreeFn = void (*)(FkClient);
using FkConnectFn = bool (*)(FkClient);
using FkDisconnectFn = void (*)(FkClient);
using FkKeyboardFn = bool (*)(FkClient, BYTE, BYTE[6]);

struct FakerApi {
    HMODULE mod = nullptr;
    FkAllocFn alloc = nullptr;
    FkFreeFn free = nullptr;
    FkConnectFn connect = nullptr;
    FkDisconnectFn disconnect = nullptr;
    FkKeyboardFn update_keyboard = nullptr;
    FkClient client = nullptr;
    bool connected = false;

    bool EnsureLoaded() {
        if (mod) return alloc && connect && update_keyboard;
        wchar_t exe[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
            wchar_t* slash = exe;
            for (wchar_t* p = exe; *p; ++p)
                if (*p == L'\\' || *p == L'/') slash = p;
            if (slash > exe) *(slash + 1) = L'\0';
            const wchar_t* names[] = {L"fakerinput.dll", L"FakerInputDll.dll"};
            for (const wchar_t* name : names) {
                wchar_t path[MAX_PATH]{};
                wsprintfW(path, L"%s%s", exe, name);
                mod = LoadLibraryW(path);
                if (mod) break;
            }
        }
        if (!mod) mod = LoadLibraryW(L"fakerinput.dll");
        if (!mod) mod = LoadLibraryW(L"FakerInputDll.dll");
        if (!mod) return false;
        alloc = (FkAllocFn)(void*)GetProcAddress(mod, "fakerinput_alloc");
        free = (FkFreeFn)(void*)GetProcAddress(mod, "fakerinput_free");
        connect = (FkConnectFn)(void*)GetProcAddress(mod, "fakerinput_connect");
        disconnect = (FkDisconnectFn)(void*)GetProcAddress(mod, "fakerinput_disconnect");
        update_keyboard =
            (FkKeyboardFn)(void*)GetProcAddress(mod, "fakerinput_update_keyboard");
        return alloc && connect && update_keyboard;
    }

    bool EnsureConnected() {
        if (!EnsureLoaded()) return false;
        if (!client) client = alloc();
        if (!client) return false;
        if (!connected) connected = connect(client);
        return connected;
    }

    void ResetConnection() {
        if (client && connected && disconnect) disconnect(client);
        connected = false;
    }
};

FakerApi& FkApi() {
    static FakerApi api;
    return api;
}

// USB HID usage IDs (same as FakerInput / DS4Windows).
BYTE VkToHidUsage(WORD vk) {
    vk = (WORD)NormalizeDodgeVk(vk);
    switch (vk) {
        case 'A':
            return 0x04;
        case 'B':
            return 0x05;
        case 'C':
            return 0x06;
        case 'D':
            return 0x07;
        case 'E':
            return 0x08;
        case 'F':
            return 0x09;
        case 'G':
            return 0x0A;
        case 'H':
            return 0x0B;
        case 'I':
            return 0x0C;
        case 'J':
            return 0x0D;
        case 'K':
            return 0x0E;
        case 'L':
            return 0x0F;
        case 'M':
            return 0x10;
        case 'N':
            return 0x11;
        case 'O':
            return 0x12;
        case 'P':
            return 0x13;
        case 'Q':
            return 0x14;
        case 'R':
            return 0x15;
        case 'S':
            return 0x16;
        case 'T':
            return 0x17;
        case 'U':
            return 0x18;
        case 'V':
            return 0x19;
        case 'W':
            return 0x1A;
        case 'X':
            return 0x1B;
        case 'Y':
            return 0x1C;
        case 'Z':
            return 0x1D;
        case VK_SPACE:
            return 0x2C;
        case '1':
            return 0x1E;
        case '2':
            return 0x1F;
        case '3':
            return 0x20;
        case '4':
            return 0x21;
        case '5':
            return 0x22;
        case '6':
            return 0x23;
        case '7':
            return 0x24;
        case '8':
            return 0x25;
        case '9':
            return 0x26;
        case '0':
            return 0x27;
        default:
            return 0;
    }
}

BYTE ShiftFlagsForDodgeVk(WORD dodge_vk) {
    dodge_vk = (WORD)NormalizeDodgeVk(dodge_vk);
    if (dodge_vk == VK_LSHIFT) return KBD_LSHIFT_BIT;
    if (dodge_vk == VK_RSHIFT) return 32;
    if (dodge_vk == VK_LCONTROL) return 1;
    if (dodge_vk == VK_LMENU) return 4;
    return 0;
}

bool TapFakerInput(int dodge_vk, int move_vk) {
    if (!FkApi().EnsureConnected()) {
        AION_WARN("FakerInput: connect failed (install FakerInput driver — see CONFIG hint)");
        return false;
    }
    FakerApi& fk = FkApi();
    BYTE keys[6] = {};
    BYTE shift = ShiftFlagsForDodgeVk((WORD)dodge_vk);
    const WORD dodge = (WORD)NormalizeDodgeVk(dodge_vk);
    const bool move = move_vk > 0 && move_vk <= 0xFE;

    if (move) keys[0] = VkToHidUsage((WORD)move_vk);
    if (!shift && dodge != VK_LSHIFT && dodge != VK_RSHIFT) {
        if (BYTE hid = VkToHidUsage(dodge)) keys[0] = hid;
    }

    BYTE clear[6] = {};
    fk.update_keyboard(fk.client, 0, clear);
    if (move && !keys[0]) {
        AION_WARN("FakerInput: move VK %d has no HID mapping", move_vk);
        return false;
    }
    if (!fk.update_keyboard(fk.client, shift, keys)) {
        fk.ResetConnection();
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    fk.update_keyboard(fk.client, 0, clear);
    return true;
}

}  // namespace

const char* DodgeInputBackendName(DodgeInputBackend b) {
    switch (b) {
        case DodgeInputBackend::SendInput:
            return "SendInput";
        case DodgeInputBackend::Interception:
            return "Interception";
        case DodgeInputBackend::FakerInput:
            return "FakerInput";
        default:
            return "none";
    }
}

DodgeInputBackend LastDodgeInputBackend() { return g_last_backend; }

bool InterceptionDllPresent() { return IcApi().EnsureLoaded(); }

bool InterceptionDriverAvailable() {
    if (!IcApi().EnsureLoaded()) return false;
    InterceptionContext ctx = IcApi().create();
    if (!ctx) return false;
    if (IcApi().destroy) IcApi().destroy(ctx);
    return true;
}

bool FakerInputDllPresent() { return FkApi().EnsureLoaded(); }

bool FakerInputDriverAvailable() {
    if (!FkApi().EnsureLoaded()) return false;
    FkClient probe = FkApi().alloc();
    if (!probe) return false;
    const bool ok = FkApi().connect(probe);
    if (ok && FkApi().disconnect) FkApi().disconnect(probe);
    if (FkApi().free) FkApi().free(probe);
    return ok;
}

bool TryBackend(int dodge_vk, int move_vk, DodgeInputMethod method, HWND game_hwnd,
                bool faker_ok, bool ic_ok) {
    if (method == DodgeInputMethod::FakerInput ||
        (method == DodgeInputMethod::Auto && faker_ok)) {
        if (TapFakerInput(dodge_vk, move_vk)) {
            g_last_backend = DodgeInputBackend::FakerInput;
            return true;
        }
        if (method == DodgeInputMethod::FakerInput) return false;
    }
    if (method == DodgeInputMethod::Interception ||
        (method == DodgeInputMethod::Auto && ic_ok)) {
        if (TapInterception(dodge_vk, move_vk)) {
            g_last_backend = DodgeInputBackend::Interception;
            return true;
        }
        if (method == DodgeInputMethod::Interception) return false;
    }
    TapSendInput(dodge_vk, move_vk, game_hwnd);
    g_last_backend = DodgeInputBackend::SendInput;
    return true;
}

void TapDodgeCombo(int dodge_vk, int move_vk, DodgeInputMethod method, HWND game_hwnd,
                   int attempts) {
    if (attempts < 1) attempts = 1;
    g_last_backend = DodgeInputBackend::None;
    const bool faker_ok = FakerInputDriverAvailable();
    const bool ic_ok = InterceptionDriverAvailable();

    for (int i = 0; i < attempts; ++i) {
        if (i > 0) std::this_thread::sleep_for(std::chrono::milliseconds(220));
        TryBackend(dodge_vk, move_vk, method, game_hwnd, faker_ok, ic_ok);
    }
}

}  // namespace aion
