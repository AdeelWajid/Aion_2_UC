// Hud.hpp - on-screen performance/state panel, drawn only when cfg.esp_debug is on.
//
// Replaces the one-line "ents=.. fov=.. loc=.." print that used to sit at (10, 10). The
// panel exists to answer one question - where did the frame time go - because an uncapped
// overlay spends most of its time reading the game's memory and the actor count is the
// only thing that scales that cost. So the five phases are drawn as a stacked bar, each
// phase gets a colour swatch keyed to its bar segment, and each phase's millisecond value
// is coloured against the display's frame budget (1000/Hz): amber once a phase eats the
// whole budget, red at twice it. "Slow" is a different number on a 144 Hz panel than on a
// 60 Hz one, which is why the budget is read from the display instead of being fixed.
//
// The rows are grouped into coloured sections - FRAME, WORLD, DEVICE, CAMERA, READER - because
// nineteen unlabelled rows of numbers read as one undifferentiated list, and the question the
// panel is opened for ("is my handle being stripped") should not require knowing its row order by
// heart. Each heading owns a hue no state colour uses: green/amber/red stay reserved for state, so
// a coloured heading can never be read as a warning. For the same reason the title line carries a
// one-word verdict for the whole session, covering the three conditions that need action.
#pragma once

#include <cstddef>
#include <cstdio>

#include "Config.hpp"
#include "Renderer.hpp"

namespace aion {

// Everything the panel shows; filled by the host loop once per frame.
struct HudStats {
    double fps = 0.0;                 // frames presented per second (1s window)
    double update_ms = 0.0;           // entity collection + memory reads
    double reproj_ms = 0.0;           // camera re-read + re-projection
    double draw_ms = 0.0;             // ESP + panel drawing
    double flush_ms = 0.0;            // vertex submit
    double present_ms = 0.0;          // compositor hand-off (this program's own cost)
    double present_wait_ms = 0.0;     // time inside Present() waiting for the compositor (pacing)
    int verts = 0;                    // vertices submitted last frame
    std::size_t listed = 0;           // actors in the level this frame
    std::size_t kept = 0;             // actors that passed filtering (entities in the draw list)
    std::size_t culled = 0;           // actors skipped because their category is off
    std::size_t validated = 0;        // actors whose cached metadata was refreshed
    int drawn = 0;                    // entities actually drawn last frame
    unsigned handle_reopens = 0;      // process handle re-opens (anti-cheat stripping). The first
                                      // acquisition is not a re-open, so 0 is the healthy value.
    unsigned stripped = 0;            // reads that hit a dead handle: a strip seen from the read
    bool cam_valid = false;
    float fov = 0.f;
    double cam_x = 0, cam_y = 0, cam_z = 0;
    bool dcomp = false;               // true = DirectComposition present path
    int width = 0, height = 0;        // overlay surface size
    int display_hz = 0;
};

namespace hud_detail {

// Panel geometry in pixels, all offsets from the panel's top-left corner. The value
// column sits at a fixed offset and the value formats below use fixed field widths
// ("%7.2f"), which together are what makes the digits line up down the panel.
inline constexpr float kMargin = 10.f;  // inset from the screen edge
inline constexpr float kY = 10.f;
inline constexpr float kWidth = 292.f;
inline constexpr float kPad = 10.f;         // interior margin on all four sides
inline constexpr float kLabelX = 10.f;
inline constexpr float kValueX = 96.f;
inline constexpr float kNoteX = 176.f;
inline constexpr float kLineGap = 4.f;      // title -> first row
inline constexpr float kGroupGap = 6.f;     // last row -> bar
inline constexpr float kBarWidth = 120.f;
inline constexpr float kBarHeight = 10.f;
// Body rows drawn below, group headings included; keep in step with the rows drawn in
// DrawHud, since the panel background is sized before any of them are emitted.
inline constexpr int kRows = 25;
// The font size in pixels those numbers were measured at: the default esp_font_px on a 1080p-tall
// client. They are units of that font, not absolute pixels - see the scale factor in DrawHud.
inline constexpr float kRefFontPx = 14.f;

// Warning colour for a phase time, measured against the display's budget. A phase that
// eats the whole budget leaves nothing for the rest of the frame, so that is where the
// value turns amber; twice the budget is red. Without a reported display rate there is
// nothing to compare against, so the plain colour stays.
inline Color BudgetColor(double ms, double budget, Color plain, Color warn, Color bad) {
    if (budget <= 0.0) return plain;
    if (ms >= 2.0 * budget) return bad;
    if (ms >= budget) return warn;
    return plain;
}

}  // namespace hud_detail

// Draws the panel at the top-left. Safe to call unconditionally: it returns immediately
// unless esp_debug is on. Nothing here allocates - every value goes through a stack
// buffer, and every string handed to the overlay is short enough to stay in the
// std::string small-string buffer the API builds on the way in.
inline void DrawHud(IOverlay& gfx, const Config& cfg, const HudStats& st) {
    if (!cfg.esp_debug || !gfx.Ready()) return;

    // Every dimension is in units of the small font, scaled here rather than drawn as fixed pixels.
    // Fixed pixels are only correct at the one font size they were measured at, and esp_font_auto
    // rasterizes the font 1.33x larger on a 1440p client: the rows grew with the text (their height
    // comes from the font) while the columns did not, so values ran into the notes and the notes off
    // the panel's right edge. Scaling the geometry by the same factor keeps the margins that fit at
    // the reference size.
    const float font_px = static_cast<float>(EspFontPx(cfg, gfx.Height()));
    const float s = font_px > 0.f ? font_px / hud_detail::kRefFontPx : 1.f;
    using hud_detail::kRows;
    const float kBarHeight = hud_detail::kBarHeight * s;
    const float kBarWidth = hud_detail::kBarWidth * s;
    const float kGroupGap = hud_detail::kGroupGap * s;
    const float kLabelX = hud_detail::kLabelX * s;
    const float kLineGap = hud_detail::kLineGap * s;
    const float kNoteX = hud_detail::kNoteX * s;
    const float kPad = hud_detail::kPad * s;
    const float kValueX = hud_detail::kValueX * s;
    const float kWidth = hud_detail::kWidth * s;
    const float kMargin = hud_detail::kMargin * s;
    const float kY = hud_detail::kY * s;

    // Same palette as the menu, so the panel does not look like a separate program.
    const Color bg = Color::RGBA(14, 16, 22, 238);
    const Color border = Color::RGBA(72, 118, 210, 220);
    const Color title_c = Color::RGBA(115, 158, 250, 255);
    const Color text = Color::RGBA(220, 220, 220, 255);
    const Color dim = Color::RGBA(130, 130, 130, 255);
    const Color ok = Color::RGBA(80, 230, 120, 255);
    const Color warn = Color::RGBA(255, 190, 60, 255);
    const Color bad = Color::RGBA(255, 70, 70, 255);
    // Group accents. None of them is a state colour on purpose: a heading is a label, not news.
    const Color g_frame = Color::RGBA(255, 168, 64, 255);
    const Color g_world = Color::RGBA(110, 235, 150, 255);
    const Color g_device = Color::RGBA(110, 200, 255, 255);
    const Color g_camera = Color::RGBA(190, 165, 255, 255);
    const Color g_reader = Color::RGBA(255, 130, 205, 255);

    const auto fs_small = IOverlay::FontSize::Small;
    const auto fs_large = IOverlay::FontSize::Large;
    const float line = gfx.TextHeight(fs_small);
    const float title_h = gfx.TextHeight(fs_large);

    const double budget = st.display_hz > 0 ? 1000.0 / (double)st.display_hz : 0.0;

    // The phases in the order the host spends the time. The swatch before each label is the
    // legend for the stacked bar; the value colour is reserved for the budget warning, so phase
    // identity (the swatch) and budget state (the value) can never be confused with each other.
    struct Phase {
        const char* label;
        double ms;
        Color c;
    };
    const Phase phases[5] = {
        {"update", st.update_ms, Color::RGBA(255, 150, 40)},   // the memory reads
        {"reproj", st.reproj_ms, Color::RGBA(90, 200, 255)},
        {"draw", st.draw_ms, Color::RGBA(80, 230, 120)},
        {"flush", st.flush_ms, Color::RGBA(170, 170, 190)},
        {"present", st.present_ms, Color::RGBA(255, 80, 200)},
    };
    double total = 0.0;
    for (const Phase& p : phases) total += p.ms;

    // One word for the state that needs attention, in the order it matters: with no camera nothing
    // else on this panel means anything, a strip means the reader is being interfered with, and an
    // over-budget frame means the ESP is spending the user's frame time. Both the word and the dot
    // beside it carry this, so it is legible at a glance and without colour.
    const char* health = "OK";
    Color hcol = ok;
    if (!st.cam_valid) {
        health = "NO CAMERA";
        hcol = bad;
    } else if (st.stripped > 0) {
        health = "STRIPPED";
        hcol = warn;
    } else if (budget > 0.0 && total > budget) {
        health = "OVER BUDGET";
        hcol = bad;
    } else if (budget > 0.0 && total > budget * 0.8) {
        health = "TIGHT";
        hcol = warn;
    }

    // Top-right corner, not top-left: the menu panel owns the left third of the screen
    // (x 40..500) and two translucent panels over each other veil whichever is underneath
    // to about 8% - the menu's title and first rows disappeared behind this panel.
    const float kx = (float)gfx.Width() - kWidth - kMargin > kMargin
                         ? (float)gfx.Width() - kWidth - kMargin
                         : kMargin;
    const float lx = kx + kLabelX;
    const float vx = kx + kValueX;
    const float nx = kx + kNoteX;
    const float top = kY + kPad;
    const float h = kPad + title_h + kLineGap + (float)kRows * line + kGroupGap + kBarHeight +
                    kPad;

    gfx.FillRect(kx, kY, kx + kWidth, kY + h, bg);
    gfx.Rect(kx, kY, kx + kWidth, kY + h, border, 1.f);
    gfx.TextSized(lx, top, "PERF", title_c, 1.f, fs_large);

    // The verdict, right-aligned on the title line and led by a dot in its own colour, so the
    // state reads before the letters do - the words themselves survive a screenshot in greyscale.
    const float badge_x = kx + kWidth - kPad - gfx.TextWidth(health, 1.f, fs_small);
    const float badge_y = top + (title_h - line) * 0.5f;
    const float dot_y = badge_y + line * 0.5f;
    gfx.FillRect(badge_x - 10.f * s, dot_y - 3.f * s, badge_x - 4.f * s, dot_y + 3.f * s, hcol);
    gfx.TextSized(badge_x, badge_y, health, hcol, 1.f, fs_small);

    float ry = top + title_h + kLineGap;
    char buf[64], note[64];

    // One row, three columns: label, value, and a note carrying the unit or the comparison. Almost
    // every row below is this shape, so it is one helper instead of five lines apiece - which is
    // what the flat version cost, and why its rows could drift out of alignment one at a time.
    const auto row = [&](const char* label, const char* value, Color vc, const char* note_text) {
        gfx.TextSized(lx, ry, label, text, 1.f, fs_small);
        gfx.TextSized(vx, ry, value, vc, 1.f, fs_small);
        if (note_text && *note_text) gfx.TextSized(nx, ry, note_text, dim, 1.f, fs_small);
        ry += line;
    };
    // The same, led by a colour swatch rather than plain text: the phase rows, whose swatch is the
    // bar's legend.
    const auto rowSw = [&](Color sw, const char* label, const char* value, Color vc,
                           const char* note_text) {
        gfx.FillRect(lx, ry + 2.f * s, lx + 6.f * s, ry + line - 2.f * s, sw);
        gfx.TextSized(lx + 10.f, ry, label, text, 1.f, fs_small);
        gfx.TextSized(vx, ry, value, vc, 1.f, fs_small);
        if (note_text && *note_text) gfx.TextSized(nx, ry, note_text, dim, 1.f, fs_small);
        ry += line;
    };
    // A group heading: its own hue plus a rule in the same hue, so the panel reads as sections
    // rather than as one column of numbers.
    const auto group = [&](const char* name, Color c) {
        gfx.TextSized(lx, ry, name, c, 1.f, fs_small);
        const float rule = ry + line - 3.f * s;
        gfx.Line(lx, rule, kx + kWidth - kPad, rule, Color{c.R, c.G, c.B, 0.35f}, 1.f);
        ry += line;
    };

    // ---- FRAME: where the frame time went ------------------------------------------------
    group("FRAME", g_frame);
    {
        // The FPS counter is only bad news when the display is not being kept up with, so it is
        // coloured against the panel's own rate instead of an absolute number.
        std::snprintf(buf, sizeof buf, "%7.1f", st.fps);
        Color fc = text;
        if (st.display_hz > 0)
            fc = st.fps >= st.display_hz * 0.9 ? ok : (st.fps >= st.display_hz * 0.5 ? warn : bad);
        if (st.display_hz > 0)
            std::snprintf(note, sizeof note, "@%d Hz", st.display_hz);
        else
            std::snprintf(note, sizeof note, "Hz n/a");
        row("fps", buf, fc, note);
    }
    for (int i = 0; i < 5; ++i) {
        std::snprintf(buf, sizeof buf, "%7.2f ms", phases[i].ms);
        if (i == 4) {
            // The hand-off row also carries the compositor wait, which is deliberately NOT part of the
            // frame total: that time is the display taking frames, not this program spending time.
            if (st.present_wait_ms > 0.05)
                std::snprintf(note, sizeof note, "waiting %.2f ms", st.present_wait_ms);
            else
                std::snprintf(note, sizeof note, "");
            rowSw(phases[i].c, phases[i].label, buf,
                  hud_detail::BudgetColor(phases[i].ms, budget, text, warn, bad), note);
        } else {
            rowSw(phases[i].c, phases[i].label, buf,
                  hud_detail::BudgetColor(phases[i].ms, budget, text, warn, bad),
                  i == 0 ? "mem reads" : "");
        }
    }
    {
        std::snprintf(buf, sizeof buf, "%7.2f ms", total);
        if (budget > 0.0)
            std::snprintf(note, sizeof note, "budget %.2f ms", budget);
        else
            std::snprintf(note, sizeof note, "budget n/a");
        row("frame", buf, hud_detail::BudgetColor(total, budget, text, warn, bad), note);
    }
    {
        // The headroom is the number the user acts on: negative means the frame cannot make the
        // display's deadline at all, whatever the FPS counter says.
        const double headroom = budget > 0.0 ? budget - total : 0.0;
        Color hc = text;
        if (budget > 0.0) hc = headroom <= 0.0 ? bad : (headroom < budget * 0.2 ? warn : ok);
        std::snprintf(buf, sizeof buf, "%7.2f ms", headroom);
        row("headroom", buf, hc,
            budget > 0.0 ? (headroom < 0.0 ? "over budget" : "spare") : "");
    }
    std::snprintf(buf, sizeof buf, "%7d", st.verts);
    row("verts", buf, dim, "");

    // Stacked bar: the five widths are the five phase shares of the frame, so it shows the split
    // and not the absolute cost - the ms values above carry that. It sits directly under its own
    // section's rows, where the swatches beside them are still its legend. The last edge is pinned
    // to the bar end, because accumulated float shares otherwise leave a seam.
    {
        const float bar_y = ry + kGroupGap;
        gfx.FillRect(lx, bar_y, lx + kBarWidth, bar_y + kBarHeight, Color::RGBA(40, 40, 48, 255));
        if (total > 0.0) {
            double acc = 0.0;
            float sx = lx;
            for (int i = 0; i < 5; ++i) {
                acc += phases[i].ms;
                const float ex =
                    i == 4 ? lx + kBarWidth : lx + (float)(acc / total) * kBarWidth;
                if (ex > sx) gfx.FillRect(sx, bar_y, ex, bar_y + kBarHeight, phases[i].c);
                sx = ex;
            }
        }
        gfx.Rect(lx, bar_y, lx + kBarWidth, bar_y + kBarHeight, border, 1.f);
        ry = bar_y + kBarHeight;
    }

    // ---- WORLD: the actor pipeline, in the order the cache reports it --------------------
    group("WORLD", g_world);
    std::snprintf(buf, sizeof buf, "%7zu", st.listed);
    row("listed", buf, text, "");
    std::snprintf(buf, sizeof buf, "%7zu", st.kept);
    row("kept", buf, text, "");
    // Culled actors are work deliberately not done, so they read as background.
    std::snprintf(buf, sizeof buf, "%7zu", st.culled);
    row("culled", buf, dim, "");
    std::snprintf(buf, sizeof buf, "%7zu", st.validated);
    row("validated", buf, text, "");
    std::snprintf(buf, sizeof buf, "%7d", st.drawn);
    row("drawn", buf, ok, "on screen");

    // ---- DEVICE: how the surface reaches the screen --------------------------------------
    group("DEVICE", g_device);
    // The CPU path reads the backbuffer back every frame, so it is called out in amber: it is the
    // first thing to turn off when the frame time does not add up.
    row("present", st.dcomp ? "GPU alpha" : "CPU alpha", st.dcomp ? ok : warn,
        st.dcomp ? "composition" : "layered");
    std::snprintf(buf, sizeof buf, "%dx%d", st.width, st.height);
    if (st.display_hz > 0)
        std::snprintf(note, sizeof note, "@%d Hz", st.display_hz);
    else
        note[0] = '\0';
    row("surface", buf, text, note);

    // ---- CAMERA -------------------------------------------------------------------------
    group("CAMERA", g_camera);
    std::snprintf(note, sizeof note, "fov %.1f", st.fov);
    row("camera", st.cam_valid ? "valid" : "invalid", st.cam_valid ? ok : bad, note);
    std::snprintf(buf, sizeof buf, "%.0f,%.0f,%.0f", st.cam_x, st.cam_y, st.cam_z);
    row("pos", buf, dim, "");

    // ---- READER: whether the client is leaving our handle alone --------------------------
    group("READER", g_reader);
    // A strip is the one event that explains a stall no timing figure shows. The first open is
    // acquisition, not a re-open, so both of these are 0 on a healthy session; `stripped` counts
    // the reads that actually failed on a dead handle, so the two agree unless a re-open failed.
    std::snprintf(buf, sizeof buf, "%7u", st.handle_reopens);
    row("reopens", buf, st.handle_reopens ? warn : ok,
        st.handle_reopens ? "handle dropped" : "clean");
    std::snprintf(buf, sizeof buf, "%7u", st.stripped);
    row("stripped", buf, st.stripped ? warn : ok, st.stripped ? "reads lost" : "");
}

}  // namespace aion
