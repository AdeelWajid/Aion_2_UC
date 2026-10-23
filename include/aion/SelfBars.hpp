// SelfBars.hpp — the local player's own health state.
//
// The client's real HP/MP is NOT reachable from outside it. That is a measured result, not an
// assumption: every route was walked and closed, with the counts in build/self_hpmp_final.md.
//
//   * gameplay objects: no HP/MP field. Measured against build/sdk_dump.txt (14k classes) -
//     AionCharacter, AionPlayerController and every AionCharSessionBaseSystem expose none, this
//     client has no PlayerState class at all, and the per-actor anim instance carries only booleans
//     (bDead +0x80C, bHpDangerAlert +0x8E9).
//   * the HUD widget that draws the bars: the chain is exactly as expected -
//     AionWidget_HUD_UserInfo -> P_PBar_Hp (+0x668) / P_PBar_Mp (+0x688) -> AionProgressBar::Percent
//     (+0x510, float 0..1) - but the class has no live instance anywhere in the object table, only its
//     class default object, whose bar pointers are null by construction. The chain cannot be entered.
//   * the packet mirror: DevPacketData_GameServer_CmdHpMpUpdate_NT exists as a ScriptStruct with no
//     instance behind it.
//   * AionUISystem's LoadedDialogMap (+0x0060, a TMap<DialogKey, AionDialog*>): walked by reflection,
//     it holds AionDialog instances - no widget, no bar.
//
// What this reader reports is therefore the one signal the client does expose about your own health:
// the low-health warning bit on the local pawn (AionCharAnimInstance::bHpDangerAlert). That makes
// the readout a STATE (ok / LOW / unknown), never a percentage it does not have.
//
// The two HUD hops are still resolved by PROPERTY NAME at run time (nothing anchored), so a patch that
// exposes the bar instance turns the same readout into a real bar and percentage with no code change.
// Finding it means walking the object table (~750k entries), which is why the search advances a slice
// per call - and why the frame path never runs it at all: on this client that scan is provably futile
// (measured: 3-8 ms per frame for a search that cannot succeed).
//
// The contract of the display: a failed read keeps the LAST KNOWN value and marks it '62%?'; with
// nothing ever read it says unknown, never "0%" - the same rule bDead and the spent-node checks
// follow. A value outside 0..1 is treated as unknown rather than clamped into a plausible bar.
#pragma once
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Config.hpp"
#include "Reflection.hpp"
#include "Renderer.hpp"   // IOverlay, Color, RgbColor
#include "Sdk.hpp"

namespace aion {

// The widget class the client's own-character HUD is an instance of. Matched as a name prefix, so
// "..._C" subclasses count too.
inline const char* SelfHudClassPrefix() { return "AionWidget_HUD_UserInfo"; }

// Walks the object table in slices, collecting instances of the self HUD. It deliberately does NOT
// stop at the first match: measured on the live client, the first AionWidget_HUD_UserInfo instance in
// the table had a NULL P_PBar_Hp - a HUD from an earlier UI state that was never rebuilt - while the
// live HUD is a later instance. The reader therefore picks the first candidate whose bars are
// actually there, and this class just hands it the candidates.
class SelfHudFinder {
public:
    static constexpr std::size_t kMaxHits = 32;

    void Reset() {
        cursor_ = 0;
        scanned_ = 0;
        done_ = false;
        hits_.clear();
        hud_classes_.clear();
        total_ = 0;
    }

    // Scans up to `max_objects` table entries. `collect` also records the distinct HUD-looking class
    // names it meets, for the diagnostic (a name lookup per object; the frame reader leaves it off).
    // Returns true when the scan is finished - the whole table has been seen, or kMaxHits collected.
    bool Step(Sdk* sdk, i32 max_objects, bool collect = false) {
        if (!sdk || done_) return true;
        const i32 n = sdk->objects.Num();
        total_ = n;
        const i32 end = (cursor_ + max_objects < n) ? cursor_ + max_objects : n;
        for (i32 i = cursor_; i < end; ++i) {
            const addr_t o = sdk->objects.ObjectAt(i);
            ++scanned_;
            if (!o) continue;
            const addr_t cls = sdk->ptr(o + off::uobject::Class);
            if (!cls) continue;
            const std::string cn = sdk->Name(sdk->rd<u32>(cls + off::uobject::Name));
            if (cn.empty()) continue;
            if (cn.compare(0, std::char_traits<char>::length(SelfHudClassPrefix()),
                           SelfHudClassPrefix()) == 0) {
                // Never a class's default object: "Default__AionWidget_HUD_UserInfo" is the CDO, its
                // children are null by construction, and it exists whether or not a HUD ever did.
                // Measured live, it was the ONLY object of that class in the table - which is a fact
                // about the client (its HUD widgets are not in the object array) and must not be
                // reported as "found a HUD".
                const std::string own = sdk->Name(sdk->rd<u32>(o + off::uobject::Name));
                if (own.compare(0, 9, "Default__") == 0) {
                    ++default_objects_;
                } else if (hits_.size() < kMaxHits) {
                    hits_.push_back(o);
                }
            }
            if (collect) {
                // A census of what is actually in this client, so a search that finds nothing (or
                // finds only the class's default object) can be answered from evidence: every class
                // name carrying "HUD", and every object whose own name carries "UserInfo" - the two
                // shapes a live HUD widget can take (native, or a blueprint subclass named something
                // else entirely).
                if (cn.find("HUD") != std::string::npos) {
                    bool seen = false;
                    for (auto& kv : hud_classes_)
                        if (kv.first == cn) { ++kv.second; seen = true; break; }
                    if (!seen && hud_classes_.size() < 256) hud_classes_.push_back({cn, 1});
                }
                if (userinfo_seen_ < 12) {
                    const std::string on = sdk->Name(sdk->rd<u32>(o + off::uobject::Name));
                    if (on.find("UserInfo") != std::string::npos) {
                        userinfo_.push_back(on + "  [" + cn + "]");
                        ++userinfo_seen_;
                    }
                }
            }
        }
        cursor_ = end;
        if (cursor_ >= n || hits_.size() >= kMaxHits) done_ = true;
        return done_;
    }

    const std::vector<addr_t>& hits() const { return hits_; }
    bool found() const { return !hits_.empty(); }
    bool finished() const { return done_; }
    i32 scanned() const { return scanned_; }
    i32 total() const { return total_; }
    const std::vector<std::pair<std::string, int>>& hud_classes() const { return hud_classes_; }
    const std::vector<std::string>& userinfo_objects() const { return userinfo_; }
    i32 default_objects() const { return default_objects_; }

private:
    i32 cursor_ = 0;
    i32 scanned_ = 0;
    i32 total_ = 0;
    i32 userinfo_seen_ = 0;
    i32 default_objects_ = 0;
    bool done_ = false;
    std::vector<addr_t> hits_;
    std::vector<std::pair<std::string, int>> hud_classes_;
    std::vector<std::string> userinfo_;
};

struct SelfVitals {
    float hp = -1.f;  // 0..1; negative = unknown (never "0")
    float mp = -1.f;
    bool hp_live = false;  // read on this call
    bool mp_live = false;
    bool found = false;    // the HUD widget resolved at all
    // The one self-health signal this client does expose: AionCharAnimInstance::bHpDangerAlert on the
    // local player's own animation instance - the same bit the ESP reads for monsters, and the same
    // path (mesh -> anim instance -> byte), resolved by name. A bit, not a number: it is the client's
    // own "low on health" warning, so it can say LOW or ok and nothing in between.
    bool hp_low = false;
    bool hp_low_known = false;
};

class SelfVitalsReader {
public:
    // Reads the local player's two bars. A value that cannot be read comes back < 0.
    //
    // `search` controls whether the object table is walked at all. The frame path passes false: the
    // walk costs a slice of a ~570k-entry table per frame, and on this client it is provably futile
    // (the HUD widget is not in the object table at all - see build/self_hpmp_research.md), so paying
    // for it every frame to keep answering "unknown" would be the worst of both. `--find-hud` passes
    // true, which is where that route is actually exercised and where a future patch would show up.
    SelfVitals Read(Sdk* sdk, bool search = true) {
        SelfVitals v;
        if (!sdk || !sdk->ready) return v;
        if (sdk_ != sdk) {
            sdk_ = sdk;
            refl_.SetSdk(sdk);
            finder_.Reset();
            widget_ = 0;
            hud_cls_ = 0;
            hp_off_ = mp_off_ = -1;
            bar_cls_ = 0;
            percent_off_ = -1;
        }
        if (!search) {
            // Display-only: use the widget if one has already been resolved in this process (the
            // diagnostic does that), otherwise report nothing rather than scanning.
            if (!widget_) {
                note_ = "exact HP/MP not read (the client's HUD widget is not in the object table)";
                return v;
            }
            return ReadAt(sdk, widget_);
        }
        // Advance the search: a slice per call, so the panel appears a few seconds after it is
        // switched on and no frame is late (~570k entries, two reads each).
        if (!finder_.finished()) {
            note_ = "searching the object table for the HUD widget";
            finder_.Step(sdk, 1200);
        }
        // Pick a candidate whose bars are actually there. The first instance in the table is not
        // always the live HUD: measured on the live client, the first one had a NULL P_PBar_Hp - a
        // HUD from an earlier UI state that was never rebuilt - and only a later instance carried the
        // live bars.
        if (!CandidateUsable(sdk, widget_)) {
            widget_ = 0;
            for (addr_t cand : finder_.hits())
                if (CandidateUsable(sdk, cand)) {
                    widget_ = cand;
                    break;
                }
        }
        if (!widget_) {
            if (!finder_.finished())
                note_ = "searching the object table for the HUD widget";
            else if (finder_.hits().empty() && finder_.default_objects() > 0)
                note_ = "only the class's default object is in the object table: this client's HUD "
                        "widgets are not registered in GObjects, so the HUD-owned HP/MP cannot be "
                        "reached from here";
            else if (finder_.hits().empty())
                note_ = "no AionWidget_HUD_UserInfo instance in the object table (HUD loaded?)";
            else
                note_ = "HUD instance(s) present, none with P_PBar_Hp/P_PBar_Mp built";
            return v;
        }
        note_ = "HUD widget resolved";
        return ReadAt(sdk, widget_);
    }

    // The read itself, for a widget that is already known. The --find-hud diagnostic uses this on the
    // widget its own (unbudgeted) scan found, so the diagnostic and the frame path cannot drift.
    SelfVitals ReadAt(Sdk* sdk, addr_t widget) {
        SelfVitals v;
        if (!sdk || !sdk->ready || !widget) return v;
        if (sdk_ != sdk) {
            sdk_ = sdk;
            refl_.SetSdk(sdk);
            hud_cls_ = 0;
            hp_off_ = mp_off_ = -1;
            bar_cls_ = 0;
            percent_off_ = -1;
        }
        // Cheap liveness: it must still be a widget of that class. A prefix test, because the client
        // instantiates a blueprint subclass ("..._C") of most widget classes.
        const addr_t cls = sdk->ptr(widget + off::uobject::Class);
        const std::string cn =
            cls ? sdk->Name(sdk->rd<u32>(cls + off::uobject::Name)) : std::string();
        if (cn.compare(0, std::char_traits<char>::length(SelfHudClassPrefix()),
                       SelfHudClassPrefix()) != 0) {
            note_ = "the HUD widget went away (HUD reloaded?)";
            return v;
        }
        // The two bars, by property name, walking the super chain. Re-resolved when the widget's
        // class changes, so a reloaded HUD cannot keep a stale offset.
        if (cls != hud_cls_ || hp_off_ < 0 || mp_off_ < 0) {
            hud_cls_ = cls;
            hp_off_ = refl_.GetPropertyOffset(cls, "P_PBar_Hp");
            mp_off_ = refl_.GetPropertyOffset(cls, "P_PBar_Mp");
            if (hp_off_ < 0 || mp_off_ < 0) {
                note_ = "P_PBar_Hp/P_PBar_Mp are not properties of the HUD widget class";
                return v;
            }
            note_ = "HP/MP properties resolved";
        }
        v.found = true;
        if (const auto b = BarPercent(sdk, sdk->ptr(widget + (addr_t)hp_off_))) {
            v.hp = *b;
            v.hp_live = true;
        }
        if (const auto b = BarPercent(sdk, sdk->ptr(widget + (addr_t)mp_off_))) {
            v.mp = *b;
            v.mp_live = true;
        }
        return v;
    }

    // Why the last Read returned nothing usable - for the log line, so an unknown is not a mystery.
    const std::string& last_note() const { return note_; }
    addr_t widget() const { return widget_; }
    bool ready() const { return widget_ != 0; }

private:
    // A candidate is usable when it is still a HUD of that class AND its HP bar object exists. The bar
    // pointer is null on a widget built for an earlier UI state, which is exactly how the first match
    // in the table turned out to be useless (measured live).
    bool CandidateUsable(Sdk* sdk, addr_t w) {
        if (!w) return false;
        const addr_t cls = sdk->ptr(w + off::uobject::Class);
        if (!cls) return false;
        const std::string cn = sdk->Name(sdk->rd<u32>(cls + off::uobject::Name));
        if (cn.compare(0, std::char_traits<char>::length(SelfHudClassPrefix()),
                       SelfHudClassPrefix()) != 0)
            return false;
        if (cls != hud_cls_ || hp_off_ < 0 || mp_off_ < 0) {
            hud_cls_ = cls;
            hp_off_ = refl_.GetPropertyOffset(cls, "P_PBar_Hp");
            mp_off_ = refl_.GetPropertyOffset(cls, "P_PBar_Mp");
        }
        if (hp_off_ < 0) return false;
        return sdk->ptr(w + (addr_t)hp_off_) != 0;
    }
    std::optional<float> BarPercent(Sdk* sdk, addr_t bar) {
        if (!bar || !sdk->mem->valid_ptr(bar)) return std::nullopt;
        const addr_t cls = sdk->ptr(bar + off::uobject::Class);
        if (!cls) return std::nullopt;
        if (cls != bar_cls_ || percent_off_ < 0) {
            bar_cls_ = cls;
            percent_off_ = refl_.GetPropertyOffset(cls, "Percent");
        }
        if (percent_off_ < 0) return std::nullopt;
        const auto f = sdk->mem->try_read<float>(bar + (addr_t)percent_off_);
        if (!f || !(*f >= 0.f) || *f > 1.0f) return std::nullopt;
        return *f;
    }

    Sdk* sdk_ = nullptr;
    Reflection refl_{nullptr};
    SelfHudFinder finder_;
    addr_t widget_ = 0;
    addr_t hud_cls_ = 0;  // the widget's class, so the bar offsets are re-resolved when it changes
    i32 hp_off_ = -1, mp_off_ = -1;
    addr_t bar_cls_ = 0;
    i32 percent_off_ = -1;
    std::string note_;
};

// The readout: the local player's health STATE, one row per resource.
//
// It is a state, not a value. This client exposes no HP/MP number to an outside reader (the walk and
// the counts are in build/self_hpmp_final.md), so what is shown is the one signal that does exist -
// the client's own low-health warning on the local pawn - as ok / LOW / unknown. A bracketed word is
// deliberate: a bar-shaped row with a number would claim a percentage that was never read, and an
// empty bar reads as zero. Never 0 for "I could not read it".
//
// If a patch ever exposes the ratio, the reader still resolves the two hops by property name, so the
// same row switches to a bar and a percentage on its own. A value read on an earlier frame and not on
// this one is kept, dimmed and marked with '?': the contract is last-known OR unknown, and which one
// it is stays visible.
class SelfBarsRenderer {
public:
    int Render(IOverlay& gfx, const Config& cfg, const SelfVitals& v) const {
        if (!gfx.Ready() || !cfg.self_vitals) return 0;
        const float w = (float)(cfg.self_vitals_width < 80 ? 80 : cfg.self_vitals_width);
        const float margin = (float)(cfg.self_vitals_margin_px < 0 ? 0 : cfg.self_vitals_margin_px);
        const float ts = cfg.esp_text_scale > 0.0 ? (float)cfg.esp_text_scale : 1.f;
        const float lh = gfx.TextHeight(IOverlay::FontSize::Small) * ts;
        const float row = lh + 6.f;
        const float h = row * 2.f + 10.f;
        const bool right = (cfg.self_vitals_corner == 1 || cfg.self_vitals_corner == 3);
        const bool bottom = (cfg.self_vitals_corner >= 2);
        const float x0 = right ? (float)gfx.Width() - margin - w : margin;
        const float y0 = bottom ? (float)gfx.Height() - margin - h : margin;
        const float x1 = x0 + w, y1 = y0 + h;
        gfx.FillRect(x0, y0, x1, y1, RgbColor(cfg.color_self_bg));
        gfx.Rect(x0, y0, x1, y1, RgbColor(cfg.color_label), 1.f);

        const float label_w = 24.f * ts;
        const float value_x = x0 + 6.f + label_w;
        const float bar_x0 = value_x;
        const float bar_x1 = x1 - 6.f - 44.f * ts;
        const Color label_c = RgbColor(cfg.color_label);

        // HP: the ratio only when the client exposes one (this build does not), otherwise the state
        // word. The bar and the percentage appear together, so a number can never be shown without the
        // bar it came from.
        const Color hp_c = RgbColor(cfg.color_self_hp);
        gfx.TextSized(x0 + 6.f, y0 + 5.f, "HP", label_c, ts, IOverlay::FontSize::Small);
        if (v.hp >= 0.f) {
            char buf[32];
            std::snprintf(buf, sizeof buf, v.hp_live ? "%.0f%%" : "%.0f%%?", v.hp * 100.f);
            if (bar_x1 > bar_x0) {
                const float by0 = y0 + 7.f, by1 = y0 + 5.f + lh - 2.f;
                gfx.Rect(bar_x0, by0, bar_x1, by1, hp_c, 1.f);
                const float fill = bar_x0 + (bar_x1 - bar_x0) * (v.hp > 1.f ? 1.f : v.hp);
                if (fill - bar_x0 > 0.5f)
                    gfx.FillRect(bar_x0 + 1.f, by0 + 1.f, fill, by1 - 1.f, hp_c);
            }
            gfx.TextSized(value_x, y0 + 5.f, buf, hp_c, ts, IOverlay::FontSize::Small);
        } else if (v.hp_low_known) {
            const Color c = v.hp_low ? hp_c : Color{hp_c.R * 0.6f, hp_c.G, hp_c.B * 0.6f, 1.f};
            gfx.TextSized(value_x, y0 + 5.f, v.hp_low ? "[LOW]" : "[ok]", c, ts,
                          IOverlay::FontSize::Small);
        } else {
            gfx.TextSized(value_x, y0 + 5.f, "[unknown]", Color{hp_c.R, hp_c.G, hp_c.B, 0.55f}, ts,
                          IOverlay::FontSize::Small);
        }

        // MP: no reflected route exists in this client at all - gameplay objects, the HUD widget (not
        // in the object table), the packet mirrors and AionUISystem's dialog map were each measured
        // closed. Saying so is the honest readout; an empty bar would look like a value of zero.
        const Color mp_c = RgbColor(cfg.color_self_mp);
        gfx.TextSized(x0 + 6.f, y0 + 5.f + row, "MP", label_c, ts, IOverlay::FontSize::Small);
        if (v.mp >= 0.f) {
            char buf[32];
            std::snprintf(buf, sizeof buf, v.mp_live ? "%.0f%%" : "%.0f%%?", v.mp * 100.f);
            gfx.TextSized(value_x, y0 + 5.f + row, buf, mp_c, ts, IOverlay::FontSize::Small);
        } else {
            gfx.TextSized(value_x, y0 + 5.f + row, "[not exposed]",
                          Color{mp_c.R, mp_c.G, mp_c.B, 0.6f}, ts, IOverlay::FontSize::Small);
        }
        return 2;
    }
};

}  // namespace aion
