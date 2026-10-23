// external_main.cpp — AION2 external ESP overlay (no injection).
//
//  * attaches read-only to AION2.exe, drives the UE layer over ReadProcessMemory
//  * owns a layered, click-through, topmost D3D11 window aligned to the game client
//  * INSERT toggles the menu; arrows navigate; ENTER/LEFT/RIGHT adjust
//
// This file is the orchestrator and nothing else: it loads the config, parses the command line,
// hands the headless modes to src/app/Modes.cpp and then runs the frame loop in src/app/Frame.cpp.
// What it drives lives in three layers, and each one is a directory:
//
//   include/aion   the engine layer: the client read, the reflection/name layer, the entity
//                  pipeline, the classifier, the renderer, the config
//   src/app        the application layer: the Win32 surface, the launch identity, the process
//                  attach, the command line, the headless modes, the frame loop
#include <windows.h>
#include <mmsystem.h>

#include <cstdint>
#include <string>

#include "app/Cli.hpp"
#include "app/Client.hpp"
#include "app/Frame.hpp"
#include "app/Launch.hpp"
#include "app/Modes.hpp"
#include "app/Window.hpp"
#include "aion/AutoDodge.hpp"
#include "aion/Config.hpp"
#include "aion/Reflection.hpp"
#include "aion/DataFile.hpp"
#include "aion/DefaultIni.hpp"
#include "aion/EntityCache.hpp"
#include "aion/ImGuiLayer.hpp"
#include "aion/Logger.hpp"
#include "aion/Menu.hpp"
#include "aion/Memory.hpp"
#include "aion/Obf.hpp"
#include "aion/Overlay.hpp"
#include "aion/Projection.hpp"
#include "aion/RealNames.hpp"
#include "aion/Sdk.hpp"
#include "aion/World.hpp"

#pragma comment(lib, "winmm.lib")
// OpenProcessToken/GetTokenInformation (the elevation report) come from advapi32.
#pragma comment(lib, "advapi32.lib")

using namespace aion;

namespace aion {
int RunOverlay(int argc, char** argv);  // the entry point entry_obf.cpp calls
}  // namespace aion

int aion::RunOverlay(int argc, char** argv) {
    // The name on disk changes first, so everything below - including the banner - reports the
    // name the process is really running under.
    const app::LaunchName launch = app::RenameSelf();
    if (!launch.Renamed)
        AION_WARN("could not rename the exe: %s", launch.Note.c_str());

    // 1 ms sleep granularity. Without this, Sleep() rounds up to the 15.6 ms system
    // timer quantum and the frame pacing below would run at ~30 fps.
    timeBeginPeriod(1);
    Config& cfg = Config::Instance();
    const std::string cfg_path = ResolveDataFile("aion.ini");
    // A fresh copy has no aion.ini: write the documented defaults next to the exe, so the file to
    // edit exists before anything looks for a setting in it. An existing file is never touched.
    if (WriteDefaultIni(cfg_path))
        AION_INFO("no config found: wrote the defaults to %s", cfg_path.c_str());
    cfg.Load(cfg_path);
    cfg.names_file = ResolveDataFile(cfg.names_file);
    // Fixed when the window and swapchain are created, so a reload cannot apply them - it says so
    // in the status line instead of appearing to have worked.
    const bool dcomp_at_start = cfg.overlay_dcomp;
    const bool vsync_at_start = cfg.overlay_vsync;
    // The command line is read before the header, so a session can ask for its own verbosity and
    // have it apply to the first line printed. Every headless mode is selected out of this one
    // struct (src/app/Cli.cpp); --menu is applied further down, where the menu exists.
    const app::Options opt = app::ParseArgs(argc, argv);
    Logger::Instance().SetLevel(opt.log_level_set ? opt.log_level : LogLevel::Info);
    if (cfg.log_to_file) Logger::Instance().OpenFile(ResolveDataFile(LogFileName()));
    app::PrintBanner(launch, cfg_path);

    // The client: the instance whose runtime objects are live, not merely the first pid that
    // matches the image name. src/app/Client.cpp owns that search and its diagnostics.
    AION_SECTION("target");
    std::uint32_t pid = 0;
    if (const int rc = app::AttachToClient(cfg, pid)) return rc;
    ExternalMemory mem;
    if (!mem.Attach(pid)) return 2;
    Sdk sdk;
    if (!sdk.WaitReady(&mem, 0)) { AION_ERROR("SDK init failed"); return 3; }
    AION_DEBUG("attached pid=%u base=0x%llX", pid, (unsigned long long)sdk.base);

    AION_SECTION("initialization");
    // The client's window, its rect and its DPI model are all needed before our own window exists:
    // the DPI model is a once-per-process choice (see app::AdoptClientDpiModel), and the initial
    // surface - and therefore the label size rasterized for it - comes from the client's rect
    // instead of a placeholder the first frame would throw away.
    HWND client_hwnd = app::FindGameWindow(pid);
    RECT client_rc{};
    const bool have_client_rc = app::GameClientRect(client_hwnd, client_rc);
    app::AdoptClientDpiModel(client_hwnd);
    const int client_h = have_client_rc ? (client_rc.bottom - client_rc.top) : 1080;

    // Overlay window: layered + transparent + click-through + topmost. The class, the extended
    // styles and the placement on the client live in src/app/Window.cpp.
    HWND overlay = app::CreateOverlayWindow(have_client_rc ? &client_rc : nullptr,
                                            !app::Headless(opt));
    if (!overlay) { AION_ERROR("overlay window creation failed"); return 4; }

    AION_SECTION("overlay");
    D3D11Overlay gfx;
    gfx.set_use_dcomp(cfg.overlay_dcomp);
    if (!gfx.InitWindow(overlay, app::FontsFromConfig(cfg, client_h))) {
        AION_ERROR("overlay D3D11 init failed");
        return 5;
    }
    // The menu is built on the overlay's own device and target. It is a convenience on top of the
    // ESP, so a failure here is a warning and the session continues without it.
    ImGuiLayer imgui;
    if (!imgui.Init(overlay, gfx)) {
        AION_WARN("menu unavailable (%s); the ESP runs without it", imgui.last_error().c_str());
    } else {
        app::SetImGuiLayer(&imgui);
        imgui.RebuildFonts(cfg.font_face, cfg.menu_font_px);
        AION_INFO("menu: ImGui %s, %s %dpx, display scale %.2f", imgui.version().c_str(),
                  cfg.font_face.c_str(), cfg.menu_font_px, imgui.display_scale());
    }
    // The display's own refresh rate is the overlay's natural rate. GetSystemMetrics cannot report
    // it, but the primary display's current mode can. This matters because the ESP is drawn in our
    // own window: at 60 fps on a 144 Hz panel each box is up to 16.7 ms behind a world that moves
    // every 6.9 ms, which is what trailing boxes look like when the camera turns.
    // aion.ini's present_fps overrides: >0 an explicit rate, 0 (default) follow the display,
    // <0 present every frame.
    int display_hz = 0;
    {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsExW(nullptr, ENUM_CURRENT_SETTINGS, &dm, 0) &&
            dm.dmDisplayFrequency > 1)
            display_hz = static_cast<int>(dm.dmDisplayFrequency);
    }
    const int present_fps = app::ResolvePresentFps(cfg, display_hz);
    // Frame loop owns pacing; always present each EndFrame (no second cap in ULW path).
    gfx.set_present_fps(0);
    gfx.set_vsync(cfg.overlay_vsync);
    AION_INFO("present rate: %s (display %d Hz, config present_fps=%d)",
              present_fps > 0 ? (std::to_string(present_fps) + " fps").c_str() : "every frame",
              display_hz, cfg.present_fps);
    // Every session records the settings that decide behaviour, so a feature that looks absent
    // (a toggle left off, the wait too short) is visible in the log instead of having to be
    // inferred from silence.
    // Whether we can open a protected client at all is decided by our token, so it belongs on the
    // record with the other settings: err=5 on every candidate is the signature of a non-elevated
    // overlay meeting the anti-cheat.
    bool elevated = false;
    {
        HANDLE tok = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
            TOKEN_ELEVATION el{};
            DWORD n = 0;
            if (GetTokenInformation(tok, TokenElevation, &el, sizeof el, &n))
                elevated = el.TokenIsElevated != 0;
            CloseHandle(tok);
        }
    }
    // aion_boss.txt / aion_target.txt are retired: their patterns are added to the ini's own
    // list_boss / list_target here and then saved, so the lists have one home - the ini the menu
    // edits - and a file cannot drift from it. The files are left on disk untouched and are never
    // read again, and an entry that is already in the list is not added twice, so this import costs
    // one write, once.
    std::size_t imported_lists = 0;
    {
        const struct { const char* File; std::string* List; } kRetired[] = {
            {"aion_boss.txt", &cfg.list_boss},
            {"aion_target.txt", &cfg.list_target},
        };
        for (const auto& r : kRetired) {
            const std::vector<std::string> pats = ReadPatternFile(ResolveDataFile(r.File));
            std::vector<std::string> have = SplitList(*r.List);
            // Equal length plus HasI is equality ignoring case, the way the lists themselves are
            // matched: an entry that only differs in case is already there, and appending it would
            // make this import write the ini again on every single start.
            const auto have_entry = [&have](const std::string& p) {
                for (const auto& e : have)
                    if (e.size() == p.size() && HasI(e, p.c_str())) return true;
                return false;
            };
            for (const auto& p : pats) {
                // list_boss / list_target are comma separated, so a pattern holding a comma cannot
                // be represented; say so instead of letting it read back as two entries.
                if (p.find(',') != std::string::npos) {
                    AION_WARN("config: '%s' entry '%s' holds a comma, which list_boss / list_target "
                              "cannot carry - skipped", r.File, p.c_str());
                    continue;
                }
                if (have_entry(p)) continue;
                if (!r.List->empty()) *r.List += ",";
                *r.List += p;
                have.push_back(p);
                ++imported_lists;
            }
        }
    }
    // The config path is in the header; this line is what the file decided, so a setting that looks
    // absent (a toggle left off, the wait too short) is visible instead of inferred from silence.
    AION_INFO("settings: list_boss='%s' list_target='%s' dcomp=%d vsync=%d wait=%ds elevated=%d",
              cfg.list_boss.c_str(), cfg.list_target.c_str(), (int)cfg.overlay_dcomp,
              (int)cfg.overlay_vsync, cfg.sdk_wait_seconds, (int)elevated);
    // Evidence for the click-through requirement: the styles the window ended up with
    // (the ULW fallback adds WS_EX_LAYERED) and the hit-test answer it will give.
    AION_INFO("overlay window ex=0x%08lX style=0x%08lX nchittest=%s",
              (unsigned long)GetWindowLongPtrA(overlay, GWL_EXSTYLE),
              (unsigned long)GetWindowLongPtrA(overlay, GWL_STYLE),
              SendMessageA(overlay, WM_NCHITTEST, 0, 0) == HTTRANSPARENT ? "HTTRANSPARENT"
                                                                        : "OPAQUE");


    // Headless font inspection: rasterize the atlas, save it and exit. Called before the menu and
    // the entity cache exist, so it takes only the overlay it draws with.
    if (const int rc = app::SaveFontAtlas(gfx, overlay, mem, opt.atlas_path); rc >= 0) return rc;
    Menu menu(&cfg);
    // The session column's facts. All of this is known once the client is attached and none of it
    // changes afterwards, so it is set once here rather than copied into the menu every frame.
    {
        MenuSession session;
        const std::string exe = TargetProcessName();
        const std::size_t dot = exe.find('.');
        session.game = dot == std::string::npos ? exe : exe.substr(0, dot);
        session.process = exe;
        session.pid = pid;
        char base[32];
        std::snprintf(base, sizeof base, "0x%llX", (unsigned long long)sdk.base);
        session.module = base;
        char image[48];
        std::snprintf(image, sizeof image, "%.1f MB, verified",
                      (double)off::build::VerifiedExeSize / (1024.0 * 1024.0));
        session.image = image;
        session.elevated = elevated;
        session.attached = true;
        menu.set_session(session);
    }
    menu.on_font_changed = [&] { gfx.SetFontConfig(app::FontsFromConfig(cfg, gfx.Height())); };
    // The three verbs are the flags the loop already acts on, so the menu drives the same code the
    // keyboard rows did rather than a second implementation of it. A rule change is a flag too: it
    // has to be applied where the classifier lives.
    menu.on_save_ini = [&] { cfg.save_config = true; };
    menu.on_reload_ini = [&] { cfg.reload_config = true; };
    menu.on_dump_classes = [&] { cfg.dump_classes = true; };
    AutoDodge auto_dodge;
    menu.on_auto_dodge_test = [&] { auto_dodge.RequestTest(); };
    bool rules_dirty = false;
    menu.on_rules_changed = [&] { rules_dirty = true; };
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--menu") menu.Toggle();
    Reflection refl(&sdk);
    CameraExtractor camx(&sdk);
    Projection proj;
    EntityCache cache;
    // The picker's snapshot: aggregated by model, taken when the user asks for it. The cache holds
    // exactly "what the overlay saw", so the rows carry the name the client shows next to the class
    // and model behind it - no guessing at class names.
    menu.on_refresh_actors = [&] {
        std::vector<ActorSighting> rows;
        for (const auto& e : cache.Entities()) {
            if (e.Type == EntityType::Noise) continue;
            ActorSighting* found = nullptr;
            for (auto& r : rows) {
                if (r.model == e.Model && r.label == e.Name && r.cls == e.ClassName) {
                    found = &r;
                    break;
                }
            }
            if (found) {
                ++found->live;
                continue;
            }
            ActorSighting s;
            s.label = e.Name;
            s.model = e.Model;
            s.cls = e.ClassName;
            s.category = EntityTypeName(e.Type);
            s.live = 1;
            rows.push_back(std::move(s));
        }
        const std::size_t n = rows.size();
        menu.set_actor_list(std::move(rows));
        char msg[96];
        std::snprintf(msg, sizeof msg, "%zu kind%s in range", n, n == 1 ? "" : "s");
        menu.SetStatus(msg);
    };
    // One mapping from config to classifier, shared with the reload action below; the dump lambda
    // keeps reading the same `rules`, so it always reports what the classifier is actually using.
    ClassRules rules;
    auto apply_rules = [&] {
        rules = RulesFromConfig(cfg);
        cache.set_rules(rules);
    };
    apply_rules();
    // Learned names (RealNames.hpp). The table is loaded before the first frame so everything
    // learned in earlier sessions is already in use, and it is saved whenever it grows.
    NameTable names;
    names.Load(cfg.names_file);
    // The user's own name rules are the ini's now (name_rules), not the rows of aion_names.txt. An
    // ini written before that key existed still has them in those rows, so they are read out of the
    // file once here and written into the ini: after that the file is only the learned cache, and
    // the rules are typed - and deleted - in the menu.
    bool imported_rules = false;
    if (!cfg.name_rules_seen) {
        cfg.name_rules = NameTable::ImportRules(cfg.names_file);
        imported_rules = !cfg.name_rules.empty();
    }
    names.SetRules(cfg.name_rules);
    cache.set_name_table(&names);
    if (names.direct_offset())
        AION_INFO("names: %zu learned name(s) from '%s', direct field actor+0x%X (%s)",
                  names.Size(), cfg.names_file.c_str(),
                  static_cast<unsigned>(names.direct_offset()),
                  NameFieldKindName(names.direct_kind()));
    else if (names.Size())
        AION_INFO("names: %zu learned name(s) from '%s'", names.Size(), cfg.names_file.c_str());
    // A start with nothing learned yet is the normal state until a nameplate is seen, and
    // "0 learned" printed at INFO reads as a failure - so it is DEBUG: in the log with -v,
    // absent in a normal run.
    else
        AION_DEBUG("names: nothing learned yet ('%s' absent or empty)", cfg.names_file.c_str());
    // A rule is a deliberate instruction, so say how many are in force: a run that silently ignored
    // them would be indistinguishable from one that never read them.
    if (names.AutoRules() || names.ForcedRules() || names.HiddenRules())
        AION_INFO("names: %zu rule(s) from the ini's name_rules (%zu forced, %zu live, %zu hidden)",
                  names.AutoRules() + names.ForcedRules() + names.HiddenRules(), names.ForcedRules(),
                  names.AutoRules(), names.HiddenRules());
    // Both one-time imports (the retired list files, the name rules) are written here, in the one
    // save, so the next start has nothing left to migrate.
    if (imported_lists || imported_rules) {
        const int written = cfg.Save(cfg_path);
        AION_INFO("config: imported %zu list pattern(s) and %zu name rule(s) into '%s' (saved %d "
                  "keys)",
                  imported_lists, imported_rules ? cfg.name_rules.size() : 0, cfg_path.c_str(),
                  written);
    }

    // The headless modes: each answers one question about the client's own state and exits. The
    // dispatcher lives in src/app/Modes.cpp; -1 means none was selected, and the session runs on
    // into the frame loop. Every mode shuts the overlay down itself, so a mode never returns here.
    {
        app::ModeContext mc{sdk, mem, cache, names, gfx, overlay, opt};
        if (const int rc = app::RunDumpModes(mc); rc >= 0) return rc;
    }

    AION_INFO("running. INSERT toggles the menu.");

    // The frame loop: read -> update the entity list -> republish the camera -> draw it.
    // src/app/Frame.hpp states the order and why it matters.
    app::FrameContext frame_ctx{sdk,  mem,         cfg,       camx,        proj,
                                cache, names,      menu,      auto_dodge,  refl,
                                imgui, gfx,        overlay,   pid,         opt,
                                present_fps, display_hz, dcomp_at_start, vsync_at_start,
                                rules_dirty, apply_rules};
    AION_SECTION("session");
    app::RunFrameLoop(frame_ctx);

    // Whatever the nameplates taught this session is kept for the next one.
    names.Save();
    imgui.Shutdown();
    app::SetImGuiLayer(nullptr);
    gfx.Shutdown();
    DestroyWindow(overlay);
    mem.Close();
    timeEndPeriod(1);
    return 0;
}
