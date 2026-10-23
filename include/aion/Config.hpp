// Config.hpp — runtime configuration and feature toggles.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>


namespace aion {

// Where the snapline to a target starts. Bottom and Top are the midpoint of that screen edge;
// Player is your own character, so the line stays attached to the pawn while the camera pans - an
// edge-anchored line slides sideways with the view and reads as detached from the character it is
// supposed to come from. Stored as an int because the menu edits settings in place (Item::IntVal).
enum SnaplineMode { SnaplineOff = 0, SnaplineBottom = 1, SnaplinePlayer = 2, SnaplineTop = 3 };

struct Config {
    // Shipped draw defaults: master on, and boxes, names, distance, class and snaplines all
    // on - the labels are what makes the boxes readable, so an untouched install shows the
    // full picture at once. There is no skeleton or health setting: the renderer has no bone or
    // health source, and a row that draws nothing is worse than no row.
    bool esp_enabled = true;
    bool esp_box = true;
    // Box outline: a full rectangle, or an L-shaped bracket at each corner. The brackets
    // leave the middle of every side open, so the box stops competing with the character's
    // silhouette and with its own labels while still marking all four extents.
    bool esp_box_corners = true;
    double esp_corner_frac = 0.22;  // bracket arm length, as a share of the short side
    bool esp_name = true;
    bool esp_distance = true;
    bool esp_class = true;
    // Markers for entities the camera is not showing. The ESP is a screen-space projection, so a
    // point behind the camera has no screen position at all and a point outside the viewport has no
    // visible pixels; both used to be dropped, which is what made an enemy or a gathering node ten
    // metres behind you invisible until you turned. Each one that is close enough gets an arrow
    // pinned to the screen border in its direction, labelled like its box would have been.
    bool esp_offscreen = true;
    // How far those markers reach, in metres, from your character. Shorter than max_distance_m on
    // purpose: a marker needs no aim and is read without turning, so at 200 m the frame would be
    // nothing but arrows.
    double offscreen_range_m = 60.0;
    // Draw a snapline to each off-screen marker, from the same origin the box snaplines use
    // (esp_snapline: bottom, top or player). The arrow says which way to turn; the line is what
    // makes one marker findable when several share an edge. Needs esp_snapline set to anything
    // other than off.
    bool esp_offscreen_snapline = true;
    // Snapline origin; see SnaplineMode. Player, not the bottom edge: the line then follows the
    // character instead of sliding sideways with the camera.
    int snapline = SnaplinePlayer;
    // The hunting list as a category of its own: paint the actors on list_target in the drop colour
    // and keep them drawn even when their own category is off. The list itself is filled in CONFIG.
    bool esp_target = true;
    bool esp_debug = false;
    // Box/snapline/ring stroke width in pixels. The stroke is feathered (see
    // D3D11Overlay::Line), so a value below 1 renders that fraction of the colour: the old 0.20
    // default drew the boxes at a fifth of their colour, which reads as
    // invisible on a bright screen. 1.0 is a solid pixel. Go lower only for a deliberately faint
    // overlay.
    double esp_thickness = 1.0;

    // Skip the local player's own character: it is the one actor the user is standing inside.
    bool filter_local_player = true;
    // Range limit in metres, measured from the local pawn (not the camera), which is the
    // unit the client itself shows - so the number here matches what a target reads in the
    // world instead of being offset by however far the camera is zoomed out.
    double max_distance_m = 200.0;
    int max_entities_drawn = 512;

    // Actors the client is not rendering. AActor::bHidden is NOT that flag, measured with
    // --dump-gates: the client sets it on actors it IS drawing (an NPC at 34 m read bHidden=1 while
    // the renderer's own bRecentlyRendered read 1), and characters past 128 m read bHidden=0 while
    // nothing was drawn past 45 m. So the gate is "bHidden AND the renderer did not draw it", which
    // keeps the on-screen actors the old rule threw away. Off: those actors are not drawn. On: they
    // are drawn in color_hidden out to hidden_range_m, reaching past what the client
    // itself shows.
    bool esp_hidden = false;
    double hidden_range_m = 600.0;

    // ESP text size (1.0 = the atlas' native pixel size, i.e. drawn 1:1).
    double esp_text_scale = 1.0;
    // Text rendering. The atlas is rasterized at exactly these pixel sizes, so text
    // is never resampled - raise esp_font_px on high-DPI displays.
    int esp_font_px = 14;  // font em height for ESP labels at a 1080p-tall client
    // Follow the client's height with the label size. Because the atlas is rasterized at exactly
    // the pixels it is drawn at, one fixed size can only be the intended fraction of the frame at
    // one resolution - the shipped 14 px would read as half the size on a 2160p client. On,
    // esp_font_px is the size at 1080p and the rasterized size grows with the client (1440p ->
    // x1.33, 4K -> x2), never shrinking below it, so a smaller or DPI-scaled client keeps the
    // size the user picked. Off, esp_font_px is the literal pixel size on every client.
    bool esp_font_auto = true;
    int menu_font_px = 16; // font em height for the menu
    std::string font_face = "Segoe UI Semibold";
    bool font_bold = false;
    bool text_shadow = true;  // 1px black drop shadow behind labels

    // ESP update rate. <0 (default) = uncapped, present every frame; 0 = follow the display's
    // ESP loop rate: 60 default (smooth, steady CPU). 0 = match display refresh; -1 = uncapped.
    int present_fps = 60;
    // GPU composition present path (fast). Disable to force UpdateLayeredWindow.
    bool overlay_dcomp = true;
    // Block in the swap-chain Present until vblank. Off keeps latency lowest.
    bool overlay_vsync = false;
    bool skip_when_unfocused = true;  // idle when the game is not the foreground window
    // Skip characters the client marks dead: a confirmed AionCharAnimInstance::bDead == 1, latched
    // until the character has a movement mode again - the client clears bDead when the death
    // animation ends, with the body still lying in the level. See EntityCache::Meta::dead_latch.
    bool hide_dead = true;
    // Diagnostic: draw EVERY actor (scenery, volumes, nav splines included) and label it
    // with its real class name instead of the category, so unknown actor types can be
    // identified in-game. The list is distance sorted, so the draw limit keeps the
    // nearest actors - use max_distance / draw limit to control the clutter.
    bool esp_show_all = false;

    // Per-category visibility (independent of esp_enabled). NPCs, pets and items are off by
    // default: in a town they are most of the actor list and they drown out the things this is
    // actually for. Players are ON: another player is the one actor the user of an ESP is looking
    // for in PvP, and a shipped default that draws them only after the user finds the category grid
    // is indistinguishable from a broken ESP. Monsters, gathering nodes, interactables and portals
    // stay on; world objects and other actors are opt-in.
    bool cat_player = true;
    bool cat_npc = false;
    bool cat_pet = false;
    bool cat_monster = true;
    bool cat_item = false;
    bool cat_gather = true;
    bool cat_interact = true;
    bool cat_portal = true;
    bool cat_object = false;
    // Reward/hidden cubes ("Hidden Cube", Od energy cubes). On by default: they are the
    // reason to look at the ground, and they are a category of their own so they no longer
    // share Interact's violet or Gather's magenta.
    bool cat_cube = true;
    // Containers (chests, lootboxes, bags) that the client still reports as **unopened**: the
    // family is drawn only while AionEnvObjAnimInstance::MainState reads Available. They used to be
    // dropped unconditionally, which is why a dungeon's chests never appeared - and the state byte
    // is exactly what separates a spent chest from a live one, so there is one category now.
    bool cat_chest = true;
    bool cat_other = false;

    // ESP colours, packed 0xRRGGBB, one per category plus the two list colours. The renderer looks
    // the category up here instead of carrying a hard-coded palette; the menu's Colours section
    // edits them in place (LEFT/RIGHT cycles a named preset, ENTER opens the R/G/B rows) and
    // "Save settings" writes them back. aion.ini takes hex: color_monster=FF9696.
    int color_player = 0x00FF00;    // green
    int color_npc = 0x00DCFF;       // cyan
    int color_pet = 0x96FF96;       // light green
    int color_monster = 0xFF9696;   // pale red: the trash-mob colour every monster used to get
    int color_boss = 0xFF3C3C;      // solid red: list_boss
    int color_target = 0xFFDC00;    // list_target: the drop colour, so "what I came for" reads
                                    // the same way loot does
    int color_item = 0xFFDC00;      // drops
    int color_chest = 0xFFA033;     // amber: an unopened container. Kept off Item's yellow and
                                    // Portal's white so a chest cannot be mistaken for a drop
    int color_gather = 0xAA82FF;    // violet
    int color_interact = 0xFF00FF;  // magenta
    int color_cube = 0xFFDC00;      // yellow, as Item: a hidden cube should read as loot
    int color_portal = 0xFFFFFF;    // white: every hue is taken, and a portal reads as a gate
    int color_object = 0xC8C8C8;    // light grey
    int color_other = 0x8C8C8C;     // grey
    // Actors the client is not rendering (AActor::bHidden). A dim grey by default: they are real
    // actors with a real position, but they are not on screen, and they must not read like
    // something you can see.
    int color_hidden = 0x8A8A8A;
    // Label text. These four were literals in the renderer, which is why the menu could not offer
    // them: they are the only colours an ESP is read through, so they belong in the file with the
    // rest.
    int color_label = 0xFFFFFF;      // the name line
    int color_distance = 0xFFFF00;   // the distance line
    int color_class = 0x78C8FF;      // the class line
    int color_class_all = 0xFFBE50;  // the class line in SHOW ALL, where it is the real class

    // --- SELF HEALTH STATE (the local player's own health) ---------------------------------------
    // The client's real HP/MP is NOT reachable from outside it. Measured, in this order, against
    // build/sdk_dump.txt and the live client: no gameplay object carries it (AionCharacter,
    // AionPlayerController and every AionCharSession system expose no HP/MP field, this client has
    // no PlayerState class at all, and the anim instance carries only booleans); the HUD widget that
    // draws the bars (AionWidget_HUD_UserInfo -> P_PBar_Hp/P_PBar_Mp -> AionProgressBar::Percent)
    // has no live instance anywhere in the object table - only its class default object, whose bar
    // pointers are null by construction; the packet mirror exists as a type with no instance; and
    // AionUISystem's LoadedDialogMap holds AionDialog instances, not widgets. The walk and its
    // counts are in build/self_hpmp_final.md. What IS readable is the client's own low-health
    // warning on the local pawn (AionCharAnimInstance::bHpDangerAlert), so the readout is a
    // STATE - ok / LOW / unknown - and never a percentage it does not have. Both HP/MP hops are
    // still resolved by property name at run time, so a patch that exposes the ratio turns the same
    // readout into a bar. Off by default; while it is off, nothing at all is read for it.
    bool self_vitals = false;
    int self_vitals_corner = 0;   // 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
    int self_vitals_width = 180;  // panel width in pixels
    int self_vitals_margin_px = 16;
    int color_self_bg = 0x0A0A10;
    int color_self_hp = 0xE05050;
    int color_self_mp = 0x5080E0;

    // Auto dodge (experimental): sends auto_dodge_vk to the game on a trigger. Off by default.
    // Trigger 1 = rising edge on bHpDangerAlert (low-HP panic, not attack telegraph timing).
    // Trigger 2 is reserved for telegraph/block-window detection once RE lands.
    bool auto_dodge = false;
    int auto_dodge_vk = 0xA0;  // VK_LSHIFT — use 0xA0 for Left Shift, not 0x10 (generic Shift)
    int auto_dodge_move_vk = 0x57;  // VK_W — AION2 dodge is directional (Shift + WASD); 0 = shift only
    // 0=auto (FakerInput, then Interception, then SendInput), 1=SendInput, 2=Interception, 3=FakerInput
    int auto_dodge_input = 0;
    int auto_dodge_cooldown_ms = 650;
    // 0=off 1=hp 2=telegraph edge 3=hp or telegraph edge
    int auto_dodge_trigger = 3;
    double auto_dodge_range_m = 18.0;
    int auto_dodge_scan_period_ms = 120;
    bool auto_dodge_projectiles = true;
    bool auto_dodge_ui_objects = false;
    bool auto_dodge_anim_flags = false;
    std::string auto_dodge_class_hints =
        "Telegraph,Dodge,BlockWindow,Counter,Parry,Evade,Warning,HitZone";
    std::string auto_dodge_anim_props =
        "bDodgeWindow,bEvadeWindow,bCounterWindow,bParryWindow,bAttackWarning,bSkillWarning,"
        "bHitWarning";

    // User class lists (comma separated substrings) - see ClassRules.
    std::string list_player, list_npc, list_pet, list_monster, list_item,
        list_gather, list_cube, list_interact, list_ignore;
    // Bosses and mini-bosses, by model, object name or class substring. They are drawn in the
    // solid red that every monster used to get; ordinary monsters are pale red. Nothing the
    // client exposes marks a monster as a boss, so this is a declared list - see
    // IsBossActor in EntityCache.hpp for what was ruled out.
    std::string list_boss;
    // The monsters the user is hunting. Same matching as list_boss (whole names, exactly), but
    // painted in the drop colour
    // (yellow) - the list is short and deliberate, so it reads as "this is what you came for".
    // Declaring a target wins over the boss colour when an entry is on both lists.
    std::string list_target;
    // Class name substrings that are never drawn, not even in the esp_show_all
    // diagnostic. Controllers, projectiles and footstep actors are always hidden.
    // Matched against the class *or* the mesh asset (see AlwaysHidden).
    std::string never_show;
    // Learned model -> localized name table (RealNames.hpp). Written by the overlay as
    // nameplates are seen; --harvest-plates fills it in one pass. Editable by hand.
    std::string names_file = "aion_names.txt";
    // Seconds to wait for AION2.exe to appear and for its runtime objects (GNames, GObjects)
    // to come up. The client publishes GNames first and GObjects a few seconds later, so an
    // overlay started during that window - or before the game - used to exit with
    // "SDK init failed" while nothing was wrong. 0 disables the wait.
    int sdk_wait_seconds = 60;
    // "pattern=Display Name;pattern2=Name2" - matched against the mesh/model name,
    // then the object name and the class name. UE names every spawned NPC after its
    // class, so this is how you turn "MOB_Specter_01" into "Lost Specter".
    std::string name_map;

    // The name rules the user edits, in the menu: what aion_names.txt used to express as
    // 'key=Name' and '#' rows, plus one that file could never express. All of them are persisted in
    // ONE key (name_rules, see Pairs):
    //   Forced  the old 'key=Name' row: always show that label for that pattern.
    //   Auto    the old '#key' / '#prefix*' row: never take a label from storage, show the name the
    //           client reports live - in this session and in later ones.
    //   Hidden  new: never label that pattern at all. The entity still draws; only the name label
    //           is suppressed.
    // The pattern is matched against the model name, lowercased; a trailing '*' means the family of
    // models whose name starts with it, which is what a portal needs (its model changes per run).
    struct NameRule {
        enum class Mode { Auto, Forced, Hidden } mode = Mode::Auto;
        std::string pattern;
        std::string label;  // only meaningful for Mode::Forced
    };
    std::vector<NameRule> name_rules;
    // Set by Load when the file has the name_rules key - which is how the one-time import of the
    // rules still held by aion_names.txt's own rows is told apart from a list the user emptied.
    // Never persisted, never read from the file (like dump_classes below).
    bool name_rules_seen = false;

    // Set by the menu; the main loop dumps a live class histogram and clears it.
    bool dump_classes = false;
    // Also set by the menu and consumed by the main loop: re-read aion.ini now, so a monster name
    // can be added mid-session. Kept in the config (like dump_classes) so Menu.cpp stays free of
    // the main loop's internals. Never read from the file.
    bool reload_config = false;

    // Set by the menu's "Save settings", consumed by the main loop: rewrite the keys above in
    // aion.ini instead of only in memory. A command like reload_config, never read from the file.
    bool save_config = false;

    bool log_to_file = false;

    // Every persisted key with its current value, in aion.ini's order. Transient commands
    // (dump_classes, reload_config, save_config) are deliberately absent: they are not settings.
    // A list is edited as text in the menu, where a line break separates entries like a comma does.
    // The file stays one line per key - a value spanning lines would be cut in two when it is read
    // back - so a break is stored as a comma.
    static std::string SingleLine(const std::string& s) {
        std::string out = s;
        for (char& c : out)
            if (c == '\n' || c == '\r') c = ',';
        return out;
    }

    std::vector<std::pair<std::string, std::string>> Pairs() const {
        std::vector<std::pair<std::string, std::string>> p;
        auto B = [](bool v) { return std::string(v ? "1" : "0"); };
        auto I = [](int v) {
            char b[32];
            std::snprintf(b, sizeof(b), "%d", v);
            return std::string(b);
        };
        auto D = [](double v) {
            char b[32];
            std::snprintf(b, sizeof(b), "%.4g", v);
            return std::string(b);
        };
        auto C = [](int v) {
            char b[32];
            std::snprintf(b, sizeof(b), "%06X", v & 0xFFFFFF);
            return std::string(b);
        };
        static const char* kSnap[4] = {"off", "bottom", "player", "top"};
        auto add = [&](const char* k, std::string v) { p.emplace_back(k, std::move(v)); };
        add("esp_enabled", B(esp_enabled));
        add("esp_box", B(esp_box));
        add("esp_box_corners", B(esp_box_corners));
        add("esp_corner_frac", D(esp_corner_frac));
        add("esp_name", B(esp_name));
        add("esp_distance", B(esp_distance));
        add("esp_class", B(esp_class));
        add("esp_offscreen", B(esp_offscreen));
        add("offscreen_range_m", D(offscreen_range_m));
        add("esp_offscreen_snapline", B(esp_offscreen_snapline));
        add("esp_snapline",
            kSnap[snapline >= 0 && snapline <= 3 ? snapline : SnaplineBottom]);
        add("esp_target", B(esp_target));
        add("esp_debug", B(esp_debug));
        add("max_distance_m", D(max_distance_m));
        add("esp_hidden", B(esp_hidden));
        add("hidden_range_m", D(hidden_range_m));
        add("esp_thickness", D(esp_thickness));
        add("esp_text_scale", D(esp_text_scale));
        add("esp_font_px", I(esp_font_px));
        add("esp_font_auto", B(esp_font_auto));
        add("menu_font_px", I(menu_font_px));
        add("font_face", font_face);
        add("font_bold", B(font_bold));
        add("text_shadow", B(text_shadow));
        add("hide_dead", B(hide_dead));
        add("filter_local_player", B(filter_local_player));
        add("max_entities_drawn", I(max_entities_drawn));
        add("present_fps", I(present_fps));
        add("overlay_dcomp", B(overlay_dcomp));
        add("overlay_vsync", B(overlay_vsync));
        add("skip_when_unfocused", B(skip_when_unfocused));
        add("esp_show_all", B(esp_show_all));
        add("cat_player", B(cat_player));
        add("cat_npc", B(cat_npc));
        add("cat_pet", B(cat_pet));
        add("cat_monster", B(cat_monster));
        add("cat_item", B(cat_item));
        add("cat_gather", B(cat_gather));
        add("cat_interact", B(cat_interact));
        add("cat_cube", B(cat_cube));
        add("cat_chest", B(cat_chest));
        add("cat_portal", B(cat_portal));
        add("cat_object", B(cat_object));
        add("cat_other", B(cat_other));
        add("color_player", C(color_player));
        add("color_npc", C(color_npc));
        add("color_pet", C(color_pet));
        add("color_monster", C(color_monster));
        add("color_boss", C(color_boss));
        add("color_target", C(color_target));
        add("color_item", C(color_item));
        add("color_chest", C(color_chest));
        add("color_gather", C(color_gather));
        add("color_interact", C(color_interact));
        add("color_cube", C(color_cube));
        add("color_portal", C(color_portal));
        add("color_object", C(color_object));
        add("color_other", C(color_other));
        add("color_hidden", C(color_hidden));
        add("self_vitals", B(self_vitals));
        add("self_vitals_corner", I(self_vitals_corner));
        add("self_vitals_width", I(self_vitals_width));
        add("self_vitals_margin_px", I(self_vitals_margin_px));
        add("auto_dodge", B(auto_dodge));
        add("auto_dodge_vk", I(auto_dodge_vk));
        add("auto_dodge_move_vk", I(auto_dodge_move_vk));
        add("auto_dodge_input", I(auto_dodge_input));
        add("auto_dodge_cooldown_ms", I(auto_dodge_cooldown_ms));
        add("auto_dodge_trigger", I(auto_dodge_trigger));
        add("auto_dodge_range_m", D(auto_dodge_range_m));
        add("auto_dodge_scan_period_ms", I(auto_dodge_scan_period_ms));
        add("auto_dodge_projectiles", B(auto_dodge_projectiles));
        add("auto_dodge_ui_objects", B(auto_dodge_ui_objects));
        add("auto_dodge_anim_flags", B(auto_dodge_anim_flags));
        add("auto_dodge_class_hints", SingleLine(auto_dodge_class_hints));
        add("auto_dodge_anim_props", SingleLine(auto_dodge_anim_props));
        add("color_self_bg", C(color_self_bg));
        add("color_self_hp", C(color_self_hp));
        add("color_self_mp", C(color_self_mp));
        add("color_label", C(color_label));
        add("color_distance", C(color_distance));
        add("color_class", C(color_class));
        add("color_class_all", C(color_class_all));
        add("list_player", SingleLine(list_player));
        add("list_npc", SingleLine(list_npc));
        add("list_pet", SingleLine(list_pet));
        add("list_monster", SingleLine(list_monster));
        add("list_item", SingleLine(list_item));
        add("list_gather", SingleLine(list_gather));
        add("list_cube", SingleLine(list_cube));
        add("list_interact", SingleLine(list_interact));
        add("list_ignore", SingleLine(list_ignore));
        add("list_boss", SingleLine(list_boss));
        add("list_target", SingleLine(list_target));
        add("never_show", SingleLine(never_show));
        add("name_map", name_map);
        add("name_rules", FormatNameRules(name_rules));
        add("names_file", names_file);
        add("sdk_wait_seconds", I(sdk_wait_seconds));
        add("log_to_file", B(log_to_file));
        return p;
    }

    // Rewrite those keys in place: a `key=value` line keeps its position and its line ending, so
    // every comment around it survives and aion.ini stays the user's documented file rather than
    // being regenerated. Keys the file does not have are appended at the end. Returns how many
    // keys were written, or -1 if the file could not be written.
    int Save(const std::string& path) const {
        std::vector<std::string> lines;
        // Read whole, then split: a line can be far longer than any buffer (name_rules carries every
        // rule, keeping the value in one line), and a line the reader split in two would be written
        // back as two - the value would be cut in half again on every save.
        {
            std::FILE* in = std::fopen(path.c_str(), "rb");
            if (in) {
                std::string text;
                char buf[4096];
                std::size_t n = 0;
                while ((n = std::fread(buf, 1, sizeof buf, in)) > 0) text.append(buf, n);
                std::fclose(in);
                for (std::size_t at = 0; at < text.size();) {
                    const std::size_t nl = text.find('\n', at);
                    if (nl == std::string::npos) {
                        lines.push_back(text.substr(at));
                        break;
                    }
                    lines.push_back(text.substr(at, nl - at + 1));  // keep '\n' on its own line
                    at = nl + 1;
                }
            }
        }
        const auto pairs = Pairs();
        std::vector<char> done(pairs.size(), 0);
        int written = 0;
        for (auto& line : lines) {
            const std::size_t a = line.find_first_not_of(" \t");
            if (a == std::string::npos || line[a] == '#' || line[a] == '\r' || line[a] == '\n')
                continue;
            const std::size_t eq = line.find('=', a);
            if (eq == std::string::npos) continue;
            const std::string key = Trim(line.substr(a, eq - a));
            for (std::size_t i = 0; i < pairs.size(); ++i) {
                if (key != pairs[i].first) continue;
                if (done[i]) break;  // a duplicated line is left alone, the first one wins
                const std::size_t eol = line.find_last_not_of("\r\n");
                const std::string tail =
                    eol == std::string::npos ? std::string("\n") : line.substr(eol + 1);
                line = line.substr(0, a) + key + "=" + pairs[i].second + tail;
                done[i] = 1;
                ++written;
                break;
            }
        }
        std::string added;
        for (std::size_t i = 0; i < pairs.size(); ++i) {
            if (done[i]) continue;
            added += pairs[i].first + "=" + pairs[i].second + "\n";
            ++written;
        }
        std::FILE* out = std::fopen(path.c_str(), "wb");
        if (!out) return -1;
        for (const auto& l : lines) std::fwrite(l.data(), 1, l.size(), out);
        if (!added.empty()) std::fwrite(added.data(), 1, added.size(), out);
        std::fclose(out);
        return written;
    }

    static Config& Instance() {
        static Config c;
        return c;
    }

    // Applies the file onto the defaults, so loading again means "the file is now the truth": a key
    // the file no longer has goes back to its shipped value instead of lingering from the previous
    // load. A file that cannot be opened changes nothing.
    void Load(const std::string& path) {
        // The file is read whole rather than with a fixed line buffer: a value can be far longer
        // than any buffer (name_rules carries every rule, and a long list_* value is one line too)
        // and a line that does not fit is a setting that silently does not load.
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return;
        std::string text;
        {
            char buf[4096];
            std::size_t n = 0;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        }
        std::fclose(f);
        *this = Config{};
        for (std::size_t at = 0; at < text.size();) {
            const std::size_t nl = text.find('\n', at);
            const std::string s =
                text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
            at = nl == std::string::npos ? text.size() : nl + 1;
            auto eq = s.find('=');
            if (eq == std::string::npos) continue;
            std::string k = Trim(s.substr(0, eq));
            std::string v = Trim(s.substr(eq + 1));
            if (k.empty()) continue;
            if (AssignColor(*this, k, v)) continue;
            if (k == "esp_enabled") esp_enabled = Bool(v);
            else if (k == "esp_box") esp_box = Bool(v);
            else if (k == "esp_name") esp_name = Bool(v);
            else if (k == "esp_distance") esp_distance = Bool(v);
            else if (k == "esp_class") esp_class = Bool(v);
            else if (k == "esp_offscreen") esp_offscreen = Bool(v);
            else if (k == "offscreen_range_m") offscreen_range_m = std::atof(v.c_str());
            else if (k == "esp_offscreen_snapline") esp_offscreen_snapline = Bool(v);
            else if (k == "esp_snapline") snapline = Snapline(v, snapline);
            else if (k == "max_distance_m") max_distance_m = std::atof(v.c_str());
            else if (k == "esp_hidden") esp_hidden = Bool(v);
            else if (k == "hidden_range_m") hidden_range_m = std::atof(v.c_str());
            // Older files hold the limit in world units (cm), where the shipped 20000
            // meant 200 m. Read it so an existing aion.ini keeps its range.
            else if (k == "max_distance") max_distance_m = std::atof(v.c_str()) / 100.0;
            else if (k == "esp_box_corners") esp_box_corners = Bool(v);
            else if (k == "esp_corner_frac") esp_corner_frac = std::atof(v.c_str());
            else if (k == "esp_thickness") esp_thickness = std::atof(v.c_str());
            else if (k == "esp_text_scale") esp_text_scale = std::atof(v.c_str());
            else if (k == "esp_font_px") esp_font_px = std::atoi(v.c_str());
            else if (k == "esp_font_auto") esp_font_auto = Bool(v);
            else if (k == "menu_font_px") menu_font_px = std::atoi(v.c_str());
            else if (k == "font_face") font_face = v;
            else if (k == "font_bold") font_bold = Bool(v);
            else if (k == "text_shadow") text_shadow = Bool(v);
            else if (k == "present_fps") present_fps = std::atoi(v.c_str());
            else if (k == "overlay_dcomp") overlay_dcomp = Bool(v);
            else if (k == "overlay_vsync") overlay_vsync = Bool(v);
            else if (k == "skip_when_unfocused") skip_when_unfocused = Bool(v);
            else if (k == "hide_dead") hide_dead = Bool(v);
            else if (k == "filter_local_player") filter_local_player = Bool(v);
            else if (k == "cat_player") cat_player = Bool(v);
            else if (k == "cat_npc") cat_npc = Bool(v);
            else if (k == "cat_pet") cat_pet = Bool(v);
            else if (k == "cat_monster") cat_monster = Bool(v);
            else if (k == "cat_item") cat_item = Bool(v);
            else if (k == "cat_gather") cat_gather = Bool(v);
            else if (k == "cat_interact") cat_interact = Bool(v);
            else if (k == "cat_portal") cat_portal = Bool(v);
            else if (k == "cat_object") cat_object = Bool(v);
            else if (k == "cat_cube") cat_cube = Bool(v);
            else if (k == "cat_chest") cat_chest = Bool(v);
            else if (k == "cat_other") cat_other = Bool(v);
            else if (k == "color_hidden") color_hidden = ParseColor(v, color_hidden);
            else if (k == "self_vitals") self_vitals = Bool(v);
            else if (k == "self_vitals_corner") self_vitals_corner = std::atoi(v.c_str());
            else if (k == "self_vitals_width") self_vitals_width = std::atoi(v.c_str());
            else if (k == "self_vitals_margin_px") self_vitals_margin_px = std::atoi(v.c_str());
            else if (k == "auto_dodge") auto_dodge = Bool(v);
            else if (k == "auto_dodge_vk") {
                auto_dodge_vk = std::atoi(v.c_str());
                if (auto_dodge_vk == 0x10) auto_dodge_vk = 0xA0;
            }
            else if (k == "auto_dodge_move_vk") auto_dodge_move_vk = std::atoi(v.c_str());
            else if (k == "auto_dodge_input") auto_dodge_input = std::atoi(v.c_str());
            else if (k == "auto_dodge_cooldown_ms") auto_dodge_cooldown_ms = std::atoi(v.c_str());
            else if (k == "auto_dodge_trigger") auto_dodge_trigger = std::atoi(v.c_str());
            else if (k == "auto_dodge_range_m") auto_dodge_range_m = std::atof(v.c_str());
            else if (k == "auto_dodge_scan_period_ms")
                auto_dodge_scan_period_ms = std::atoi(v.c_str());
            else if (k == "auto_dodge_projectiles") auto_dodge_projectiles = Bool(v);
            else if (k == "auto_dodge_ui_objects") auto_dodge_ui_objects = Bool(v);
            else if (k == "auto_dodge_anim_flags") auto_dodge_anim_flags = Bool(v);
            else if (k == "auto_dodge_class_hints") auto_dodge_class_hints = v;
            else if (k == "auto_dodge_anim_props") auto_dodge_anim_props = v;
            else if (k == "list_player") list_player = v;
            else if (k == "list_npc") list_npc = v;
            else if (k == "list_pet") list_pet = v;
            else if (k == "list_monster") list_monster = v;
            else if (k == "list_item") list_item = v;
            else if (k == "list_gather") list_gather = v;
            else if (k == "list_cube") list_cube = v;
            else if (k == "list_interact") list_interact = v;
            else if (k == "list_ignore") list_ignore = v;
            else if (k == "list_boss") list_boss = v;
            else if (k == "list_target") list_target = v;
            else if (k == "esp_target") esp_target = Bool(v);
            else if (k == "esp_debug") esp_debug = Bool(v);
            else if (k == "max_entities_drawn") max_entities_drawn = std::atoi(v.c_str());
            else if (k == "never_show") never_show = v;
            else if (k == "names_file") names_file = v;
            else if (k == "sdk_wait_seconds") sdk_wait_seconds = std::atoi(v.c_str());
            else if (k == "name_map") name_map = v;
            else if (k == "name_rules") {
                name_rules = ParseNameRules(v);
                name_rules_seen = true;  // present, even when empty: the user emptied the list
            }
            else if (k == "log_to_file") log_to_file = Bool(v);
            // Unknown keys are ignored rather than collected: the file is rewritten from Pairs() on
            // Save, so anything kept here would be dropped on the next save anyway.
        }
    }

private:
    // The colour keys, one row each: adding a category is one line here, not a new branch.
    static bool AssignColor(Config& c, const std::string& k, const std::string& v) {
        static const struct { const char* Key; int Config::*Field; } kTable[] = {
            {"color_player", &Config::color_player},     {"color_npc", &Config::color_npc},
            {"color_pet", &Config::color_pet},           {"color_monster", &Config::color_monster},
            {"color_boss", &Config::color_boss},         {"color_target", &Config::color_target},
            {"color_item", &Config::color_item},
            {"color_chest", &Config::color_chest},
            {"color_gather", &Config::color_gather},     {"color_interact", &Config::color_interact},
            {"color_cube", &Config::color_cube},         {"color_portal", &Config::color_portal},
            {"color_object", &Config::color_object},     {"color_other", &Config::color_other},
            {"color_label", &Config::color_label},       {"color_distance", &Config::color_distance},
            {"color_class", &Config::color_class},       {"color_class_all", &Config::color_class_all},
            {"color_hidden", &Config::color_hidden},
            {"color_self_bg", &Config::color_self_bg},
            {"color_self_hp", &Config::color_self_hp},
            {"color_self_mp", &Config::color_self_mp},
        };
        for (const auto& e : kTable) {
            if (k != e.Key) continue;
            c.*(e.Field) = ParseColor(v, c.*(e.Field));
            return true;
        }
        return false;
    }

    // "RRGGBB", "#RRGGBB" or "0xRRGGBB". Anything else - a typo, a colour name, a wrong length -
    // leaves the current value in place: a mis-typed key should not paint a category black.
    static int ParseColor(const std::string& v, int fallback) {
        std::string h = v;
        if (!h.empty() && h[0] == '#') h.erase(0, 1);
        else if (h.size() > 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h.erase(0, 2);
        if (h.size() != 6) return fallback;
        int out = 0;
        for (char c : h) {
            int d = -1;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            if (d < 0) return fallback;
            out = out * 16 + d;
        }
        return out;
    }

    // The ini words "off", "bottom", "player" (or "pawn") and "top", plus 0 and 1 - the old boolean,
    // which meant off and bottom, the same thing the words do. Anything else (a typo, a colour word)
    // leaves the current value rather than quietly turning the snapline off, which is what
    // atoi() != 0 did when the value was not a number at all.
    static int Snapline(const std::string& v, int fallback) {
        if (v == "top") return SnaplineTop;
        if (v == "player" || v == "pawn") return SnaplinePlayer;
        if (v == "bottom" || v == "1") return SnaplineBottom;
        if (v == "off" || v == "0") return SnaplineOff;
        return fallback;
    }

    // A name_rules value is one line of `mode:pattern` or `mode:pattern=label` entries joined by
    // ';', so a ';' or '=' inside a pattern or a label would be read back as a separator. Both are
    // written as a space instead; a pattern that needs one cannot be written in a key=value file.
    static std::string Sanitise(const std::string& s) {
        std::string o = s;
        for (char& c : o)
            if (c == ';' || c == '=') c = ' ';
        return o;
    }

    // The rule list as the ini value: `force:pattern=Label;auto:pattern;hide:pattern`. Only Forced
    // carries a label - the other two modes never take one from storage.
    static std::string FormatNameRules(const std::vector<NameRule>& rules) {
        std::string out;
        for (const auto& r : rules) {
            if (r.pattern.empty()) continue;  // an entry with nothing to match is nothing to write
            if (!out.empty()) out += ';';
            out += r.mode == NameRule::Mode::Forced ? "force"
                   : r.mode == NameRule::Mode::Hidden ? "hide"
                                                      : "auto";
            out += ':';
            out += Sanitise(r.pattern);
            if (r.mode == NameRule::Mode::Forced) {
                out += '=';
                out += Sanitise(r.label);
            }
        }
        return out;
    }

    // The same format read back. An entry whose mode is not one of the three, or that has no
    // pattern, is skipped: a hand-edited line should cost its own rule, not the whole list.
    static std::vector<NameRule> ParseNameRules(const std::string& v) {
        std::vector<NameRule> out;
        for (std::size_t b = 0; b < v.size();) {
            const std::size_t end = v.find(';', b);
            const std::string entry =
                Trim(v.substr(b, end == std::string::npos ? std::string::npos : end - b));
            b = end == std::string::npos ? v.size() : end + 1;
            const std::size_t colon = entry.find(':');
            if (colon == std::string::npos) continue;
            const std::string mode = Trim(entry.substr(0, colon));
            NameRule r;
            if (mode == "force") r.mode = NameRule::Mode::Forced;
            else if (mode == "auto") r.mode = NameRule::Mode::Auto;
            else if (mode == "hide") r.mode = NameRule::Mode::Hidden;
            else continue;
            // Split the rest at its first '=': a ':' inside the pattern is part of the pattern.
            const std::size_t eq = entry.find('=', colon + 1);
            const std::size_t body_end =
                eq == std::string::npos ? std::string::npos : eq - colon - 1;
            const std::string body = entry.substr(colon + 1);
            r.pattern = Lower(Trim(body.substr(0, body_end)));
            if (eq != std::string::npos) r.label = Trim(body.substr(body_end + 1));
            if (r.pattern.empty()) continue;
            out.push_back(std::move(r));
        }
        return out;
    }

    static std::string Lower(const std::string& s) {
        std::string o = s;
        for (char& c : o)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        return o;
    }
    static std::string Trim(const std::string& s) {
        const char* ws = " \t\r\n";
        auto a = s.find_first_not_of(ws);
        if (a == std::string::npos) return {};
        auto b = s.find_last_not_of(ws);
        return s.substr(a, b - a + 1);
    }
    static bool Bool(const std::string& v) {
        return v == "1" || v == "true" || v == "on" || v == "yes";
    }
};

// The pixel size the ESP labels are rasterized at for a client that is client_h pixels tall. The
// glyph atlas is drawn 1:1 and never resampled, so following the frame means rasterizing at the
// new size (see Config::esp_font_auto) - this is the one place that decides it, so the init path,
// the menu's re-raster on a font change and the per-frame check all agree.
inline int EspFontPx(const Config& c, int client_h) {
    if (!c.esp_font_auto || client_h <= 0) return c.esp_font_px;
    const double scale = static_cast<double>(client_h) / 1080.0;
    // Grows only: at or below 1080 lines the configured size is already the right physical size
    // for that panel (and a 125%-scaled 1536x864 client still gets legible labels).
    if (scale <= 1.0) return c.esp_font_px;
    const int px = static_cast<int>(c.esp_font_px * scale + 0.5);
    return px > 96 ? 96 : px;  // atlas cell budget, not a legibility limit
}

}  // namespace aion
