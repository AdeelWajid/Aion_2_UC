// ImGuiLayer.hpp — the ImGui context, its style, and the focus hand-off.
//
// The menu is ImGui's only user, so everything ImGui-global lives here rather than in the menu:
// the context, the font atlas, the light flat style, the two backends, and the window state
// machine that decides whether the overlay is click-through (playing) or interactive (editing).
//
// Two things about this project make the integration different from a stock ImGui sample:
//
//  * The overlay presents a *premultiplied-alpha* layered window (see Overlay.hpp). ImGui's DX11
//    backend already writes alpha the way that needs (its blend state uses SrcBlendAlpha = ONE,
//    verified in the vendored copy), so nothing is patched and the vendored tree stays stock - which
//    is what keeps re-vendoring a straight file replacement.
//
//  * Nothing else in the overlay takes focus, ever: the window is WS_EX_NOACTIVATE and
//    click-through, and all keyboard input is polled with GetAsyncKeyState. Editing widgets needs
//    the opposite, so SetInteractive() is the single place that flips the window between the two.
//
// A failed Init() leaves the layer inactive rather than fatal: the ESP is the product, the menu is
// a convenience, and a machine without a usable D3D11 device should still play.
#pragma once
#include <string>

namespace aion {

class D3D11Overlay;

class ImGuiLayer {
public:
    ImGuiLayer();
    ~ImGuiLayer();

    // Creates the context, installs the DX11 backend against the overlay's device/context and the
    // Win32 backend against `hwnd`. Safe to call once.
    bool Init(void* hwnd, D3D11Overlay& gfx);
    void Shutdown();
    bool Active() const { return active_; }

    // One ImGui frame. Call once per host frame, but only while interactive: a closed menu costs
    // nothing, not even the ImGui bookkeeping.
    void BeginFrame();
    // ImGui::Render + ImGui_ImplDX11_RenderDrawData. Runs with the overlay's render target still
    // bound, i.e. between D3D11Overlay::Flush() and Present() — the overlay calls it through its
    // pre-present hook, so both passes land in the same surface.
    void RenderDrawData();

    // The whole click-through / focus policy.
    //   on  = editing: drops WS_EX_TRANSPARENT and WS_EX_NOACTIVATE, takes the foreground so ImGui
    //         receives real WM_KEYDOWN, and the OS cursor works because clicks now reach us.
    //   off = playing: styles restored, focus handed back to the game window, clicks pass through
    //         again. The game window is found by pid, so nothing has to be threaded through.
    void SetInteractive(bool on);
    bool Interactive() const { return interactive_; }

    // Message-pump hook. Returns true when ImGui consumed the message (only while interactive, and
    // never for WM_NCHITTEST, which the overlay answers itself).
    bool WndProc(void* hwnd, unsigned msg, unsigned long long wparam, long long lparam);

    // Dark charcoal theme, soft corners, cool blue accent aligned with the HUD/session panel.
    // Called by Init and after a scale change.
    void ApplyStyle();
    // While the menu is open, take the foreground back if the client has grabbed it: a fullscreen
    // client reclaims the foreground within seconds of the menu opening, and an unfocused menu
    // receives no keys while its first click only activates it.
    void ReclaimForeground();
    // While the menu is open, skip ReclaimForeground until this time (e.g. auto-dodge test input).
    void DeferForegroundReclaim(int ms);
    // Re-rasterizes the atlas. `face` is a family name ("Segoe UI"), `px` the pixel size at
    // scale 1. Non-Latin labels need more than the default range, so the builder merges a CJK
    // face when the system has one and stays Latin-only when it does not.
    void RebuildFonts(const std::string& face, int px);

    // Monitor DPI scale: the menu is meant to be readable at 1:1 and sane on a 4K panel.
    void SetDisplayScale(float scale);
    float display_scale() const;

    // Which window focus is handed back to on close (null = none resolved yet: focus is just
    // dropped). The host already resolves the client window every second for positioning, so it
    // passes the handle in rather than this layer enumerating windows a second time.
    void set_game_window(void* hwnd) { game_hwnd_ = hwnd; }

    // Last failure, for the log. Empty when everything worked.
    const std::string& last_error() const { return error_; }

    // The ImGui version this build was linked against (logged at startup with the font settings).
    std::string version() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool active_ = false;
    bool interactive_ = false;
    void* game_hwnd_ = nullptr;
    std::string error_;
};

}  // namespace aion
