// AutoDodge.cpp — see AutoDodge.hpp.
#include "aion/AutoDodge.hpp"

#include <thread>

#include "aion/Config.hpp"
#include "aion/DodgeTelegraph.hpp"
#include "aion/Foreground.hpp"
#include "aion/KeyboardInject.hpp"
#include "aion/Reflection.hpp"

namespace aion {

int NormalizeDodgeVk(int vk) {
    if (vk == VK_SHIFT) return VK_LSHIFT;
    if (vk == VK_CONTROL) return VK_LCONTROL;
    if (vk == VK_MENU) return VK_LMENU;
    return vk;
}

namespace {

bool SameProcessWindow(HWND a, HWND b) {
    if (!a || !b) return false;
    DWORD pa = 0, pb = 0;
    GetWindowThreadProcessId(a, &pa);
    GetWindowThreadProcessId(b, &pb);
    return pa && pa == pb;
}

bool GameProcessHasForeground(HWND game) {
    HWND fg = GetForegroundWindow();
    return fg && SameProcessWindow(game, fg);
}

DodgeInputMethod InputMethodFromConfig(int v) {
    if (v == 1) return DodgeInputMethod::SendInput;
    if (v == 2) return DodgeInputMethod::Interception;
    if (v == 3) return DodgeInputMethod::FakerInput;
    return DodgeInputMethod::Auto;
}

}  // namespace

bool GameProcessInForeground(std::uint32_t game_pid, HWND game_hwnd, HWND overlay_hwnd) {
    const HWND fg = GetForegroundWindow();
    if (!fg) return false;
    if (fg == overlay_hwnd) return true;
    DWORD fg_pid = 0;
    GetWindowThreadProcessId(fg, &fg_pid);
    if (fg_pid != game_pid) return false;
    if (game_hwnd && IsIconic(game_hwnd)) return false;
    return true;
}

AutoDodgeStats AutoDodge::Stats() const {
    AutoDodgeStats s;
    s.fires = fires_;
    s.last_reason = last_reason_;
    s.telegraph_projectiles = last_proj_;
    s.telegraph_ui = last_ui_;
    s.telegraph_anim = last_anim_;
    s.last_test_focus_ok = last_test_focus_ok_;
    s.last_input_backend = last_input_backend_;
    if (fires_ == 0) return s;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - last_fire_)
                        .count();
    s.ms_since_last = (int)ms;
    return s;
}

bool AutoDodge::CanSend(const TickArgs& args) const {
    if (!args.cfg || !args.cfg->auto_dodge) return false;
    if (args.menu_open) return false;
    if (!args.game_in_front) return false;
    if (args.cfg->auto_dodge_vk <= 0 || args.cfg->auto_dodge_vk > 0xFE) return false;
    return GameProcessInForeground(args.game_pid, args.game_hwnd, args.overlay_hwnd);
}

void AutoDodge::Fire(WORD vk, WORD move_vk, int input_method, const char* reason, HWND game_hwnd,
                     int attempts) {
    TapDodgeCombo(vk, move_vk, InputMethodFromConfig(input_method), game_hwnd, attempts);
    last_input_backend_ = DodgeInputBackendName(LastDodgeInputBackend());
    last_fire_ = std::chrono::steady_clock::now();
    ++fires_;
    last_reason_text_ = reason ? reason : "";
    last_reason_ = last_reason_text_.empty() ? nullptr : last_reason_text_.c_str();
}

bool AutoDodge::HpTrigger(const TickArgs& args) const {
    return args.hp_low_known && args.hp_low && !prev_hp_low_;
}

namespace {

bool WantsTelegraphScan(int trig) { return trig >= 2 && trig <= 3; }

int NormalizeTrigger(int trig) {
    if (trig == 4) return 3;
    if (trig < 0 || trig > 3) return 3;
    return trig;
}

}  // namespace

void AutoDodge::Tick(const TickArgs& args) {
    const int input_method = args.cfg ? args.cfg->auto_dodge_input : 0;

    if (test_pending_) {
        test_pending_ = false;
        last_test_focus_ok_ = false;
        const int vk = NormalizeDodgeVk(args.cfg ? args.cfg->auto_dodge_vk : 0);
        const int move_vk = args.cfg ? args.cfg->auto_dodge_move_vk : 0;
        if (vk > 0 && vk <= 0xFE && args.game_hwnd) {
            const bool hid_overlay =
                args.overlay_hwnd && IsWindow(args.overlay_hwnd);
            if (hid_overlay) ShowWindow(args.overlay_hwnd, SW_HIDE);
            bool focused = FocusGameForKeyboard(args.game_hwnd);
            if (!focused) {
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
                focused = FocusGameForKeyboard(args.game_hwnd);
            }
            if (focused) std::this_thread::sleep_for(std::chrono::milliseconds(120));
            last_test_focus_ok_ = GameProcessHasForeground(args.game_hwnd);
            if (last_test_focus_ok_)
                Fire((WORD)vk, (WORD)move_vk, input_method, "test", args.game_hwnd, 3);
            if (hid_overlay) {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                ShowWindow(args.overlay_hwnd, SW_SHOWNOACTIVATE);
            }
        }
        if (!args.cfg || !args.cfg->auto_dodge) return;
    }

    if (!args.cfg || !args.cfg->auto_dodge) {
        if (args.hp_low_known) prev_hp_low_ = args.hp_low;
        return;
    }

    if (args.menu_open) {
        if (args.hp_low_known) prev_hp_low_ = args.hp_low;
        return;
    }

    TelegraphReading tr;
    const int trig = NormalizeTrigger(args.cfg->auto_dodge_trigger);
    if (args.sdk && args.refl && args.sdk->objects.ok() && WantsTelegraphScan(trig)) {
        tr = scanner_.Scan(args.sdk, args.refl, *args.cfg, args.local_pawn, args.origin);
        last_proj_ = tr.projectiles_near;
        last_ui_ = tr.ui_matches;
        last_anim_ = tr.anim_props_hot;
    }

    const auto now = std::chrono::steady_clock::now();
    constexpr int kHeldUrgeMs = 1500;
    if (held_urge_) {
        const auto held_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - held_urge_at_)
                                 .count();
        if (held_ms > kHeldUrgeMs) held_urge_ = false;
    }

    bool signal = false;
    const char* reason = nullptr;
    if (trig == 1 && HpTrigger(args)) {
        signal = true;
        reason = "hp danger";
    } else if (trig == 2 && tr.urge) {
        signal = true;
        reason = tr.reason ? tr.reason : "telegraph";
    } else if (trig == 3) {
        if (HpTrigger(args)) {
            signal = true;
            reason = "hp danger";
        } else if (tr.urge) {
            signal = true;
            reason = tr.reason ? tr.reason : "telegraph";
        }
    }
    if (args.hp_low_known) prev_hp_low_ = args.hp_low;

    bool cooldown_ok = true;
    if (fires_ > 0) {
        const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_fire_)
                               .count();
        cooldown_ok = since >= args.cfg->auto_dodge_cooldown_ms;
    }

    const bool can_send = CanSend(args);
    if (signal && (!can_send || !cooldown_ok)) {
        held_urge_ = true;
        held_urge_reason_text_ =
            reason ? reason : (tr.reason ? tr.reason : std::string("telegraph"));
        held_urge_reason_ = held_urge_reason_text_.c_str();
        held_urge_at_ = now;
    }

    bool fire = signal && can_send && cooldown_ok;
    if (!fire && held_urge_ && can_send && cooldown_ok) {
        fire = true;
        reason = held_urge_reason_text_.empty() ? "telegraph" : held_urge_reason_;
    }
    if (!fire) return;

    held_urge_ = false;
    if (args.game_hwnd && !GameProcessHasForeground(args.game_hwnd))
        FocusGameForKeyboard(args.game_hwnd);
    Fire((WORD)args.cfg->auto_dodge_vk, (WORD)args.cfg->auto_dodge_move_vk, input_method,
         reason, args.game_hwnd, 2);
}

}  // namespace aion
