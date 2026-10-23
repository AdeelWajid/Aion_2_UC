// ImGuiLayer.cpp — see ImGuiLayer.hpp for why this class exists and what it owns.
#include "aion/ImGuiLayer.hpp"

#include <windows.h>

#include <d3d11.h>

#include <algorithm>
#include <string>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include "aion/Foreground.hpp"
#include "aion/Logger.hpp"
#include "aion/Overlay.hpp"

// Declared by the Win32 backend's header only as IMGUI_IMPL_API; the definition lives in the
// backend .cpp we compiled, so declaring it here is all that is needed to route messages.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace aion {
namespace {

// Per-monitor DPI without a hard dependency on the Windows 10 SDK headers: resolved at runtime so
// the binary still runs on a build where the import does not exist.
using GetDpiForWindowFn = UINT(WINAPI*)(HWND);

float DetectScale(HWND hwnd) {
    if (HMODULE user32 = GetModuleHandleA("user32.dll")) {
        auto dpi_for_window = (GetDpiForWindowFn)GetProcAddress(user32, "GetDpiForWindow");
        if (dpi_for_window) {
            const UINT dpi = dpi_for_window(hwnd);
            if (dpi >= 96 && dpi <= 480) return (float)dpi / 96.f;
        }
    }
    return 1.f;
}

// Family name -> file. Keeping the config readable ("Segoe UI") matters more than covering every
// installed font; an unknown family falls back to ImGui's built-in bitmap font.
const char* FontFileFor(const std::string& face) {
    struct Entry {
        const char* family;
        const char* file;
    };
    static const Entry kFonts[] = {
        {"Segoe UI", "C:\\Windows\\Fonts\\segoeui.ttf"},
        {"Segoe UI Semibold", "C:\\Windows\\Fonts\\seguisb.ttf"},
        {"Tahoma", "C:\\Windows\\Fonts\\tahoma.ttf"},
        {"Verdana", "C:\\Windows\\Fonts\\verdana.ttf"},
        {"Arial", "C:\\Windows\\Fonts\\arial.ttf"},
        {"Consolas", "C:\\Windows\\Fonts\\consola.ttf"},
    };
    for (const Entry& e : kFonts)
        if (face == e.family) return e.file;
    return nullptr;
}

// A face with CJK coverage to merge into the atlas, so a label typed in Korean or Chinese is not
// rendered as boxes. Latin-only when the system has neither.
struct CjkFace {
    const char* file;
    const ImWchar* (*ranges)(ImFontAtlas*);
};

const CjkFace* PickCjkFace() {
    static const CjkFace kFaces[] = {
        {"C:\\Windows\\Fonts\\malgun.ttf", [](ImFontAtlas* a) { return a->GetGlyphRangesKorean(); }},
        {"C:\\Windows\\Fonts\\msyh.ttc",
         [](ImFontAtlas* a) { return a->GetGlyphRangesChineseSimplifiedCommon(); }},
    };
    for (const CjkFace& f : kFaces)
        if (GetFileAttributesA(f.file) != INVALID_FILE_ATTRIBUTES) return &f;
    return nullptr;
}

bool FileExists(const char* path) {
    return path && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

// Union of every clip rectangle ImGui recorded for this frame, in window pixels: that is exactly the
// area it will touch. A command with a user callback draws its own geometry and may reset its clip
// rect, so it is skipped rather than claiming the whole surface.
void MarkDrawnRegion(D3D11Overlay& gfx, const ImDrawData& data) {
    bool any = false;
    float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
    for (int n = 0; n < data.CmdListsCount; ++n) {
        for (const ImDrawCmd& cmd : data.CmdLists[n]->CmdBuffer) {
            if (cmd.UserCallback) continue;
            const float cx0 = cmd.ClipRect.x - data.DisplayPos.x;
            const float cy0 = cmd.ClipRect.y - data.DisplayPos.y;
            const float cx1 = cmd.ClipRect.z - data.DisplayPos.x;
            const float cy1 = cmd.ClipRect.w - data.DisplayPos.y;
            if (cx1 <= cx0 || cy1 <= cy0) continue;
            if (!any) {
                x0 = cx0;
                y0 = cy0;
                x1 = cx1;
                y1 = cy1;
                any = true;
            } else {
                x0 = (std::min)(x0, cx0);
                y0 = (std::min)(y0, cy0);
                x1 = (std::max)(x1, cx1);
                y1 = (std::max)(y1, cy1);
            }
        }
    }
    if (any) gfx.MarkDirty(x0, y0, x1, y1);
}

}  // namespace

// ------------------------------------------------------------------------ impl --

struct ImGuiLayer::Impl {
    HWND hwnd = nullptr;
    D3D11Overlay* gfx = nullptr;
    LONG_PTR saved_ex_style = 0;
    bool style_saved = false;
    bool frame_begun = false;
    // ShowCursor calls made when the menu opened, so closing puts the count back exactly.
    int cursor_calls = 0;
    // When the foreground was last reclaimed, and whether that has been logged: the reclaim repeats
    // while the client keeps taking it back, so the log line must not.
    unsigned long long reclaim_ms = 0;
    unsigned long long defer_reclaim_until = 0;
    bool reclaim_logged = false;
    float scale = 1.f;
    std::string face = "Segoe UI";
    int px = 16;
};

ImGuiLayer::ImGuiLayer() : impl_(new Impl()) {}

ImGuiLayer::~ImGuiLayer() {
    Shutdown();
    delete impl_;
    impl_ = nullptr;
}

bool ImGuiLayer::Init(void* hwnd, D3D11Overlay& gfx) {
    auto* i = impl_;
    if (!i || active_) return active_;
    i->hwnd = (HWND)hwnd;
    i->gfx = &gfx;
    i->scale = DetectScale(i->hwnd);

    IMGUI_CHECKVERSION();
    if (!ImGui::CreateContext()) {
        error_ = "ImGui::CreateContext failed";
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    // The menu's layout is not worth a second settings file: nothing about it is preserved, and
    // imgui.ini appearing next to the exe would be one more thing to explain.
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    // Keyboard navigation is kept because the old menu was keyboard-only: INSERT, arrows and Enter
    // still drive every widget without a mouse.
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    ID3D11Device* device = (ID3D11Device*)gfx.d3d_device();
    ID3D11DeviceContext* context = (ID3D11DeviceContext*)gfx.d3d_context();
    if (!device || !context) {
        error_ = "overlay has no D3D11 device";
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplDX11_Init(device, context)) {
        error_ = "ImGui_ImplDX11_Init failed";
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplWin32_Init(i->hwnd)) {
        error_ = "ImGui_ImplWin32_Init failed";
        ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    active_ = true;
    ApplyStyle();
    RebuildFonts(i->face, i->px);
    // Both passes go into the same surface: the overlay flushes its own primitives, then calls
    // this, then presents.
    gfx.set_pre_present_hook([this] { RenderDrawData(); });
    return true;
}

void ImGuiLayer::Shutdown() {
    auto* i = impl_;
    if (!i) return;
    if (active_) {
        if (i->gfx) i->gfx->set_pre_present_hook(nullptr);
        if (interactive_) SetInteractive(false);
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    active_ = false;
    i->frame_begun = false;
}

void ImGuiLayer::BeginFrame() {
    auto* i = impl_;
    if (!i || !active_) return;
    ReclaimForeground();
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    i->frame_begun = true;
}

// A fullscreen client takes the foreground back within seconds of the menu opening - measured: the
// open-time hand-off succeeds, and by the time the user clicks, the client owns the foreground
// again. An unfocused menu receives no keys at all and its first click is spent activating it, which
// is why the X and the text fields looked broken while a globally polled INSERT kept working.
//
// Only the client is taken back from: if the foreground is some other application, the user switched
// to it on purpose and the menu must not fight that. Repeats are paced, because the client keeps
// taking it back and a reclaim every frame would be a focus war.
void ImGuiLayer::DeferForegroundReclaim(int ms) {
    auto* i = impl_;
    if (!i || ms <= 0) return;
    const unsigned long long until = GetTickCount64() + (unsigned long long)ms;
    if (until > i->defer_reclaim_until) i->defer_reclaim_until = until;
}

void ImGuiLayer::ReclaimForeground() {
    auto* i = impl_;
    if (!i || !active_ || !interactive_ || !i->hwnd) return;
    if (i->defer_reclaim_until && GetTickCount64() < i->defer_reclaim_until) return;
    HWND game = (HWND)game_hwnd_;
    if (!game) return;  // nothing to tell a deliberate switch apart from a steal
    HWND fg = GetForegroundWindow();
    if (!fg || fg == i->hwnd) return;

    // Compare by process, not by window: the client owns several top-level windows, and whichever of
    // them took the foreground it is still the client taking it. Any other application in front is
    // the user switching on purpose, which the menu must not fight.
    DWORD fg_pid = 0, game_pid = 0;
    GetWindowThreadProcessId(fg, &fg_pid);
    GetWindowThreadProcessId(game, &game_pid);
    if (!game_pid || fg_pid != game_pid) return;

    const unsigned long long now = GetTickCount64();
    // Reclaimed at ~10 Hz rather than every frame: SetForegroundWindow is cheap, but the unlock
    // path presses and releases ALT, and synthesising that 60 times a second would be its own bug.
    // 100 ms is short enough that a keystroke cannot land in the client between two attempts, which
    // is the point - while the menu is open the only window receiving input must be the menu.
    if (i->reclaim_ms && now - i->reclaim_ms < 100) return;
    i->reclaim_ms = now;
    if (RaiseWindowForeground(i->hwnd, false, nullptr) && !i->reclaim_logged) {
        i->reclaim_logged = true;
        AION_INFO("menu: the client had taken the foreground back; reclaimed it");
    }
}

void ImGuiLayer::RenderDrawData() {
    auto* i = impl_;
    if (!i || !active_ || !i->frame_begun) return;
    ImGui::Render();
    if (ImDrawData* data = ImGui::GetDrawData()) {
        // Claim the area about to be drawn, before it is drawn: the overlay presents the union of
        // this frame's dirty regions and the ESP has already marked its own. Everything ImGui draws
        // is covered by the clip rectangles it recorded, so the union of those is the exact region
        // to claim - no full-window hand-off, and the menu stays whole wherever the ESP drew.
        //
        // The target is bound by the ESP pass only when that pass had vertices, so an empty ESP
        // frame would otherwise leave the menu drawing into nothing: bind it here, where the second
        // pass owns it. That is what made the menu vanish when the master switch or the last enabled
        // category went off, and flicker while typing (a revalidation empties the pass for a frame).
        if (i->gfx) {
            i->gfx->BindRenderTarget();
            if (interactive_) i->gfx->MarkDirty(0.f, 0.f, (float)i->gfx->Width(),
                                                (float)i->gfx->Height());
            MarkDrawnRegion(*i->gfx, *data);
        }
        ImGui_ImplDX11_RenderDrawData(data);
    }
    i->frame_begun = false;
}

// ------------------------------------------------------------- focus hand-off --

void ImGuiLayer::SetInteractive(bool on) {
    auto* i = impl_;
    if (!i || !active_ || !i->hwnd) return;
    if (on == interactive_) return;

    if (on) {
        LONG_PTR ex = GetWindowLongPtrA(i->hwnd, GWL_EXSTYLE);
        i->saved_ex_style = ex;
        i->style_saved = true;
        // Click-through off (clicks must reach the widgets), no-activate off (the window has to
        // take the keyboard for text fields to work at all). Layered and topmost stay.
        ex &= ~(LONG_PTR)(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
        SetWindowLongPtrA(i->hwnd, GWL_EXSTYLE, ex);
        SetWindowPos(i->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        // Takes the keyboard away from the client and frees the pointer, so no ALT press is needed
        // to get the mouse back. Whether it worked is reported: a menu without the foreground gets
        // no keys, and its first click only activates it - which reads as a menu whose X does
        // nothing, the exact symptom a silent refusal produced.
        if (RaiseWindowForeground(i->hwnd, true, &i->cursor_calls))
            AION_INFO("menu: took the foreground (keys and clicks reach the menu)");
        else
            AION_WARN("menu: the foreground was refused; keys will not reach the menu and the first "
                      "click only activates it");
        interactive_ = true;
    } else {
        interactive_ = false;
        if (i->style_saved) {
            SetWindowLongPtrA(i->hwnd, GWL_EXSTYLE, i->saved_ex_style);
            i->style_saved = false;
        }
        // Hand the pointer and the keyboard back. The client restores its own cursor clip when it
        // processes the activation, so only the cursor counter is undone here.
        for (int n = i->cursor_calls; n > 0; --n) ShowCursor(FALSE);
        i->cursor_calls = 0;
        i->reclaim_ms = 0;
        i->reclaim_logged = false;
        HWND game = (HWND)game_hwnd_;
        if (game) {
            if (!RaiseWindowForeground(game, false, nullptr))
                AION_INFO("menu: the client did not take the foreground back on close");
        } else {
            SetWindowPos(i->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    }
}

bool ImGuiLayer::WndProc(void* hwnd, unsigned msg, unsigned long long wparam, long long lparam) {
    if (!active_ || !interactive_) return false;
    // The overlay answers WM_NCHITTEST itself: HTTRANSPARENT while playing, DefWindowProc while
    // editing. Routing it to ImGui would leave the window unable to be clicked.
    if (msg == WM_NCHITTEST) return false;
    return ImGui_ImplWin32_WndProcHandler((HWND)hwnd, (UINT)msg, (WPARAM)wparam,
                                          (LPARAM)lparam) != 0;
}

// ------------------------------------------------------------------- style ----

void ImGuiLayer::ApplyStyle() {
    if (!active_) return;
    // Rebuild from scratch: scaling is applied at the end, and applying it twice would grow the
    // padding on every DPI change.
    ImGui::GetStyle() = ImGuiStyle();
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    // Soft corners and consistent spacing — reads as a product panel, not a debug widget.
    s.WindowRounding = 6.f;
    s.ChildRounding = 4.f;
    s.FrameRounding = 4.f;
    s.PopupRounding = 4.f;
    s.ScrollbarRounding = 4.f;
    s.GrabRounding = 3.f;
    s.TabRounding = 4.f;
    s.WindowBorderSize = 1.f;
    s.ChildBorderSize = 1.f;
    s.FrameBorderSize = 0.f;
    s.PopupBorderSize = 1.f;
    s.TabBorderSize = 0.f;
    s.WindowPadding = ImVec2(12, 10);
    s.FramePadding = ImVec2(8, 5);
    s.ItemSpacing = ImVec2(8, 7);
    s.ItemInnerSpacing = ImVec2(6, 5);
    s.CellPadding = ImVec2(8, 4);
    s.IndentSpacing = 16.f;
    s.ScrollbarSize = 12.f;
    s.GrabMinSize = 8.f;
    s.WindowTitleAlign = ImVec2(0.f, 0.5f);
    s.AntiAliasedLines = true;
    s.AntiAliasedFill = true;

    // Neutral charcoal surfaces + one cool accent (matches the menu session panel and debug HUD).
    const ImVec4 accent = ImVec4(0.45f, 0.62f, 0.98f, 1.00f);
    const ImVec4 accent_hover = ImVec4(0.55f, 0.70f, 1.00f, 1.00f);
    const ImVec4 accent_active = ImVec4(0.32f, 0.48f, 0.86f, 1.00f);
    const ImVec4 surface = ImVec4(0.09f, 0.095f, 0.115f, 0.96f);
    const ImVec4 surface_low = ImVec4(0.07f, 0.075f, 0.09f, 1.00f);
    const ImVec4 surface_high = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
    const ImVec4 line = ImVec4(0.24f, 0.26f, 0.32f, 1.00f);
    const ImVec4 text_dim = ImVec4(0.52f, 0.55f, 0.62f, 1.00f);

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = ImVec4(0.910f, 0.920f, 0.940f, 1.00f);
    c[ImGuiCol_TextDisabled] = text_dim;
    c[ImGuiCol_WindowBg] = surface;
    c[ImGuiCol_ChildBg] = surface_low;
    c[ImGuiCol_PopupBg] = ImVec4(0.098f, 0.102f, 0.115f, 0.98f);
    c[ImGuiCol_Border] = line;
    c[ImGuiCol_BorderShadow] = ImVec4(0.f, 0.f, 0.f, 0.f);
    c[ImGuiCol_FrameBg] = surface_high;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.190f, 0.200f, 0.225f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.225f, 0.235f, 0.265f, 1.00f);
    c[ImGuiCol_TitleBg] = surface_low;
    c[ImGuiCol_TitleBgActive] = surface_low;
    c[ImGuiCol_TitleBgCollapsed] = surface_low;
    c[ImGuiCol_MenuBarBg] = surface_low;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.f, 0.f, 0.f, 0.f);
    c[ImGuiCol_ScrollbarGrab] = line;
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.280f, 0.292f, 0.330f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive] = accent_active;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent_hover;
    c[ImGuiCol_Button] = surface_high;
    c[ImGuiCol_ButtonHovered] = ImVec4(0.18f, 0.20f, 0.26f, 1.00f);
    c[ImGuiCol_ButtonActive] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    c[ImGuiCol_Header] = ImVec4(accent.x, accent.y, accent.z, 0.28f);
    c[ImGuiCol_HeaderHovered] = ImVec4(accent.x, accent.y, accent.z, 0.40f);
    c[ImGuiCol_HeaderActive] = ImVec4(accent.x, accent.y, accent.z, 0.55f);
    c[ImGuiCol_Separator] = line;
    c[ImGuiCol_SeparatorHovered] = accent_hover;
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = ImVec4(accent.x, accent.y, accent.z, 0.22f);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(accent.x, accent.y, accent.z, 0.45f);
    c[ImGuiCol_ResizeGripActive] = accent;
    c[ImGuiCol_Tab] = surface_high;
    c[ImGuiCol_TabHovered] = ImVec4(accent.x, accent.y, accent.z, 0.45f);
    c[ImGuiCol_TabSelected] = ImVec4(accent.x, accent.y, accent.z, 0.65f);
    c[ImGuiCol_TabDimmed] = surface_low;
    c[ImGuiCol_TabDimmedSelected] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    c[ImGuiCol_TableHeaderBg] = surface_low;
    c[ImGuiCol_TableBorderStrong] = line;
    c[ImGuiCol_TableBorderLight] = ImVec4(0.180f, 0.190f, 0.215f, 1.00f);
    c[ImGuiCol_TableRowBg] = ImVec4(0.f, 0.f, 0.f, 0.f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.f, 1.f, 1.f, 0.020f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    c[ImGuiCol_NavCursor] = accent;

    auto* i = impl_;
    if (i && i->scale > 0.f) s.ScaleAllSizes(i->scale);
}

void ImGuiLayer::RebuildFonts(const std::string& face, int px) {
    auto* i = impl_;
    if (!i || !active_) return;
    i->face = face.empty() ? "Segoe UI" : face;
    i->px = px > 6 ? px : 16;

    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    const float size = (float)i->px * (i->scale > 0.f ? i->scale : 1.f);

    ImFontConfig cfg;
    cfg.OversampleH = 3;
    cfg.OversampleV = 2;
    cfg.PixelSnapH = true;
    cfg.RasterizerMultiply = 1.05f;
    const char* file = FontFileFor(i->face);
    if (!file && i->face == "Segoe UI")
        file = FontFileFor("Segoe UI Semibold");
    ImFont* font = nullptr;
    if (FileExists(file))
        font = io.Fonts->AddFontFromFileTTF(file, size, &cfg, io.Fonts->GetGlyphRangesDefault());
    if (!font) {
        // Unknown or missing face: the built-in bitmap font keeps the menu usable.
        io.Fonts->AddFontDefault();
    } else if (const CjkFace* cjk = PickCjkFace()) {
        // Merged, not replaced: the Latin face stays the primary so the menu looks the same, and
        // the CJK face fills in the glyphs it lacks (names typed in Korean or Chinese).
        ImFontConfig merge;
        merge.MergeMode = true;
        merge.OversampleH = 1;
        merge.OversampleV = 1;
        io.Fonts->AddFontFromFileTTF(cjk->file, size, &merge, cjk->ranges(io.Fonts));
    }
    io.Fonts->Build();
    ImGui_ImplDX11_InvalidateDeviceObjects();
    ImGui_ImplDX11_CreateDeviceObjects();
}

void ImGuiLayer::SetDisplayScale(float scale) {
    auto* i = impl_;
    if (!i || scale <= 0.f) return;
    if (i->scale == scale) return;
    i->scale = scale;
    ApplyStyle();
    RebuildFonts(i->face, i->px);
}

float ImGuiLayer::display_scale() const { return impl_ ? impl_->scale : 1.f; }

std::string ImGuiLayer::version() const { return IMGUI_VERSION; }

}  // namespace aion
