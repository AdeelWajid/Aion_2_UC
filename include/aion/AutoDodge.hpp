// AutoDodge.hpp — optional dodge-key assist (external input).
//
// Triggers (Config::auto_dodge_trigger):
//   0 — off (test button still works)
//   1 — low-HP panic (bHpDangerAlert rising edge)
//   2 — telegraph rising edge only
//   3 — hp edge or telegraph edge (projectile / ui rise / anim rise)
#pragma once
#include <chrono>
#include <cstdint>
#include <string>

#include <windows.h>

#include "DodgeTelegraph.hpp"
#include "Memory.hpp"
#include "Sdk.hpp"

namespace aion {

struct Config;
class Reflection;

struct AutoDodgeStats {
    unsigned fires = 0;
    int ms_since_last = -1;
    const char* last_reason = nullptr;
    int telegraph_projectiles = 0;
    int telegraph_ui = 0;
    int telegraph_anim = 0;
    bool last_test_focus_ok = false;
    const char* last_input_backend = nullptr;
};

class AutoDodge {
public:
    struct TickArgs {
        const Config* cfg = nullptr;
        Sdk* sdk = nullptr;
        Reflection* refl = nullptr;
        addr_t local_pawn = 0;
        FVector origin{};
        bool menu_open = false;
        bool game_in_front = false;
        std::uint32_t game_pid = 0;
        HWND game_hwnd = nullptr;
        HWND overlay_hwnd = nullptr;
        bool hp_low = false;
        bool hp_low_known = false;
    };

    void RequestTest() { test_pending_ = true; }
    bool HasPendingTest() const { return test_pending_; }
    AutoDodgeStats Stats() const;
    void Tick(const TickArgs& args);

private:
    bool CanSend(const TickArgs& args) const;
    void Fire(WORD vk, WORD move_vk, int input_method, const char* reason, HWND game_hwnd,
              int attempts = 1);
    bool HpTrigger(const TickArgs& args) const;

    DodgeTelegraphScanner scanner_;
    std::chrono::steady_clock::time_point last_fire_{};
    bool prev_hp_low_ = false;
    bool test_pending_ = false;
    unsigned fires_ = 0;
    std::string last_reason_text_;
    const char* last_reason_ = nullptr;
    int last_proj_ = 0;
    int last_ui_ = 0;
    int last_anim_ = 0;
    bool last_test_focus_ok_ = false;
    const char* last_input_backend_ = nullptr;
};

bool GameProcessInForeground(std::uint32_t game_pid, HWND game_hwnd, HWND overlay_hwnd);

// 0x10 (VK_SHIFT) → 0xA0 (VK_LSHIFT); use before send and in config UI.
int NormalizeDodgeVk(int vk);

}  // namespace aion
