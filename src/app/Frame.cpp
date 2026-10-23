// src/app/Frame.cpp — see Frame.hpp.
//
// The loop below is the original one, moved: every statement keeps its place and its comment, and
// the names it already used are bound to the context it is handed.
#include "Frame.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Client.hpp"      // DumpActorClasses
#include "Window.hpp"      // ShouldQuit, FindGameWindow, GameClientRect, FontsFromConfig
#include "aion/AutoDodge.hpp"
#include "aion/Camera.hpp"
#include "aion/Reflection.hpp"
#include "aion/Config.hpp"
#include "aion/DataFile.hpp"
#include "aion/EntityCache.hpp"
#include "aion/Hud.hpp"
#include "aion/ImGuiLayer.hpp"
#include "aion/Logger.hpp"
#include "aion/Menu.hpp"
#include "aion/NamePool.hpp"
#include "aion/Obf.hpp"
#include "aion/Offsets.hpp"
#include "aion/Overlay.hpp"
#include "aion/Projection.hpp"
#include "aion/RealNames.hpp"
#include "aion/Renderer.hpp"
#include "aion/SelfBars.hpp"
#include "aion/Sdk.hpp"
#include "aion/World.hpp"

namespace aion {
namespace app {

void RunFrameLoop(FrameContext& fc) {
    Sdk& sdk = fc.sdk;
    ExternalMemory& mem = fc.mem;
    Config& cfg = fc.cfg;
    CameraExtractor& camx = fc.camx;
    Projection& proj = fc.proj;
    EntityCache& cache = fc.cache;
    NameTable& names = fc.names;
    Menu& menu = fc.menu;
    AutoDodge& auto_dodge = fc.auto_dodge;
    Reflection& refl = fc.refl;
    ImGuiLayer& imgui = fc.imgui;
    D3D11Overlay& gfx = fc.gfx;
    HWND overlay = fc.overlay;
    const std::uint32_t pid = fc.pid;
    const Options& opt = fc.opt;

    MSG msg;
    int frame = 0;
    int frames_since_log = 0;
    bool soak_done = false;  // reported by the exit summary, not by its own line
    // Last frame's drawn total, kept for the menu's Debug pane (the value itself is computed
    // inside the camera-valid branch).
    int last_drawn = 0;
    double ms_update = 0.0, ms_reproj = 0.0, ms_draw = 0.0;
    double last_fps = 0.0;  // frames per second over the last logging window
    const auto loop_start = std::chrono::steady_clock::now();

    while (!ShouldQuit()) {
        const auto frame_start = std::chrono::steady_clock::now();
        // The soak deadline is checked here, above the idle path: --seconds is a documented way to
        // run for a fixed time, and a client that is not in the foreground hides the overlay and
        // skips the rest of the iteration - which used to make that timer never fire.
        if (opt.run_seconds > 0 &&
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - loop_start).count() >= opt.run_seconds) {
            soak_done = true;
            break;
        }
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        // Keep the overlay aligned with the game window. The handle is cached and
        // re-resolved once a second - enumerating top-level windows every frame was
        // pure overhead.
        const auto now_t = std::chrono::steady_clock::now();
        static HWND game_hwnd = nullptr;
        static RECT prev_rect{};
        static auto last_find = std::chrono::steady_clock::now() - std::chrono::seconds(10);
        if (!IsWindow(game_hwnd) ||
            std::chrono::duration_cast<std::chrono::milliseconds>(now_t - last_find).count() >=
                1000) {
            last_find = now_t;
            HWND found = FindGameWindow(pid);
            if (!found) { AION_INFO("game window gone; exiting"); break; }
            game_hwnd = found;
        }
        // INSERT is polled globally (the overlay is click-through while playing). This must run
        // before the idle continue: when the game is unfocused the loop skips the rest of the
        // iteration and INSERT would never be seen, so the menu could not be opened from a hidden
        // overlay or recovered without restarting.
        {
            static bool insert_down = false;
            const bool insert_now = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
            if (insert_now && !insert_down) menu.Toggle();
            insert_down = insert_now;
        }
        if (menu.request_close) {
            menu.request_close = false;
            menu.Close();
        }
        imgui.set_game_window(game_hwnd);
        imgui.SetInteractive(menu.Open());

        RECT gr{};
        // While the menu is open the window stops following the client. Taking the foreground from a
        // fullscreen client makes it switch display mode, its rect changes, and re-sizing our window
        // for it re-creates the D3D surface under a menu the user is typing into - which is the
        // flash, and sometimes a window small enough that the menu is clipped away entirely. A
        // cleared prev_rect re-applies the client's rect on the first frame after the menu closes.
        if (imgui.Interactive()) {
            prev_rect = RECT{-1, -1, -1, -1};
        } else if (GameClientRect(game_hwnd, gr) &&
                   (gr.left != prev_rect.left || gr.top != prev_rect.top ||
                    gr.right != prev_rect.right || gr.bottom != prev_rect.bottom)) {
            SetWindowPos(overlay, HWND_TOPMOST, gr.left, gr.top, gr.right - gr.left,
                         gr.bottom - gr.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            prev_rect = gr;
        }
        // Idle when the game is not the foreground window: nothing is visible, so
        // skip the whole read/render/present pipeline.
        // Idle when the game is not the window in front: nothing is visible, so skip the whole
        // read/render/present pipeline - which is also what keeps an alt-tab out of both the
        // client's and our own CPU.
        static int idle_state = -1;
        // Not while the menu is open: editing is what makes the *game* unfocused, so this rule
        // would hide the window being typed into.
        if (cfg.skip_when_unfocused && !opt.capture_mode && !menu.Open()) {
            // "In front" is compared by process id, not by handle: the client is one UE window
            // among several of its process, and a child window coming to the front (or a host
            // window like ApplicationFrameHost for a UWP app) does not change the window the user
            // is looking at. A minimised game is never in front either, even though Windows keeps
            // it as the foreground window - which is the alt-tab case this exists for. The overlay
            // itself counts as in front: it is WS_EX_NOACTIVATE and can never take focus, so
            // treating it as "not the game" would blink the panel off mid-edit.
            const HWND fg = GetForegroundWindow();
            bool game_in_front = false;
            if (fg == overlay) {
                game_in_front = true;
            } else if (fg) {
                DWORD fg_pid = 0;
                GetWindowThreadProcessId(fg, &fg_pid);
                game_in_front = (fg_pid == pid) && !IsIconic(game_hwnd);
            }
            const int idle = game_in_front ? 0 : 1;
            if (idle != idle_state) {
                ShowWindow(overlay, idle ? SW_HIDE : SW_SHOWNOACTIVATE);
                idle_state = idle;
            }
            if (idle) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
        } else if (idle_state != -1) {
            // Switched off again while the overlay was hidden (it is a menu row now): come back,
            // or the window would stay hidden for the rest of the session.
            ShowWindow(overlay, SW_SHOWNOACTIVATE);
            idle_state = -1;
        }

        // Game state. Read through a lambda because the same three values are read again at
        // the end of the frame: everything is committed only when the whole chain resolved,
        // so a failed re-read cannot blank out a pose that was already good.
        addr_t pc = 0, pawn = 0;
        Camera cam;
        auto read_pose = [&]() -> bool {
            World world(&sdk);
            UObject gi = world.OwningGameInstance();
            if (!gi.Addr()) return false;
            auto lps = mem.read_tarray<addr_t>(gi.Addr() + off::ugameinstance::LocalPlayers, 8);
            if (lps.empty()) return false;
            const addr_t pc_now = mem.ptr_at(lps[0] + off::uplayer::PlayerController);
            if (!pc_now) return false;
            const addr_t pawn_now = mem.ptr_at(pc_now + off::aplayercontroller::AcknowledgedPawn);
            Camera cam_now = camx.FromPlayerController(pc_now);
            if (!cam_now.valid) return false;
            pc = pc_now;
            pawn = pawn_now;
            cam = cam_now;
            return true;
        };
        read_pose();

        // A list, a class pattern, a colour or a name rule changed in the menu: rebuild the
        // classifier and drop the per-actor metadata, or the new rule would only reach each actor
        // when it next came up for revalidation - up to a full sweep, which reads as a rule that
        // half applied. The name rules are set here too, so a row typed in the menu takes effect on
        // the very next frame.
        if (fc.rules_dirty) {
            fc.rules_dirty = false;
            fc.apply_rules();
            names.SetRules(cfg.name_rules);
            cache.invalidate_metadata();
        }
        // "Reload aion.ini": re-read the file and re-derive everything that came out of it, so a
        // monster name can be added mid-session. The metadata cache is dropped too, or the new
        // rules would only reach each actor when it next came up for revalidation - up to a full
        // sweep, ~150 frames with the validation budget - which reads as a reload that half worked.
        if (cfg.reload_config) {
            cfg.reload_config = false;
            const std::string again = ResolveDataFile("aion.ini");
            cfg.Load(again);  // resets first: the file is now the truth
            cfg.names_file = ResolveDataFile(cfg.names_file);
            fc.apply_rules();
            cache.invalidate_metadata();
            names.Load(cfg.names_file);
            // The name rules come back with the rest of the file. An ini that has lost the key -
            // it is older than the key, or the user deleted the line - falls back on the rows the
            // file still holds, the way the start of the session does.
            if (!cfg.name_rules_seen) cfg.name_rules = NameTable::ImportRules(cfg.names_file);
            names.SetRules(cfg.name_rules);
            menu.on_font_changed();
            const std::size_t nboss = cache.rules().boss.size();
            const std::size_t ntarget = cache.rules().target.size();
            const bool restart_only = cfg.overlay_dcomp != fc.dcomp_at_start ||
                                      cfg.overlay_vsync != fc.vsync_at_start;
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                          "reloaded: %zu boss, %zu target, %zu name(s), %zu rule(s)%s", nboss,
                          ntarget, names.Size(), cfg.name_rules.size(),
                          restart_only ? "; dcomp/vsync need a restart" : "");
            menu.SetStatus(msg);
            AION_INFO("config reloaded: %s (list_boss='%s', %zu boss entr%s, %zu target entr%s, "
                      "names=%zu, rules=%zu)",
                      again.c_str(), cfg.list_boss.c_str(), nboss, nboss == 1 ? "y" : "ies",
                      ntarget, ntarget == 1 ? "y" : "ies", names.Size(), cfg.name_rules.size());
        }
        // "Save settings": write the settings back into aion.ini, in place. Only `key=value` lines
        // are touched, so the comments and the file's own order survive - the alternative (regenerate
        // the file from the code) would throw away the documentation the file is made of. A file
        // that has no aion.ini yet gets one, with the keys it does not know about.
        if (cfg.save_config) {
            cfg.save_config = false;
            const std::string path = ResolveDataFile("aion.ini");
            const int written = cfg.Save(path);
            char msg[256];
            if (written < 0)
                std::snprintf(msg, sizeof(msg), "could not write %s", path.c_str());
            else
                std::snprintf(msg, sizeof(msg), "saved %d setting(s) to %s", written, path.c_str());
            menu.SetStatus(msg);
            AION_INFO("settings saved: %s (%d keys)", path.c_str(), written);
        }
        if (cfg.dump_classes) {
            cfg.dump_classes = false;
            menu.SetStatus(DumpActorClasses(sdk, cache));
        }
        // Soak mode: report health every second so AC handle-stripping is visible.
        if (cfg.esp_debug || opt.run_seconds > 0) {
            static std::chrono::steady_clock::time_point last_log = loop_start;
            const auto now2 = std::chrono::steady_clock::now();
            const double window_ms =
                std::chrono::duration<double, std::milli>(now2 - last_log).count();
            if (window_ms >= 1000.0) {
                last_log = now2;
                // The window is never exactly a second, so derive the rate from the real one
                // instead of reporting the frame count as if it were.
                last_fps = frames_since_log * 1000.0 / window_ms;
                const auto secs =
                    std::chrono::duration_cast<std::chrono::seconds>(now2 - loop_start).count();
                const auto& st = gfx.stats();
                const auto& dg = cache.diag();
                AION_INFO("t=%llds cam_valid=%d entities=%zu listed=%zu validated=%zu culled=%zu "
                          "pc=0x%llX pawn=0x%llX reopens=%u "
                          "fps=%d update=%.2fms reproj=%.2fms draw=%.2fms flush=%.2fms "
                          "present=%.2fms wait=%.2fms region=%dx%d verts=%d",
                          (long long)secs, (int)cam.valid, cache.Entities().size(), dg.listed,
                          dg.validated, dg.culled, (unsigned long long)pc,
                          (unsigned long long)pawn, mem.reopen_count(), frames_since_log,
                          ms_update, ms_reproj, ms_draw, st.flush_ms, st.present_ms, st.wait_ms,
                          st.dirty_w, st.dirty_h, st.verts);
                frames_since_log = 0;
            }
        }

        gfx.BeginFrame();
        // Labels follow the frame. The atlas is rasterized at exactly the size it is drawn at, so a
        // client whose size changed (a windowed client resized, a resolution or scaling switch)
        // needs a new atlas rather than a resampled one - and this is also what applies the menu's
        // "scale with resolution" toggle, without a second code path doing it. The comparison is
        // one int per frame; the rasterize happens only on a real change.
        {
            const int want_px = EspFontPx(cfg, gfx.Height());
            if (want_px != gfx.font_config().esp_px) {
                FontConfig f = gfx.font_config();
                f.esp_px = want_px;
                gfx.SetFontConfig(f);
                AION_INFO("labels re-rasterized for a %dx%d client: %d px", gfx.Width(),
                          gfx.Height(), want_px);
            }
        }
        auto t_phase0 = std::chrono::steady_clock::now();
        auto t_update_end = t_phase0;   // the update phase ends before the reprojection, or the HUD's
        auto t_phase1 = t_phase0;       // total would count the reprojection twice
        if (cam.valid) {
            proj.SetViewport(gfx.Width(), gfx.Height());
            // Never overwrite a good pawn with 0: AcknowledgedPawn reads 0 across a zone change or a
            // respawn, and a 0 here drops the LocalPlayer flag for as long as it takes the chain to
            // resolve again - which re-bases every distance on the camera and drops the "character"
            // snapline origin to the bottom edge.
            if (pawn) cache.set_local_pawn(pawn);
            // Capture mode is a census of every actor, so nothing is culled there.
            cache.UpdateBulk(&sdk, cam, proj, cfg.hide_dead, !opt.capture_mode);
            t_update_end = std::chrono::steady_clock::now();
            // The transforms were just read, one syscall per actor, so the camera read
            // before them is that whole pass old. Re-read it and re-project: the frame is
            // then drawn against the newest camera instead of one that trails it by the
            // read time, which is visible as boxes swinging late when the camera turns.
            {
                const auto t_rep0 = std::chrono::steady_clock::now();
                if (read_pose()) cache.Reproject(proj, cam);
                ms_reproj = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - t_rep0).count();
            }
            t_phase1 = std::chrono::steady_clock::now();
            const int drawn = last_drawn =
                EspRenderer().RenderEntities(gfx, cam, cache.Entities(), cfg);
            if (cfg.auto_dodge && !menu.Open()) {
                bool hp_low = false;
                bool hp_low_known = false;
                for (const auto& e : cache.Entities()) {
                    if (!e.LocalPlayer) continue;
                    hp_low = e.LowHealth;
                    hp_low_known = e.LowHealthKnown;
                    break;
                }
                FVector dodge_origin = cam.Location;
                for (const auto& e : cache.Entities()) {
                    if (e.LocalPlayer) {
                        dodge_origin = e.Location;
                        break;
                    }
                }
                AutoDodge::TickArgs ad;
                ad.cfg = &cfg;
                ad.sdk = &sdk;
                ad.refl = &refl;
                ad.local_pawn = pawn;
                ad.origin = dodge_origin;
                ad.menu_open = menu.Open();
                ad.game_in_front = GameProcessInForeground(pid, game_hwnd, overlay);
                ad.game_pid = pid;
                ad.game_hwnd = game_hwnd;
                ad.overlay_hwnd = overlay;
                ad.hp_low = hp_low;
                ad.hp_low_known = hp_low_known;
                auto_dodge.Tick(ad);
            }
            // The local player's own HP/MP, when asked for: read once per frame (a handful of reads
            // through the client's own HUD bar, see SelfBars.hpp) and drawn from the last known value,
            // so a frame that failed to read shows what was there before with a '?' rather than
            // dropping to 0 - which is the one thing this must never do.
            if (cfg.self_vitals) {
                static SelfVitalsReader vitals_reader;
                static SelfVitals vitals_last;
                static bool vitals_noted = false;
                const SelfVitals now_v = vitals_reader.Read(&sdk, /*search=*/false);
                if (now_v.hp_live) vitals_last.hp = now_v.hp;
                if (now_v.mp_live) vitals_last.mp = now_v.mp;
                vitals_last.hp_live = now_v.hp_live;
                vitals_last.mp_live = now_v.mp_live;
                vitals_last.found = now_v.found;
                // The bit the client DOES expose: its own low-health warning on the local pawn, read
                // with the rest of the frame (see EntityCache). Cleared first, so a frame with no
                // local pawn in the list reports unknown rather than the last value it saw.
                vitals_last.hp_low_known = false;
                for (const auto& e : cache.Entities()) {
                    if (!e.LocalPlayer) continue;
                    vitals_last.hp_low = e.LowHealth;
                    vitals_last.hp_low_known = e.LowHealthKnown;
                    break;
                }
                SelfBarsRenderer().Render(gfx, cfg, vitals_last);
                // Say WHY the exact values are unknown, once: a panel stuck on "unknown" with no
                // explanation is the failure mode this contract exists to avoid, and the reason is a
                // live fact about the client (see build/self_hpmp_research.md).
                if (!now_v.found && !vitals_noted) {
                    vitals_noted = true;
                    AION_INFO("self HP/MP: exact values unavailable: %s",
                              vitals_reader.last_note().c_str());
                }
            }
            if (cfg.esp_debug) {
                // The panel carries what the old one-line print carried (entity count, FOV,
                // camera position) plus the frame budget and the read-path counters, which are
                // what actually explain a late frame.
                const auto& st = gfx.stats();
                const auto& dg = cache.diag();
                HudStats hs;
                hs.fps = last_fps;
                hs.update_ms = ms_update;
                hs.reproj_ms = ms_reproj;
                hs.draw_ms = ms_draw;
                hs.flush_ms = st.flush_ms;
                hs.present_ms = st.present_ms;
                hs.present_wait_ms = st.wait_ms;
                hs.verts = st.verts;
                hs.listed = dg.listed;
                hs.kept = cache.Entities().size();
                hs.culled = dg.culled;
                hs.validated = dg.validated;
                hs.drawn = drawn;
                hs.handle_reopens = mem.reopen_count();
                hs.stripped = mem.stripped_count();
                hs.cam_valid = cam.valid;
                hs.fov = cam.FOV;
                hs.cam_x = cam.Location.X;
                hs.cam_y = cam.Location.Y;
                hs.cam_z = cam.Location.Z;
                hs.dcomp = gfx.using_dcomp();
                hs.width = gfx.Width();
                hs.height = gfx.Height();
                hs.display_hz = fc.display_hz;
                DrawHud(gfx, cfg, hs);
            }
        } else {
            // No camera this frame: nothing was read and nothing was re-projected, so the
            // reprojection phase is zero rather than the previous frame's value - a stale number
            // there reads as a frame that paid for a reprojection it never ran.
            ms_reproj = 0.0;
            t_update_end = t_phase1 = std::chrono::steady_clock::now();
            gfx.Text(10, 10, "waiting for local player...", Color::RGBA(255, 200, 0), 1.f);
        }
        // The menu's numbers, copied once per frame from values the frame already computed: the
        // panes then cost nothing but drawing.
        {
            MenuStats ms;
            ms.fps = last_fps;
            ms.update_ms = ms_update;
            ms.reproj_ms = ms_reproj;
            ms.draw_ms = ms_draw;
            const auto& st = gfx.stats();
            ms.flush_ms = st.flush_ms;
            ms.present_ms = st.present_ms;
            ms.present_wait_ms = st.wait_ms;
            ms.verts = st.verts;
            const auto& dg = cache.diag();
            ms.listed = (int)dg.listed;
            ms.kept = (int)cache.Entities().size();
            ms.culled = (int)dg.culled;
            ms.validated = (int)dg.validated;
            ms.garbage = (int)dg.garbage;
            ms.drawn = last_drawn;
            ms.handle_reopens = mem.reopen_count();
            ms.stripped = mem.stripped_count();
            ms.names_learned = (int)names.Size();
            for (const auto& rule : cfg.name_rules) {
                if (rule.mode == Config::NameRule::Mode::Forced)
                    ++ms.names_forced;
                else if (rule.mode == Config::NameRule::Mode::Hidden)
                    ++ms.names_hidden;
            }
            ms.cam_valid = cam.valid;
            ms.dcomp = gfx.using_dcomp();
            ms.width = gfx.Width();
            ms.height = gfx.Height();
            ms.display_hz = fc.display_hz;
            ms.fov = (float)cam.FOV;
            ms.cam_x = (float)cam.Location.X;
            ms.cam_y = (float)cam.Location.Y;
            ms.cam_z = (float)cam.Location.Z;
            for (const auto& e : cache.Entities()) {
                ++ms.per_category[(int)e.Type];
                if (e.OnScreen) ++ms.per_category_onscreen[(int)e.Type];
                if (e.Target) ++ms.targets;
            }
            {
                const AutoDodgeStats ads = auto_dodge.Stats();
                ms.auto_dodge_fires = ads.fires;
                ms.auto_dodge_ms_since = ads.ms_since_last;
                ms.auto_dodge_last = ads.last_reason;
                ms.auto_dodge_proj = ads.telegraph_projectiles;
                ms.auto_dodge_ui = ads.telegraph_ui;
                ms.auto_dodge_anim = ads.telegraph_anim;
                ms.auto_dodge_input = ads.last_input_backend;
            }
            menu.set_stats(ms);
        }
        // Only while open: a closed menu costs nothing, not even ImGui's own bookkeeping. The
        // drawing itself happens later, inside D3D11Overlay::EndFrame, through the layer's
        // pre-present hook - that is what puts both passes in the same surface.
        if (menu.Open() && imgui.Active()) {
            // Run on the frame after the click, before BeginFrame reclaims focus for the menu.
            if (auto_dodge.HasPendingTest()) {
                imgui.DeferForegroundReclaim(4500);
                FVector dodge_origin = cam.Location;
                if (cam.valid) {
                    for (const auto& e : cache.Entities()) {
                        if (e.LocalPlayer) {
                            dodge_origin = e.Location;
                            break;
                        }
                    }
                }
                AutoDodge::TickArgs ad;
                ad.cfg = &cfg;
                ad.sdk = &sdk;
                ad.refl = &refl;
                ad.local_pawn = pawn;
                ad.origin = dodge_origin;
                ad.menu_open = true;
                ad.game_in_front = GameProcessInForeground(pid, game_hwnd, overlay);
                ad.game_pid = pid;
                ad.game_hwnd = game_hwnd;
                ad.overlay_hwnd = overlay;
                const unsigned fires_before = auto_dodge.Stats().fires;
                auto_dodge.Tick(ad);
                const AutoDodgeStats after = auto_dodge.Stats();
                if (after.fires > fires_before) {
                    const int vk = NormalizeDodgeVk(cfg.auto_dodge_vk);
                    const int mv = cfg.auto_dodge_move_vk;
                    char msg[260];
                    const char* via = after.last_input_backend ? after.last_input_backend : "?";
                    std::snprintf(msg, sizeof msg,
                                  "Test sent %d+%d via %s, focus %s. If SendInput: install "
                                  "FakerInput driver (CONFIG hint). Dodge in combat.",
                                  vk, mv, via, after.last_test_focus_ok ? "yes" : "no");
                    menu.SetStatus(msg);
                } else {
                    menu.SetStatus(
                        "Test dodge: set Key (VK), e.g. 32=Space; could not focus game window.");
                }
            }
            imgui.BeginFrame();
            menu.BuildImGui(imgui);
        }
        // Diagnostic: prove the composed output reaches the screen. Covers the whole
        // client area so the result cannot be confused with game imagery.
        // NOTE: Color::RGBA takes 0-255 components.
        if (opt.pattern_mode)
            gfx.FillRect(0.f, 0.f, (float)gfx.Width(), (float)gfx.Height(),
                         Color::RGBA(255, 0, 255));
        if (!menu.Open())
            gfx.Text(10, (float)gfx.Height() - 20, "INSERT = menu", Color::RGBA(160, 160, 160), 1.f);

        ++frame;
        // update and reproj are disjoint here, so the HUD's five phases add up to the frame's own
        // work (they used to overlap by exactly the reprojection, which made the panel read over
        // budget on a frame that was not).
        ms_update = std::chrono::duration<double, std::milli>(t_update_end - t_phase0).count();
        ms_draw = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t_phase1).count();
        if (opt.capture_mode && frame >= opt.capture_frames &&
            (cam.valid || frame >= opt.capture_frames + 600)) {
            gfx.Flush();
            const bool ok = gfx.SaveBackbufferToBmp(opt.capture_path);
            gfx.Present();
            int on_screen = 0, boxable = 0;
            for (const auto& e : cache.Entities()) {
                if (e.OnScreen) ++on_screen;
                if (e.OnScreen && (e.BoundsMax.X - e.BoundsMin.X) >= 2.f &&
                    (e.BoundsMax.Y - e.BoundsMin.Y) >= 2.f)
                    ++boxable;
            }
            AION_INFO("capture '%s' saved=%d entities=%zu on_screen=%d boxable=%d "
                      "cam=(%.0f,%.0f,%.0f) fov=%.1f valid=%d viewport=%dx%d",
                      opt.capture_path.c_str(), (int)ok, cache.Entities().size(), on_screen, boxable,
                      cam.Location.X, cam.Location.Y, cam.Location.Z, cam.FOV, (int)cam.valid,
                      gfx.Width(), gfx.Height());
            int shown = 0;
            // Sized from the enum, not a literal: a category appended to EntityType must not be able
            // to write past the end of this.
            int cat[(int)EntityType::Chest + 1] = {0};
            for (const auto& e : cache.Entities()) {
                cat[(int)e.Type]++;
                if (shown++ >= 12) continue;
                AION_INFO("  ent %-26s class=%-22s model=%-22s %-8s%s d=%.0f on=%d "
                          "scr=(%.1f,%.1f) box=(%.1f,%.1f)-(%.1f,%.1f)",
                          e.Name.c_str(), e.ClassName.c_str(), e.Model.c_str(),
                          EntityTypeName(e.Type), e.Dead ? " DEAD" : "", e.Distance,
                          (int)e.OnScreen, e.Screen.X, e.Screen.Y, e.BoundsMin.X, e.BoundsMin.Y,
                          e.BoundsMax.X, e.BoundsMax.Y);
            }
            AION_INFO("  categories: other=%d player=%d npc=%d pet=%d monster=%d item=%d "
                      "gather=%d interact=%d object=%d noise=%d portal=%d cube=%d chest=%d",
                      cat[0], cat[1], cat[2], cat[3], cat[4], cat[5], cat[6], cat[7], cat[8],
                      cat[9], cat[10], cat[11], cat[(int)EntityType::Chest]);
            const auto& dg = cache.diag();
            AION_INFO("  cache: listed=%zu cached=%zu validated=%zu kept=%zu culled=%zu "
                      "noise=%zu read_fail=%zu name_fail=%zu root_fail=%zu",
                      dg.listed, dg.cached, dg.validated, dg.kept, dg.culled, dg.noise,
                      dg.read_fail, dg.name_fail, dg.root_fail);
            break;
        }
        gfx.EndFrame();

        ++frames_since_log;
        // No artificial pacing by default (fc.present_fps < 0): the ESP presents as fast as the
        // pipeline allows, so the frame DWM composites is never older than one iteration. A rate
        // from aion.ini still paces it here - >0 that many per second, 0 the display's rate.
        if (fc.present_fps > 0) {
            const double spent = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - frame_start).count();
            const double target = 1000.0 / fc.present_fps;
            const double rest = target - spent - 0.7;  // SleepFor overshoots slightly
            if (rest > 0.2)
                std::this_thread::sleep_for(
                    std::chrono::duration<double, std::milli>(rest));
        }
    }

    // One summary at exit. The per-second line reports the running state; this reports what the
    // session was worth, and it is the only block a session ends with.
    {
        const double secs =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - loop_start).count();
        const auto& dg = cache.diag();
        AION_INFO("session: %.1fs frames=%d fps=%.1f listed=%zu kept=%zu culled=%zu reopens=%u (%s)",
                  secs, frame, last_fps, dg.listed, cache.Entities().size(), dg.culled,
                  mem.reopen_count(), soak_done ? "soak complete" : "window closed");
    }
}

}  // namespace app
}  // namespace aion
