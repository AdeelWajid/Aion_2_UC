// DodgeTelegraph.hpp — read-only dodge timing signals from the live client.
#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "Sdk.hpp"

namespace aion {

class Reflection;
struct Config;
struct FVector;

struct TelegraphReading {
    bool urge = false;
    const char* reason = nullptr;
    int projectiles_near = 0;
    int ui_matches = 0;
    int anim_props_hot = 0;
};

class DodgeTelegraphScanner {
public:
    TelegraphReading Scan(Sdk* sdk, Reflection* refl, const Config& cfg, addr_t local_pawn,
                          const FVector& origin);

private:
    std::chrono::steady_clock::time_point last_scan_{};
    bool slow_ready_ = false;
    int last_ui_matches_ = 0;
    int last_projectile_count_ = 0;
    bool prev_anim_hot_ = false;
    std::string last_anim_prop_;
    i32 object_cursor_ = 0;
    i32 projectile_cursor_ = 0;
    bool slice_pass_complete_ = false;
    int ui_pass_accum_ = 0;
    std::unordered_set<addr_t> projectiles_in_range_;
    std::string urge_reason_buf_;
    bool ScanAnimFlags(Sdk* sdk, Reflection* refl, addr_t pawn, const Config& cfg);
    void ScanProjectilesSlice(Sdk* sdk, const Config& cfg, const FVector& origin, int* count_out,
                              bool* urge_out);
    void ScanObjectSliceUi(Sdk* sdk, const Config& cfg, int* ui_out);
    void PruneProjectileSet(Sdk* sdk, const Config& cfg, const FVector& origin);
    static void SplitCsv(const std::string& csv, std::vector<std::string>& out);
};

// Headless: list anim bools and live class names that match dodge UI hints.
int ScanDodgeHintsReport(Sdk* sdk, Reflection* refl);

}  // namespace aion
