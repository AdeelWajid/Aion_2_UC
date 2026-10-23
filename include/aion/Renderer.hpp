// Renderer.hpp — backend-agnostic 2D overlay primitive interface + ESP renderer.
#pragma once
#include <cmath>
#include <string>

#include "Config.hpp"
#include "EntityCache.hpp"
#include "Sdk.hpp"

namespace aion {

struct Color {
    float R = 1, G = 1, B = 1, A = 1;
    static Color RGBA(int r, int g, int b, int a = 255) {
        return {r / 255.f, g / 255.f, b / 255.f, a / 255.f};
    }
};

// Implemented by the D3D11 overlay (src/Overlay.cpp) when injected.
class IOverlay {
public:
    // Two built-in bitmap fonts: Small is rendered 1:1 for ESP labels, Large for
    // the menu. Using an atlas at its native size keeps text crisp (no resampling).
    enum class FontSize { Small, Large };

    virtual ~IOverlay() = default;
    virtual bool Ready() const = 0;
    virtual int Width() const = 0;
    virtual int Height() const = 0;
    virtual void BeginFrame() = 0;
    virtual void Line(float x0, float y0, float x1, float y1, Color c, float thickness = 1.f) = 0;
    virtual void Rect(float x0, float y0, float x1, float y1, Color c, float thickness = 1.f) = 0;
    virtual void FillRect(float x0, float y0, float x1, float y1, Color c) = 0;
    virtual void Text(float x, float y, const std::string& s, Color c, float scale = 1.f) = 0;
    // Draws using the selected font. Backends without a small font fall back to Text().
    virtual void TextSized(float x, float y, const std::string& s, Color c, float scale,
                           FontSize fs) {
        (void)fs;
        Text(x, y, s, c, scale);
    }
    // Native cell height of a font at scale 1.0 (for layout).
    virtual float TextHeight(FontSize fs) const {
        return fs == FontSize::Small ? 12.f : 20.f;
    }
    // Ink width of a string at the given scale and font, so a label can be centred on a box
    // rather than hung off its left edge. Backends that cannot measure return the nominal
    // advance sum; the real overlay measures from the glyph advances it rasterizes with.
    virtual float TextWidth(const std::string& s, float scale, FontSize fs) const {
        return static_cast<float>(s.size()) * TextHeight(fs) * 0.52f * scale;
    }
    virtual void EndFrame() = 0;
};

// Packed 0xRRGGBB (as Config stores colours and aion.ini writes them) to a draw colour.
inline Color RgbColor(int rgb) {
    return Color::RGBA((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

// Draws the entity cache; knows nothing about the graphics API.
class EspRenderer {
public:
    // Renderer core; operates on a plain entity list so it is independent of collection.
    // Returns how many entities were drawn, for the on-screen performance panel.
    int RenderEntities(IOverlay& gfx, const Camera& cam, const std::vector<Entity>& entities,
                       const Config& cfg = Config::Instance()) const {
        if (!gfx.Ready() || !cfg.esp_enabled) return 0;
        const float frame_cx = gfx.Width() * 0.5f;
        // Floor at 0.05 rather than 0.5: aion.ini may ask for a deliberately faint overlay, and
        // clamping it up to half a pixel made that setting look broken. The shipped default and the
        // menu's own floor are both 1 px, so this only ever applies to a hand-edited file.
        const float th = cfg.esp_thickness < 0.05 ? 0.05f : (float)cfg.esp_thickness;
        const float ts = (float)cfg.esp_text_scale;
        const float line_h = gfx.TextHeight(IOverlay::FontSize::Small) * ts;
        const Color name_c = RgbColor(cfg.color_label);
        const Color dist_c = RgbColor(cfg.color_distance);
        const Color cls_c = RgbColor(cfg.color_class);
        // Diagnostic mode labels the real UObject class instead of the guessed category.
        const Color cls_all_c = RgbColor(cfg.color_class_all);
        const float text_pad = (float)std::ceil(th) + 1.f;

        // Snapline origin, resolved once per frame: Bottom and Top are the midpoint of that screen
        // edge; Player is the local character's own projected origin - its feet, the point every
        // range is measured from. The bottom edge is only the origin until the pawn has been seen.
        float snap_x = frame_cx, snap_y = (float)gfx.Height();
        if (cfg.snapline == SnaplineTop) {
            snap_y = 0.f;
        } else if (cfg.snapline == SnaplinePlayer) {
            // OnScreen is deliberately NOT required: it only means "in front of the camera this
            // frame", while Screen keeps the last point that projected - so a frame whose pawn read
            // failed holds the origin instead of dropping every line to the bottom edge, which is
            // exactly what this mode looked like when it went wrong. A pawn at (0,0) has never
            // projected, so it is not used as an origin either.
            for (const auto& e : entities) {
                if (!e.LocalPlayer) continue;
                if (e.Screen.X != 0.f || e.Screen.Y != 0.f) {
                    snap_x = e.Screen.X;
                    snap_y = e.Screen.Y;
                }
                break;
            }
        }

        int drawn = 0;
        for (const auto& e : entities) {
            if (drawn >= cfg.max_entities_drawn) break;
            // An actor the client is not rendering is only drawn when asked for: the cache drops
            // them unless esp_hidden is on, but the census path and SHOW ALL keep them, so the
            // renderer decides too rather than trusting whichever path built the list.
            if (e.ClientHidden && !cfg.esp_hidden && !cfg.esp_show_all) continue;
            // ... and it gets its own range, which is the whole point of the switch: what the
            // client shows is limited to max_distance_m, what it merely knows about reaches
            // hidden_range_m.
            const double range_m = e.ClientHidden ? cfg.hidden_range_m : cfg.max_distance_m;
            if (e.Distance > range_m * 100.0) continue;  // setting is metres, Distance is world units
            if (e.LocalPlayer && cfg.filter_local_player) continue;
            if (e.Dead && cfg.hide_dead) continue;
            // A listed target is drawn even when its own category is off: the hunting list is a
            // category of its own (the cull in EntityCache applies the same rule, or the actor is
            // collected and then not drawn).
            if (!CategoryVisible(cfg, e.Type) && !(e.Target && cfg.esp_target)) continue;

            // Behind the camera there is no projection at all (WorldToScreen returns false, so the
            // box is empty) and outside the viewport the box has no visible pixels. Both were
            // dropped here, which is what made an enemy ten metres behind you invisible until you
            // turned: the marker carries what the box would have said. The filters above still
            // apply, so an off-screen marker is exactly a box you cannot see yet.
            if (!e.OnScreen) {
                if (OffscreenMarker(gfx, cam, e, cfg, snap_x, snap_y, line_h, ts, th)) ++drawn;
                continue;
            }

            // Snap box edges to whole pixels for crisp 1px hairlines.
            const float x0 = std::round(e.BoundsMin.X);
            const float y0 = std::round(e.BoundsMin.Y);
            const float x1 = std::round(e.BoundsMax.X);
            const float y1 = std::round(e.BoundsMax.Y);
            // The box has no size: one of the two projected points did not resolve - a corner that
            // fell behind the camera plane. Dropping it here drew the entity as neither a box nor a
            // marker, because this test ran before the off-frame branch below: an actor standing in
            // front of you with one extent unresolvable simply vanished. Fall back to the marker,
            // which is what a box with no extent has to become.
            if (x1 - x0 < 2.f || y1 - y0 < 2.f) {
                if (OffscreenMarker(gfx, cam, e, cfg, snap_x, snap_y, line_h, ts, th)) ++drawn;
                continue;
            }
            // In front of the camera but outside the viewport: the same case as above, reached
            // through a projection that exists but lands off the frame.
            if (x1 < 0 || y1 < 0 || x0 > gfx.Width() || y0 > gfx.Height()) {
                if (OffscreenMarker(gfx, cam, e, cfg, snap_x, snap_y, line_h, ts, th)) ++drawn;
                continue;
            }

            const Color col = EntityColor(e, cfg);
            if (cfg.esp_box) {
                if (cfg.esp_box_corners)
                    CornerBox(gfx, x0, y0, x1, y1, col, th, cfg.esp_corner_frac);
                else
                    gfx.Rect(x0, y0, x1, y1, col, th);
            }
            // The snapline runs to the unrounded box centre. The box itself is snapped to
            // whole pixels so its hairline stays crisp, but a target snapped the same way
            // jumps a pixel at a time, which reads as the line twitching while the camera
            // swings. The stroke is feathered (see D3D11Overlay::Line), so a fractional
            // target costs nothing in smoothness of the line's own edges.
            if (cfg.snapline != SnaplineOff)
                gfx.Line(snap_x, snap_y, (e.BoundsMin.X + e.BoundsMax.X) * 0.5f, e.BoundsMax.Y,
                         col, th);
            // Labels are centred on the box: hung off the left edge they stop reading as part of
            // the box, and a long name runs away from it. The width comes from the same per-glyph
            // advances the rasterizer advances by, so the centring is exact rather than estimated.
            const float cx = (e.BoundsMin.X + e.BoundsMax.X) * 0.5f;
            const auto centred = [&](const std::string& s) {
                return cx - gfx.TextWidth(s, ts, IOverlay::FontSize::Small) * 0.5f;
            };
            float ty = y0 - line_h - text_pad;
            // Diagnostic mode forces both labels on and shows the real class, which is the
            // whole point: the box colour still encodes the guessed category.
            if (cfg.esp_name || cfg.esp_show_all) {
                gfx.TextSized(centred(e.Name), ty, e.Name, name_c, ts, IOverlay::FontSize::Small);
                ty -= line_h;
            }
            if (cfg.esp_class || cfg.esp_show_all) {
                const std::string cls_txt =
                    cfg.esp_show_all ? e.ClassName : EntityTypeName(e.Type);
                gfx.TextSized(centred(cls_txt), ty, cls_txt,
                              cfg.esp_show_all ? cls_all_c : cls_c, ts, IOverlay::FontSize::Small);
                ty -= line_h;
            }
            if (cfg.esp_distance) {
                char buf[64];
                // Distance is world units (cm); the ESP shows metres, as the client does.
                std::snprintf(buf, sizeof(buf), "%.0fm", e.Distance / 100.0);
                const std::string dist_txt(buf);
                // Clamped to the frame: a box whose bottom edge is on the last row would draw the
                // distance label entirely off-screen, and unlike the box the label has no other way
                // of being read. The off-screen markers already clamp their label this way.
                float dy = y1 + text_pad;
                if (dy + line_h > (float)gfx.Height() - 2.f)
                    dy = (float)gfx.Height() - 2.f - line_h;
                if (dy < 2.f) dy = 2.f;
                gfx.TextSized(centred(dist_txt), dy, dist_txt, dist_c, ts,
                              IOverlay::FontSize::Small);
            }
            ++drawn;
        }
        return drawn;
    }

    // An arrow pinned to the frame's border, pointing at an entity the camera is not showing,
    // labelled with what its box would have carried. Returns false when nothing was drawn, so the
    // caller can leave the draw budget alone.
    static bool OffscreenMarker(IOverlay& gfx, const Camera& cam, const Entity& e, const Config& cfg,
                                float snap_x, float snap_y, float line_h, float ts, float th) {
        if (!cfg.esp_offscreen) return false;
        if (e.Distance > cfg.offscreen_range_m * 100.0) return false;  // metres, like every range
        const float w = (float)gfx.Width(), h = (float)gfx.Height();
        // Inset by the arrow's own length plus a pixel: a marker half off the frame reads as a
        // smudge in the corner.
        const float margin = 20.f;
        float ax = 0.f, ay = 0.f, vx = 0.f, vy = 0.f;
        // A target the camera projected is placed by that projection - pitch, roll and all - so one
        // that is below the view lands on the bottom edge. Only a target behind the camera, which
        // has no projection, falls back to the yaw-plane bearing.
        if (e.OnScreen) {
            if (!BorderFromScreen(e.Screen, w, h, margin, ax, ay, vx, vy)) return false;
        } else if (!BorderPoint(cam, e.Location, w, h, margin, ax, ay, vx, vy)) {
            return false;
        }
        const Color col = EntityColor(e, cfg);
        // The same snapline a box gets, to the marker's tip on the border - it runs out along the
        // direction the arrow points, which is what makes a marker findable when several share an
        // edge. Off with esp_offscreen_snapline, and absent when snaplines are off altogether.
        if (cfg.esp_offscreen_snapline && cfg.snapline != SnaplineOff)
            gfx.Line(snap_x, snap_y, ax, ay, col, th);
        // Shaft with two barbs at the tip. Direction is the entire message, so the arrow has to
        // survive a glance: a bare line at the frame's edge would not.
        const float shaft = 12.f, barb = 5.5f;
        gfx.Line(ax - vx * shaft, ay - vy * shaft, ax, ay, col, th);
        const float px = -vy, py = vx;
        gfx.Line(ax, ay, ax - vx * barb + px * barb * 0.6f, ay - vy * barb + py * barb * 0.6f, col,
                 th);
        gfx.Line(ax, ay, ax - vx * barb - px * barb * 0.6f, ay - vy * barb - py * barb * 0.6f, col,
                 th);
        if (!cfg.esp_name && !cfg.esp_distance && !cfg.esp_show_all) return true;  // arrow only
        // One line inward from the arrow, on whichever side of the frame has room for it.
        std::string label;
        if (cfg.esp_name || cfg.esp_show_all) label = e.Name;
        if (cfg.esp_distance) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.0fm", e.Distance / 100.0);
            if (!label.empty()) label += " ";
            label += buf;
        }
        if (label.empty() || line_h <= 0.f) return true;
        // Measured with the same per-glyph advances the on-screen labels are centred with: the
        // interface exposes TextWidth for exactly this, and estimating from the font cell (half the
        // height per glyph) clipped long names and left short ones floating off their arrow.
        const float tw = gfx.TextWidth(label, ts, IOverlay::FontSize::Small);
        float tx = ax > w * 0.5f ? ax - tw - 6.f : ax + 6.f;
        float ty = ay - line_h * 0.5f;
        if (tx < 2.f) tx = 2.f;
        if (tx + tw > w - 2.f) tx = w - 2.f - tw;
        if (ty < 2.f) ty = 2.f;
        if (ty + line_h > h - 2.f) ty = h - 2.f - line_h;
        gfx.TextSized(tx, ty, label, RgbColor(cfg.color_label), ts, IOverlay::FontSize::Small);
        return true;
    }

    // Where the frame's border is met by the ray from the frame's centre through a target's own
    // projected screen point. This is the marker for anything the camera can project at all: that
    // point already carries pitch, roll, FOV and aspect, so a target below the view lands on the
    // bottom edge however far the camera is pitched up - the case BorderPoint below gets wrong,
    // because a bearing on the yaw plane has no pitch in it.
    static bool BorderFromScreen(const FVector2D& s, float w, float h, float margin, float& x,
                                 float& y, float& ux, float& uy) {
        const float hw = w * 0.5f - margin, hh = h * 0.5f - margin;
        if (hw <= 0.f || hh <= 0.f) return false;
        double vx = s.X - w * 0.5f, vy = s.Y - h * 0.5f;
        const double len = std::sqrt(vx * vx + vy * vy);
        // Dead centre leaves no direction to read; fall back to the bottom edge, the same
        // convention BorderPoint uses when a target sits on the camera's own position.
        if (len < 1e-6) { vx = 0.0; vy = 1.0; } else { vx /= len; vy /= len; }
        double t = 1e18;
        if (vx > 1e-9)       { const double q = hw / vx;  if (q < t) t = q; }
        else if (vx < -1e-9) { const double q = -hw / vx; if (q < t) t = q; }
        if (vy > 1e-9)       { const double q = hh / vy;  if (q < t) t = q; }
        else if (vy < -1e-9) { const double q = -hh / vy; if (q < t) t = q; }
        if (t > 1e17) return false;
        x = (float)(w * 0.5 + vx * t);
        y = (float)(h * 0.5 + vy * t);
        ux = (float)vx;
        uy = (float)vy;
        return true;
    }

    // Where on the frame's border a direction from the camera lands, and that direction as a unit
    // vector in screen axes (x right, y down). On the yaw plane only, which is what is left when
    // there is no projection: a target behind the camera cannot be projected - every such point
    // collapses to one direction - so the bearing it would have is read off the yaw plane instead,
    // forward being up the screen, which pins an entity behind the camera to the bottom edge. A
    // target ahead but off to the side lands on that side's edge, which is also where a box outside
    // the viewport belongs. Anything the camera CAN project goes through BorderFromScreen above.
    static bool BorderPoint(const Camera& cam, const FVector& loc, float w, float h, float margin,
                            float& x, float& y, float& ux, float& uy) {
        const float hw = w * 0.5f - margin, hh = h * 0.5f - margin;
        if (hw <= 0.f || hh <= 0.f) return false;
        const double d2r = 3.14159265358979323846 / 180.0;
        const double sy = std::sin(cam.Rotation.Yaw * d2r), cy = std::cos(cam.Rotation.Yaw * d2r);
        const double dx = loc.X - cam.Location.X, dy = loc.Y - cam.Location.Y;
        // The axes Projection.hpp uses, with roll and pitch dropped: forward = (cos yaw, sin yaw),
        // right = (-sin yaw, cos yaw).
        double vx = -dx * sy + dy * cy;    // right
        double vy = -(dx * cy + dy * sy);  // forward, and the screen's y grows downward
        const double len = std::sqrt(vx * vx + vy * vy);
        if (len < 1e-6) { vx = 0.0; vy = 1.0; } else { vx /= len; vy /= len; }
        // Ray from the frame's centre: the first of the four edges it meets.
        double t = 1e18;
        if (vx > 1e-9)       { const double s = hw / vx;  if (s < t) t = s; }
        else if (vx < -1e-9) { const double s = -hw / vx; if (s < t) t = s; }
        if (vy > 1e-9)       { const double s = hh / vy;  if (s < t) t = s; }
        else if (vy < -1e-9) { const double s = -hh / vy; if (s < t) t = s; }
        if (t > 1e17) return false;
        x = (float)(w * 0.5 + vx * t);
        y = (float)(h * 0.5 + vy * t);
        ux = (float)vx;
        uy = (float)vy;
        return true;
    }

    // Corner brackets: an L at each corner instead of a closed outline. The outline reads as
    // a frame around the character and competes with the name/class labels that sit on the
    // same edges; the brackets mark the same four extents and leave the sides open. Arms are
    // clamped to half the short side so two corners can never meet and re-form an outline.
    static void CornerBox(IOverlay& gfx, float x0, float y0, float x1, float y1, Color c,
                          float th, double frac) {
        const float w = x1 - x0, h = y1 - y0;
        const float shortest = w < h ? w : h;
        float len = (float)(frac * (double)shortest);
        const float half = shortest * 0.5f;
        if (len > half) len = half;
        if (len < 2.f) len = 2.f;  // shorter than this reads as a dot, not a bracket
        len = std::round(len);     // whole pixels, like the box edges
        gfx.Line(x0, y0, x0 + len, y0, c, th);  // top-left
        gfx.Line(x0, y0, x0, y0 + len, c, th);
        gfx.Line(x1 - len, y0, x1, y0, c, th);  // top-right
        gfx.Line(x1, y0, x1, y0 + len, c, th);
        gfx.Line(x0, y1, x0 + len, y1, c, th);  // bottom-left
        gfx.Line(x0, y1 - len, x0, y1, c, th);
        gfx.Line(x1 - len, y1, x1, y1, c, th);  // bottom-right
        gfx.Line(x1, y1 - len, x1, y1, c, th);
    }

    // Box colour. An actor on the user's target list is painted in the drop colour (yellow): it is
    // the one they are hunting, and sharing the colour loot already uses makes it read as "this is
    // what you came for". Checked before the boss colour, because putting an entry on the target
    // list is the more recent intent. Bosses keep the solid red every monster used to be drawn in
    // and ordinary monsters are pale red, so the ones that attack unprovoked stand out; which is
    // which is the user's declaration (list_boss) - the client is never told a monster's rank.
    static Color EntityColor(const Entity& e, const Config& cfg) {
        // An actor the client is not rendering reads in its own colour, and before the category:
        // the whole reason it is drawn at all is that it is NOT what its category colour would
        // suggest - it is a monster you cannot see yet.
        if (e.ClientHidden) return RgbColor(cfg.color_hidden);
        if (e.Target && cfg.esp_target) return RgbColor(cfg.color_target);
        if (e.Boss) return RgbColor(cfg.color_boss);
        return CategoryColor(e.Type, cfg);
    }

    // Category colour, from the config: the palette is data now, so every row of it is editable
    // in the menu (Colours section) and in aion.ini (color_*=RRGGBB) instead of only in the source.
    static Color CategoryColor(EntityType t, const Config& cfg) {
        switch (t) {
            case EntityType::Player:   return RgbColor(cfg.color_player);
            case EntityType::Npc:      return RgbColor(cfg.color_npc);
            case EntityType::Pet:      return RgbColor(cfg.color_pet);
            case EntityType::Monster:  return RgbColor(cfg.color_monster);
            case EntityType::Chest:    return RgbColor(cfg.color_chest);
            case EntityType::Item:     return RgbColor(cfg.color_item);
            case EntityType::Gather:   return RgbColor(cfg.color_gather);
            case EntityType::Interact: return RgbColor(cfg.color_interact);
            case EntityType::Cube:     return RgbColor(cfg.color_cube);
            case EntityType::Portal:   return RgbColor(cfg.color_portal);
            case EntityType::Object:   return RgbColor(cfg.color_object);
            default:                   return RgbColor(cfg.color_other);
        }
    }
};

}  // namespace aion
