// Overlay.hpp — D3D11 immediate-mode 2D overlay (external process).
//
// The overlay owns a D3D11 render target, rasterizes into it and hands the result
// to a layered, click-through, topmost window through UpdateLayeredWindow(ULW_ALPHA),
// i.e. true per-pixel alpha (see Present()).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Renderer.hpp"

namespace aion {

struct D3D11OverlayImpl;

// Bitmap font the overlay rasterizes at init. Glyphs are drawn 1:1, so these are
// true screen pixels (not scaled) - that is what keeps text crisp.
struct FontConfig {
    std::string face = "Segoe UI";
    int esp_px = 14;   // font em height in pixels for ESP labels
    int menu_px = 19;  // font em height in pixels for the menu
    bool bold = false;
    bool shadow = true;  // 1px black drop shadow, for legibility over the game
};

struct OverlayStats {
    double flush_ms = 0.0;    // last frame's draw submit
    double present_ms = 0.0;  // last frame's own hand-off cost (readback + copy + UpdateLayeredWindow)
    // Time spent inside the composition swapchain's Present() waiting for the compositor to take the
    // frame. That is the DISPLAY's cadence, not this program's cost: on the composition path the
    // frame cannot be handed over faster than it is consumed, so charging it to the frame's budget
    // reported every paced frame as over budget. Kept separate and shown beside the hand-off.
    double wait_ms = 0.0;
    int verts = 0;            // vertices submitted last frame
    int dirty_w = 0, dirty_h = 0;  // presented region size
    bool presented = false;        // false when the frame was skipped
};

class D3D11Overlay final : public IOverlay {
public:
    D3D11Overlay();
    ~D3D11Overlay() override;

    // Create device + offscreen target for our own window.
    bool InitWindow(void* hwnd, const FontConfig& fonts = {});
    void Shutdown();

    // Re-rasterize the font atlas (e.g. after the user changed the size).
    void SetFontConfig(const FontConfig& fonts);
    const FontConfig& font_config() const;
    const OverlayStats& stats() const;

    // Cap for UpdateLayeredWindow (0 = present every frame). Readback + the DWM
    // hand-off dominate the per-frame cost, so this is the main CPU dial.
    void set_present_fps(int fps);
    // Block in Present until vblank. Off by default: the composition path is paced
    // by the host loop instead, which keeps latency low and CPU flat.
    void set_vsync(bool on);
    // Prefer the composition swapchain (call before InitWindow). Off forces the
    // UpdateLayeredWindow path.
    void set_use_dcomp(bool on);

    bool Ready() const override;
    int Width() const override;
    int Height() const override;
    // Which present path is actually live: DirectComposition can fall back to the CPU
    // UpdateLayeredWindow path at init, so the request in the config is not the answer.
    bool using_dcomp() const;
    void BeginFrame() override;   // clears the target to fully transparent
    void Line(float x0, float y0, float x1, float y1, Color c, float thickness) override;
    void Rect(float x0, float y0, float x1, float y1, Color c, float thickness) override;
    void FillRect(float x0, float y0, float x1, float y1, Color c) override;
    void Text(float x, float y, const std::string& s, Color c, float scale) override;
    void TextSized(float x, float y, const std::string& s, Color c, float scale,
                   FontSize fs) override;
    float TextHeight(FontSize fs) const override;
    float TextWidth(const std::string& s, float scale, FontSize fs) const override;
    void EndFrame() override;     // flush primitives + present
    void Present();               // readback + UpdateLayeredWindow

    // Renders the accumulated primitives into the current target and clears them.
    void Flush();

    // Saves the rendered frame to a .bmp (verification/debug).
    bool SaveBackbufferToBmp(const std::string& path);

    // Writes the rasterized glyph atlas to a .bmp (font quality debugging).
    bool SaveFontAtlas(const std::string& path);

    // --- second-pass host (the ImGui menu) -------------------------------------------
    // The menu rasterizes with its own pipeline, so it needs the device and context this overlay
    // created - there is no second device to be had for the same window.
    void* d3d_device() const;
    void* d3d_context() const;
    // Called after our primitives are flushed and before the frame is presented. The target is
    // bound only when Flush() had vertices to draw, which an empty frame (ESP off, or nothing in
    // the enabled categories) never does - so a second pass MUST call BindRenderTarget() rather
    // than assume the binding is still there.
    void set_pre_present_hook(std::function<void()> fn);

    // Binds this frame's render target. Idempotent and required by any second pass: without it the
    // ImGui draw is issued against whatever target was bound last, so a frame with no ESP vertices
    // drew no menu at all - invisible while still eating clicks.
    void BindRenderTarget();

    // Adds a rectangle to this frame's dirty region, in window pixels. The UpdateLayeredWindow path
    // hands the compositor the union of the marked regions only, so geometry drawn without a mark is
    // visible just where something else was marked, and is never repainted once it stops being drawn
    // - which is what a menu that cannot be closed and a menu that vanishes look like. The
    // composition path presents the whole surface and ignores this.
    void MarkDirty(float x0, float y0, float x1, float y1);

private:
    D3D11OverlayImpl* impl_ = nullptr;
};

}  // namespace aion
