// DodgeTelegraph.cpp — see DodgeTelegraph.hpp.
#include "aion/DodgeTelegraph.hpp"

#include <cctype>
#include <cstdio>
#include <unordered_set>

#include "aion/Config.hpp"
#include "aion/Logger.hpp"
#include "aion/Offsets.hpp"
#include "aion/Reflection.hpp"
#include "aion/Strings.hpp"
#include "aion/UObject.hpp"

namespace aion {
namespace {

constexpr i32 kObjectScanBudget = 5000;
constexpr i32 kProjectileScanBudget = 12000;

// Class name only — matching actor *names* caught telegraph widgets and scenery (spam).
bool IsProjectileClass(const std::string& cls) { return HasI(cls, "Projectile"); }

bool InRangeSq(const FVector& a, const FVector& b, double max_sq) {
    const double dx = a.X - b.X;
    const double dy = a.Y - b.Y;
    const double dz = a.Z - b.Z;
    return dx * dx + dy * dy + dz * dz <= max_sq;
}

bool ActorLocation(Sdk* sdk, addr_t actor, FVector& out) {
    if (!sdk || !actor) return false;
    const addr_t root = sdk->ptr(actor + off::aactor::RootComponent);
    if (!sdk->mem->valid_ptr(root)) return false;
    out = sdk->ReadVector(root + off::uscene_component::RelativeLocation);
    return true;
}

bool ClassMatchesHints(const std::string& cls, const std::vector<std::string>& hints) {
    for (const auto& h : hints)
        if (!h.empty() && HasI(cls, h.c_str())) return true;
    return false;
}

}  // namespace

void DodgeTelegraphScanner::SplitCsv(const std::string& csv, std::vector<std::string>& out) {
    out.clear();
    std::string cur;
    for (char c : csv) {
        if (c == ',' || c == ';') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else if (!std::isspace((unsigned char)c)) {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
}

void DodgeTelegraphScanner::PruneProjectileSet(Sdk* sdk, const Config& cfg,
                                               const FVector& origin) {
    if (!sdk || projectiles_in_range_.empty()) return;
    const double max_uu = cfg.auto_dodge_range_m * 100.0;
    const double max_sq = max_uu * max_uu;
    for (auto it = projectiles_in_range_.begin(); it != projectiles_in_range_.end();) {
        const addr_t o = *it;
        UObject uo(sdk, o);
        if (!uo.IsValid() || uo.IsGarbage()) {
            it = projectiles_in_range_.erase(it);
            continue;
        }
        if (!IsProjectileClass(uo.GetClassName())) {
            it = projectiles_in_range_.erase(it);
            continue;
        }
        FVector loc;
        if (!ActorLocation(sdk, o, loc)) {
            it = projectiles_in_range_.erase(it);
            continue;
        }
        if (!InRangeSq(loc, origin, max_sq))
            it = projectiles_in_range_.erase(it);
        else
            ++it;
    }
    if (projectiles_in_range_.empty()) last_projectile_count_ = 0;
}

void DodgeTelegraphScanner::ScanProjectilesSlice(Sdk* sdk, const Config& cfg,
                                                 const FVector& origin, bool* urge_out) {
    *urge_out = false;
    if (!sdk || !cfg.auto_dodge_projectiles || !sdk->objects.ok()) return;

    const double max_uu = cfg.auto_dodge_range_m * 100.0;
    const double max_sq = max_uu * max_uu;
    const i32 n = sdk->objects.Num();
    if (n <= 0) {
        projectiles_in_range_.clear();
        last_projectile_count_ = 0;
        return;
    }

    const i32 step = n < kProjectileScanBudget ? n : kProjectileScanBudget;
    for (i32 k = 0; k < step; ++k) {
        const i32 i = (projectile_cursor_ + k) % n;
        const addr_t o = sdk->objects.ObjectAt(i);
        if (!o) continue;
        UObject uo(sdk, o);
        if (!uo.IsValid() || uo.IsGarbage()) continue;
        if (!IsProjectileClass(uo.GetClassName())) continue;
        FVector loc;
        if (!ActorLocation(sdk, o, loc)) continue;
        if (!InRangeSq(loc, origin, max_sq)) continue;
        // Urge only when a new in-range projectile actor appears (not on re-scan of the same set).
        if (projectiles_in_range_.insert(o).second) *urge_out = true;
    }
    projectile_cursor_ = (projectile_cursor_ + step) % n;
    if (projectile_cursor_ == 0) {
        PruneProjectileSet(sdk, cfg, origin);
        const int count = (int)projectiles_in_range_.size();
        if (count == 0)
            last_projectile_count_ = 0;
        else
            last_projectile_count_ = count;
    }
}

void DodgeTelegraphScanner::ScanObjectSliceUi(Sdk* sdk, const Config& cfg, int* ui_out) {
    *ui_out = 0;
    if (!sdk || !cfg.auto_dodge_ui_objects) return;
    std::vector<std::string> hints;
    SplitCsv(cfg.auto_dodge_class_hints, hints);
    const i32 n = sdk->objects.Num();
    if (n <= 0) return;

    const i32 step = n < kObjectScanBudget ? n : kObjectScanBudget;
    for (i32 k = 0; k < step; ++k) {
        const i32 i = (object_cursor_ + k) % n;
        const addr_t o = sdk->objects.ObjectAt(i);
        if (!o) continue;
        UObject uo(sdk, o);
        if (!uo.IsValid() || uo.IsGarbage()) continue;
        if (ClassMatchesHints(uo.GetClassName(), hints)) ++(*ui_out);
    }
    object_cursor_ = (object_cursor_ + step) % n;
    if (object_cursor_ == 0) slice_pass_complete_ = true;
}

bool DodgeTelegraphScanner::ScanAnimFlags(Sdk* sdk, Reflection* refl, addr_t pawn,
                                          const Config& cfg) {
    if (!sdk || !refl || !pawn || !cfg.auto_dodge_anim_flags) return false;
    if (!UObject(sdk, pawn).IsValid()) return false;
    const addr_t mesh = sdk->ptr(pawn + off::acharacter::Mesh);
    if (!sdk->mem->valid_ptr(mesh)) return false;
    const addr_t comp_class = sdk->ptr(mesh + off::uobject::Class);
    if (!sdk->mem->valid_ptr(comp_class)) return false;
    const i32 anim_off = refl->GetPropertyOffset(comp_class, "AnimScriptInstance");
    if (anim_off < 0) return false;
    const addr_t anim = sdk->ptr(mesh + (addr_t)anim_off);
    if (!sdk->mem->valid_ptr(anim)) return false;
    const addr_t anim_class = sdk->ptr(anim + off::uobject::Class);
    if (!sdk->mem->valid_ptr(anim_class)) return false;

    std::vector<std::string> props;
    SplitCsv(cfg.auto_dodge_anim_props, props);
    for (const auto& name : props) {
        const i32 off = refl->GetPropertyOffset(anim_class, name);
        if (off < 0) continue;
        if (sdk->mem->u8_at(anim + (addr_t)off) != 0) {
            last_anim_prop_ = name;
            return true;
        }
    }
    last_anim_prop_.clear();
    return false;
}

TelegraphReading DodgeTelegraphScanner::Scan(Sdk* sdk, Reflection* refl, const Config& cfg,
                                             addr_t local_pawn, const FVector& origin) {
    TelegraphReading r;
    const auto now = std::chrono::steady_clock::now();
    const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_scan_)
                           .count();
    const int period_ms = cfg.auto_dodge_scan_period_ms < 80 ? 80 : cfg.auto_dodge_scan_period_ms;
    const bool do_slow = !slow_ready_ || since >= period_ms;

    if (do_slow) {
        last_scan_ = now;
        slow_ready_ = true;

        const bool anim_hot = ScanAnimFlags(sdk, refl, local_pawn, cfg);
        r.anim_props_hot = anim_hot ? 1 : 0;
        if (anim_hot && !prev_anim_hot_) {
            r.urge = true;
            urge_reason_buf_ = last_anim_prop_.empty() ? "anim flag" : last_anim_prop_;
            r.reason = urge_reason_buf_.c_str();
        }
        prev_anim_hot_ = anim_hot;

        bool proj_urge = false;
        ScanProjectilesSlice(sdk, cfg, origin, &proj_urge);
        if (proj_urge) {
            r.urge = true;
            urge_reason_buf_ = "projectile";
            r.reason = urge_reason_buf_.c_str();
        }

        int slice_ui = 0;
        ScanObjectSliceUi(sdk, cfg, &slice_ui);
        ui_pass_accum_ += slice_ui;
        if (slice_pass_complete_) {
            const int prev_ui = last_ui_matches_;
            r.ui_matches = ui_pass_accum_;
            if (r.ui_matches > prev_ui) {
                r.urge = true;
                urge_reason_buf_ = "ui telegraph";
                r.reason = urge_reason_buf_.c_str();
            }
            last_ui_matches_ = r.ui_matches;
            ui_pass_accum_ = 0;
            slice_pass_complete_ = false;
        } else {
            r.ui_matches = last_ui_matches_;
        }
    } else {
        r.ui_matches = last_ui_matches_;
        r.anim_props_hot = prev_anim_hot_ ? 1 : 0;
    }

    r.projectiles_near = (int)projectiles_in_range_.size();
    return r;
}

int ScanDodgeHintsReport(Sdk* sdk, Reflection* refl) {
    if (!sdk || !refl) return 1;
    std::printf("%s", SectionForStdio("dodge anim properties (AionCharAnimInstance)").c_str());
    UObject anim_cls = refl->FindClass("AionCharAnimInstance");
    if (!anim_cls.Addr()) anim_cls = refl->FindClass("AnimInstance");
    if (anim_cls.Addr()) {
        static const char* kNeed[] = {"Dodge",  "Evade",  "Counter", "Parry",  "Block",
                                      "Warn",   "Tele",   "Guard",   "Perfect"};
        for (const auto& pi : refl->EnumerateProperties(anim_cls.Addr())) {
            if (pi.Type != "BoolProperty") continue;
            bool hit = false;
            for (const char* n : kNeed)
                if (HasI(pi.Name, n)) {
                    hit = true;
                    break;
                }
            if (hit) std::printf("  bool %s @+%d\n", pi.Name.c_str(), pi.Offset);
        }
    } else {
        std::printf("  (no anim instance class found)\n");
    }

    std::printf("%s", SectionForStdio("live classes matching dodge UI hints").c_str());
    static const char* kHints[] = {"Telegraph", "Dodge", "BlockWindow", "Counter", "Parry",
                                   "Evade",     "Warn",  "Perfect",     "HitZone"};
    std::unordered_set<std::string> seen;
    const i32 n = sdk->objects.Num();
    for (i32 i = 0; i < n; ++i) {
        const addr_t o = sdk->objects.ObjectAt(i);
        if (!o) continue;
        const std::string cn = UObject(sdk, o).GetClassName();
        for (const char* h : kHints) {
            if (!HasI(cn, h)) continue;
            if (seen.insert(cn).second)
                std::printf("  %s (e.g. '%s')\n", cn.c_str(), UObject(sdk, o).GetName().c_str());
            break;
        }
    }
    std::printf("%s", SectionForStdio("projectile actors").c_str());
    int proj = 0;
    for (i32 i = 0; i < n; ++i) {
        const addr_t o = sdk->objects.ObjectAt(i);
        if (!o) continue;
        const std::string cn = UObject(sdk, o).GetClassName();
        if (!HasI(cn, "Projectile")) continue;
        if (++proj <= 8)
            std::printf("  %s '%s'\n", cn.c_str(), UObject(sdk, o).GetName().c_str());
    }
    std::printf("  total with 'Projectile' in class: %d\n", proj);
    return 0;
}

}  // namespace aion
