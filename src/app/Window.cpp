// src/app/Window.cpp — see Window.hpp.
#include "Window.hpp"

#include <windows.h>

#include <atomic>
#include <string>

#include "aion/ImGuiLayer.hpp"   // ImGuiLayer (the WndProc forwards to it)
#include "aion/Logger.hpp"      // AION_ERROR
#include "aion/Obf.hpp"         // OverlayWindowClass, OverlayWindowTitle

namespace aion {
namespace app {

HWND FindGameWindow(std::uint32_t pid) {
    struct Ctx { std::uint32_t pid; HWND hwnd; } ctx{pid, nullptr};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            DWORD wp = 0;
            GetWindowThreadProcessId(h, &wp);
            if (wp != c->pid || !IsWindowVisible(h)) return TRUE;
            RECT r{};
            if (GetClientRect(h, &r) && (r.right - r.left) > 200 && (r.bottom - r.top) > 200) {
                c->hwnd = h;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.hwnd;
}

// Screen-space top-left + size of the game's client area.
bool GameClientRect(HWND game, RECT& out) {
    if (!game || !IsWindow(game)) return false;
    RECT rc{};
    if (!GetClientRect(game, &rc)) return false;
    POINT tl{0, 0};
    if (!ClientToScreen(game, &tl)) return false;
    out.left = tl.x; out.top = tl.y;
    out.right = tl.x + (rc.right - rc.left);
    out.bottom = tl.y + (rc.bottom - rc.top);
    return true;
}

// Adopt the client's own DPI model, before this process has a window of its own.
//
// A DPI-unaware process is handed virtualised coordinates: on a 4K panel at 150% scaling the
// client's real 3840x2160 client rect comes back to us as 2560x1440, so the overlay would
// rasterize at two thirds of the resolution and let DWM stretch it - soft text, soft hairlines,
// and the whole present paid for pixels nobody sees. Matching the *client's* model, rather than
// simply demanding per-monitor-v2, is the part that keeps alignment: an aware overlay over an
// unaware client (whose own frame is itself DWM-stretched) would place every box off by the
// scaling factor.
//
// Set once per process, and only while it has no windows of its own - which is why this runs
// before CreateWindowEx, and reports what it got instead of assuming it worked. A failure leaves
// the default in place, i.e. the unaware path the overlay has always run on.
void AdoptClientDpiModel(HWND game) {
    if (!game) return;
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (!user32) return;
    // Resolved at run time: four pseudo-handle constants are not worth a hard dependency on a
    // Windows 10 1607 SDK for a build that otherwise compiles against whatever is installed.
    using GetWindowDpiAwarenessContextFn = HANDLE(WINAPI*)(HWND);
    using GetAwarenessFromDpiAwarenessContextFn = int(WINAPI*)(HANDLE);
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
    using SetProcessDPIAwareFn = BOOL(WINAPI*)();
    auto get_ctx = reinterpret_cast<GetWindowDpiAwarenessContextFn>(
        reinterpret_cast<void*>(GetProcAddress(user32, "GetWindowDpiAwarenessContext")));
    auto get_awareness = reinterpret_cast<GetAwarenessFromDpiAwarenessContextFn>(
        reinterpret_cast<void*>(GetProcAddress(user32, "GetAwarenessFromDpiAwarenessContext")));
    auto set_ctx = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
        reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
    static const char* kNames[] = {"unaware", "system-aware", "per-monitor"};
    // DPI_AWARENESS_UNAWARE / SYSTEM_AWARE / PER_MONITOR_AWARE. A v1 per-monitor client is
    // reported as PER_MONITOR_AWARE like a v2 one; the difference is non-client scaling, which a
    // borderless popup does not have.
    int want = -1;
    if (get_ctx && get_awareness) {
        const HANDLE ctx = get_ctx(game);
        if (ctx) {
            const int aw = get_awareness(ctx);
            if (aw >= 0 && aw <= 2) want = aw;
        }
    }
    if (want < 0) {
        AION_INFO("dpi: client awareness unreadable; keeping this process's default");
        return;
    }
    const INT_PTR value = want == 0 ? -1 : (want == 1 ? -2 : -4);
    bool ok = false;
    if (set_ctx) {
        ok = set_ctx(reinterpret_cast<HANDLE>(value)) != FALSE;
    } else {
        // Before the per-monitor API existed, system-aware is the closest match for an aware
        // client. An unaware client is left alone: that is this process's default anyway.
        auto set_old = reinterpret_cast<SetProcessDPIAwareFn>(
            reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDPIAware")));
        ok = want >= 1 && set_old && set_old() != FALSE;
    }
    AION_INFO("dpi: client=%s, this process=%s", kNames[want], ok ? kNames[want] : "unchanged");
}

#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif

static std::atomic<bool> g_quit{false};
// Config -> overlay font settings (glyphs are rasterized at exactly these sizes). client_h is the
// height of the surface the labels are drawn into, which is what the auto label size follows.
FontConfig FontsFromConfig(const Config& c, int client_h) {
    FontConfig f;
    f.face = c.font_face;
    f.esp_px = EspFontPx(c, client_h);
    f.menu_px = c.menu_font_px;
    f.bold = c.font_bold;
    f.shadow = c.text_shadow;
    return f;
}

// The WndProc is a free function, so the layer is reached through a file-scope pointer. Set once
// the layer initialises, cleared on shutdown.
static aion::ImGuiLayer* g_imgui = nullptr;

LRESULT CALLBACK OverlayWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { g_quit = true; return 0; }
    // While the menu is open it sees every message first: text fields and a clickable pointer only
    // work through real window messages, which the old keyboard menu never needed.
    if (g_imgui && g_imgui->WndProc(h, m, (unsigned long long)w, (long long)l)) return 1;
    // Click-through. WS_EX_TRANSPARENT governs painting order, not hit-testing: a
    // window without WS_EX_LAYERED is still the mouse target even when it is
    // transparent and topmost, so every click went to us and the game (an action MMO
    // that attacks on left/right click) received nothing - the cursor was released
    // and reappeared mid-screen. HTTRANSPARENT takes the window out of the hit-test
    // walk, which passes the click to whatever is underneath.
    //
    // Editing needs the opposite answer: the widgets have to be hit-testable, so while the menu is
    // open the window answers normally and takes the clicks itself.
    if (m == WM_NCHITTEST) {
        if (g_imgui && g_imgui->Interactive()) return DefWindowProcA(h, m, w, l);
        return HTTRANSPARENT;
    }
    return DefWindowProcA(h, m, w, l);
}


// The window messages are the only writer of the quit flag and the only reader of the ImGui layer
// pointer, and both stay private to this translation unit: the header exports the two entry points
// the rest of the program needs and no mutable global.
bool ShouldQuit() { return g_quit.load(); }
void SetImGuiLayer(ImGuiLayer* layer) { g_imgui = layer; }

// Whether another instance of this tool is running. Its window class is fixed for the build
// (Obf.hpp), so a window scan is enough and no shared file or mutex is needed. The window belongs to
// whatever process created it, so a hit is always a live process.
bool OverlayInstanceRunning() {
    const std::string cls = OverlayWindowClass();
    return FindWindowA(cls.c_str(), nullptr) != nullptr;
}

// Registers the class and creates the window. Moved verbatim from the block that used to sit in
// aion::RunOverlay; the failure return became a null HWND.
HWND CreateOverlayWindow(const RECT* client_rc, bool exclusive) {
    // A second instance is not a second view of the world: each reads the client on its own
    // schedule, so the two draws coincide while the camera is still and separate by the difference
    // in their read times the moment it moves - one box becomes two lines. That is the whole of the
    // "doubled box while the camera moves" artefact, and refusing the second instance is the only
    // fix that does not have to guess, at draw time, which of the two is the overlay the user means.
    // A headless mode is exempt: it draws its own surface and exits, and it is usually being run
    // precisely because the interactive session is up.
    if (exclusive)
        if (const HWND other = FindWindowA(OverlayWindowClass().c_str(), nullptr)) {
            DWORD other_pid = 0;
            GetWindowThreadProcessId(other, &other_pid);
            AION_ERROR("refusing to start: another overlay is already running (pid %lu, window "
                       "0x%llX). Two overlays draw every box twice as soon as the camera moves; close "
                       "the running one first (Task Manager -> Details -> PID %lu), or it will keep "
                       "holding the screen.",
                       (unsigned long)other_pid, (unsigned long long)(std::uintptr_t)other,
                       (unsigned long)other_pid);
            return nullptr;
        }
    WNDCLASSA wc{};
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    // The class and title come from Obf.hpp: a window-class scan is the cheapest way to find
    // this tool from another process, so the names are not literals in the image.
    const std::string wnd_class = OverlayWindowClass();
    const std::string wnd_title = OverlayWindowTitle();
    wc.lpszClassName = wnd_class.c_str();
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    // WS_EX_NOREDIRECTIONBITMAP: no redirection surface, so DWM composites our
    // DirectComposition visual directly.
    // WS_EX_LAYERED | WS_EX_TRANSPARENT: this pair is what makes the window
    // click-through. WS_EX_TRANSPARENT alone only affects painting order - a
    // non-layered window is still the mouse target, so every click was swallowed and
    // the game (which attacks on left/right click) received none. WM_NCHITTEST
    // returning HTTRANSPARENT does not help either: it only forwards to windows in the
    // same thread, and the game is another process. Verified with a click injected
    // through the overlay at a decoy window underneath.
    HWND overlay = CreateWindowExA(WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP |
                                       WS_EX_TRANSPARENT | WS_EX_TOPMOST |
                                       WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                   wnd_class.c_str(), wnd_title.c_str(), WS_POPUP,
                                   0, 0, 1280, 720, nullptr, nullptr, wc.hInstance, nullptr);
    if (!overlay) return nullptr;
    ShowWindow(overlay, SW_SHOWNOACTIVATE);
    // Place and size it on the client before the device exists: at 4K the alternative was to build
    // a 1280x720 target (and an atlas rasterized for it) and replace both on the first frame.
    if (client_rc)
        SetWindowPos(overlay, HWND_TOPMOST, client_rc->left, client_rc->top,
                     client_rc->right - client_rc->left, client_rc->bottom - client_rc->top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return overlay;
}

}  // namespace app
}  // namespace aion
