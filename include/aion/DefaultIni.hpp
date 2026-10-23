// DefaultIni.hpp - the aion.ini a fresh copy of the overlay starts with.
//
// Written once, when nothing is found next to the exe (see ResolveDataFile). The values come from
// Pairs() of a default-constructed Config, so the generated file cannot disagree with the shipped
// defaults; this table only supplies the order and the comments. A key that is missing from the
// table is still written - appended at the end, uncommented - so a setting added to Config can never
// be absent from a generated file, and overlay_test asserts both directions: every persisted key
// appears exactly once, and loading the file back reproduces the defaults.
#pragma once
#include <cstdio>
#include <string>
#include <vector>

#include "Config.hpp"

namespace aion {

struct IniComment {
    const char* Key;
    const char* Comment;  // "" for none; every line already ends in '\n'
};

inline const char* IniHeader() {
    return "# AION2 external overlay configuration.\n"
           "#\n"
           "# Every setting here can also be changed in the INSERT menu, and the menu's\n"
           "# \"Save settings\" row writes this file back in place - it rewrites the key=value lines\n"
           "# only, so these comments and any key it does not know survive. A key that is missing\n"
           "# falls back to its shipped default, so deleting a line is safe.\n"
           "#\n"
           "# Data the overlay accumulates lives in aion_names.txt next to the exe: the learned\n"
           "# model -> name cache, filled in by playing or by --harvest-plates. Everything else -\n"
           "# the lists, the colours, the name rules - is a setting below, edited in the menu or\n"
           "# here.\n"
           "\n";
}

inline const std::vector<IniComment>& IniCommentTable() {
    static const std::vector<IniComment> kTable = {
        {"esp_enabled", "# --- ESP -----------------------------------------------------------------\n"
                        "# Master switch. With it off nothing is drawn at all.\n"},
        {"esp_box", ""},
        {"esp_box_corners",
         "# 1 = an L bracket at each corner (leaves the sides open, so the box stops competing with\n"
         "# the character and its own labels), 0 = a closed rectangle.\n"},
        {"esp_corner_frac",
         "# Bracket arm length as a share of the box's short side (clamped to half, so two corners\n"
         "# can never meet and re-form an outline).\n"},
        {"esp_name", ""},
        {"esp_distance", ""},
        {"esp_class", ""},
        {"esp_offscreen",
         "# Off-screen markers: an arrow pinned to the screen border pointing at anything nearby\n"
         "# that the camera is not showing (behind you, or outside the viewport). A box in screen\n"
         "# space has nothing to draw for those - a point behind the camera has no screen position\n"
         "# at all - so without this they are invisible until you turn the camera onto them.\n"},
        {"offscreen_range_m",
         "# How far those markers reach, in metres, from your character. Deliberately shorter than\n"
         "# max_distance_m: a marker is read without turning, so a long reach is a frame of arrows.\n"},
        {"esp_offscreen_snapline",
         "# Draw a snapline to each off-screen marker, from the same origin the box snaplines use.\n"
         "# The arrow says which way to turn; the line is what makes one marker findable when\n"
         "# several share an edge. Needs esp_snapline set to anything but off.\n"},
        {"esp_snapline",
         "# Snapline origin: off, bottom, player, top. bottom/top start at the midpoint of that\n"
         "# screen edge; player starts on your own character's box centre, so the line stays\n"
         "# attached to the character while the camera pans - an edge-anchored one slides sideways\n"
         "# with the view. 1/0 (the old on/off) still read as bottom/off. Also a menu row.\n"},
        {"esp_target",
         "# The hunting list as a category of its own: paint the actors on list_target in the\n"
         "# drop colour and keep them drawn when their own category is off.\n"},
        {"esp_debug",
         "# Diagnostic: log a per-entity dump and show the on-screen performance panel.\n"},
        {"max_distance_m",
         "# Range limit in metres, measured from your character - not from the camera, which sits\n"
         "# behind and above you and would overstate every distance by the zoom level.\n"},
        {"esp_thickness",
         "# Box and snapline stroke width in pixels. The stroke is feathered, so 1.0 is\n"
         "# a solid pixel and 0.5 is half the colour; anything under about 0.5 reads as invisible on\n"
         "# a bright screen. 1.0 is the default.\n"},
        {"esp_text_scale",
         "# ESP text size: 1.0 = the atlas' native pixel size (drawn 1:1, never resampled).\n"},
        {"esp_font_px",
         "# ESP label font em height in pixels at a 1080p-tall client (8..32). The glyph atlas is\n"
         "# rasterized at exactly the size it is drawn at, so this is a crisp pixel size, not a\n"
         "# scaled one. Changing it rebuilds the atlas.\n"},
        {"esp_font_auto",
         "# Follow the client's height with the label size: esp_font_px is the size at 1080p and\n"
         "# is rasterized larger on a taller frame (1440p -> x1.33, 4K -> x2), never smaller. Turn\n"
         "# it off to use esp_font_px literally on every client.\n"},
        {"menu_font_px", "# Font em height for the menu panel.\n"},
        {"font_face", ""},
        {"font_bold", ""},
        {"text_shadow", "# 1px black drop shadow behind labels: keeps them readable over anything.\n"},
        {"hide_dead",
         "# Skip characters the client itself marks dead (AionCharAnimInstance::bDead, read by\n"
         "# reflection). The death is latched - the client clears bDead when the death animation\n"
         "# ends, with the body still lying in the level - and released when the character has a\n"
         "# movement mode again. A failed read is never treated as dead. Off: corpses stay drawn.\n"},
        {"filter_local_player",
         "# Hide your own character: it is the one actor you are standing inside.\n"},
        {"max_entities_drawn",
         "# Draw limit per frame. The entity list is distance sorted, so the near ones win.\n"},

        {"cat_player", "# --- categories -----------------------------------------------------------\n"
                       "# What is drawn at all, independent of the master switch and of the ESP\n"
                       "# box/label rows. Players, NPCs, pets and drops are off by default: in a town\n"
                       "# they are most of the actor list and they drown out what this is for.\n"},
        {"cat_npc", ""},
        {"cat_pet", ""},
        {"cat_monster", ""},
        {"cat_item", ""},
        {"cat_gather", ""},
        {"cat_interact", ""},
        {"cat_cube", "# Reward/hidden cubes: yellow like drops, a category of their own.\n"},
        {"cat_chest", "# Containers the client still reports as unopened - a chest you have not opened\n"
                      "# yet, in amber. One that has been opened is not drawn at all, and one whose\n"
                      "# state cannot be read is left visible rather than silently dropped.\n"},
        {"cat_portal", ""},
        {"cat_object", ""},
        {"cat_other", ""},

        {"color_player", "# --- colours --------------------------------------------------------------\n"
                         "# RRGGBB (a leading # or 0x is accepted). One per category plus the two\n"
                         "# list colours: box, label and snapline of an actor are all drawn in its\n"
                         "# category's colour. The menu's Colours section walks a named preset with\n"
                         "# LEFT/RIGHT and opens R/G/B with ENTER. A value that is not six hex digits\n"
                         "# is ignored, which leaves the shipped default in place.\n"},
        {"color_npc", ""},
        {"color_pet", ""},
        {"color_monster", ""},
        {"color_boss", "# everything in list_boss\n"},
        {"color_target", "# the drop colour: everything in list_target\n"},
        {"color_item", ""},
        {"color_gather", ""},
        {"color_interact", ""},
        {"color_cube", ""},
        {"color_chest", ""},
        {"color_portal", ""},
        {"color_object", ""},
        {"color_other", ""},
        {"color_hidden", "# actors the client is not rendering (see esp_hidden)\n"},

        {"color_label", "# Label text. The four label colours live here rather than in the renderer,\n"
                        "# so the menu can offer them: color_label is the name line,\n"
                        "# color_distance the distance line, color_class the class line, and\n"
                        "# color_class_all the class line under SHOW ALL (the real UObject class).\n"},
        {"color_distance", ""},
        {"color_class", ""},
        {"color_class_all", ""},

        {"list_player", "# --- class lists ----------------------------------------------------------\n"
                        "# Comma-separated case-insensitive substrings, checked BEFORE the built-in\n"
                        "# patterns against the class name, then the object name, then the model.\n"
                        "# Use the menu's \"Dump actor classes\" to see the live names to pin.\n"},
        {"list_npc", ""},
        {"list_pet", ""},
        {"list_monster", ""},
        {"list_item", ""},
        {"list_gather", ""},
        {"list_cube", ""},
        {"list_interact", ""},
        {"list_ignore", ""},
        {"list_boss",
         "# Declared bosses and mini-bosses, matched EXACTLY (case-insensitive whole name) against\n"
         "# the model first, then the object name, the ESP label and the class - any of them works,\n"
         "# but a whole name must match: \"Contaminated Swarm\" does not claim\n"
         "# \"Contaminated Swarm Egg\". Nothing the client exposes says which\n"
         "# monster attacks unprovoked (aggro is the server's decision, and no rank survives in the\n"
         "# object names or the dumps), so the rank is declared. Bosses are drawn in color_boss.\n"},
        {"list_target",
         "# The monsters you are hunting: the same exact whole-name matching as list_boss, painted in\n"
         "# color_target, the drop colour, so \"what I came for\" reads the way loot does. An entry on\n"
         "# both lists takes this colour.\n"},
        {"never_show",
         "# Classes or mesh assets that are NEVER drawn, not even in SHOW ALL (comma-separated\n"
         "# substrings, matched against both). Chests, lootboxes, bags and the rest of the\n"
         "# container family are already hidden this way, so this is for everything else - the\n"
         "# controller, projectile and footstep actors are hidden by default too.\n"},
        {"name_map",
         "# Display names: pattern=Display Name, entries separated by ';'. UE names every spawned\n"
         "# actor after its class, so the ESP shows the mesh asset instead (MOB_WindEle_01); this\n"
         "# maps it onto the real name. The pattern is matched against the model, then the object\n"
         "# name and the class name. A forced name rule (below) and a learned name in\n"
         "# aion_names.txt both win over this mapping.\n"},
        {"name_rules",
         "# What each model's label is, one entry per rule, ';' between them - the menu edits these\n"
         "# as rows and this line is what it writes. 'force:pattern=Label' always shows that label;\n"
         "# 'auto:pattern' never takes a label from storage, showing the name the client reports\n"
         "# live instead; 'hide:pattern' draws the entity but never its name. A trailing '*' on a\n"
         "# pattern covers the whole family of models starting with it (portals need it: their model\n"
         "# changes between runs). A ';' or '=' inside a pattern or a label is written as a space.\n"},
        {"names_file",
         "# Learned names: this table is keyed by the model (mesh), and it is what covers the actors\n"
         "# the client builds no nameplate for - measured, it builds none beyond roughly 25 m. A\n"
         "# name the client reveals for one actor is written here and applied to every instance of\n"
         "# that model, at any distance; where the client does name an actor itself, its own name\n"
         "# wins, which is the only way two monsters sharing one mesh are told apart. Fill it in one\n"
         "# pass with:  aion_external.exe --harvest-plates\n"
         "# A cache, not a setting: the rules above decide what is shown, and what the client\n"
         "# reports wins over anything learned while an auto rule covers the model.\n"},

        {"sdk_wait_seconds", "# --- performance ---------------------------------------------------------\n"
                             "# Seconds to wait for the AION2 client and its runtime objects (GNames, then\n"
                             "# GObjects a few seconds later) before giving up on the attach. Starting\n"
                             "# the overlay early is normal; 0 disables the wait.\n"},
        {"present_fps",
         "# ESP update rate. -1 (default) = uncapped: present every frame, so the frame the\n"
         "# compositor picks is never older than one iteration. 0 = follow the display's refresh\n"
         "# rate (lighter on CPU; a 60 fps overlay on a 144 Hz panel visibly trails the camera).\n"
         "# >0 = that many updates per second.\n"},
        {"overlay_dcomp",
         "# GPU composition present path (fast). 0 forces the UpdateLayeredWindow fallback.\n"},
        {"overlay_vsync", "# Block in Present until vblank (0 = lowest latency).\n"},
        {"skip_when_unfocused",
         "# Idle (and hide) while the game window is not in the foreground: alt-tabbing to a\n"
         "# browser, a launcher or the desktop hides the window outright, and it comes back with\n"
         "# the game. Also a menu row.\n"},
        {"esp_show_all",
         "# Diagnostic: draw EVERY actor (scenery, volumes, nav splines included) and label it with\n"
         "# its real class name instead of the category, so unknown actor types can be identified\n"
         "# in-game. Use max_distance_m and the draw limit to control the clutter.\n"},
        {"log_to_file", "# Mirror the log into the session log next to the exe.\n"},
        {"esp_hidden",
         "# Draw the actors the client is NOT rendering. bHidden alone is NOT that flag - measured\n"
         "# with --dump-gates, the client sets it on actors it IS drawing - so the gate is 'bHidden\n"
         "# and the renderer did not draw it', and a read failure never hides an actor. 0 (default)\n"
         "# leaves those actors out. Measured on the live client, the game itself draws characters\n"
         "# to about 45 m and nothing past 128 m; these are drawn in color_hidden instead, so a\n"
         "# ESP still reaches past what the game shows.\n"},
        {"hidden_range_m",
         "# How far those reach, in metres. Only used while esp_hidden is on.\n"},
        {"self_vitals",
         "# Your own health, as a STATE: ok / LOW / unknown (never a percentage, because this client\n"
         "# exposes no HP/MP number to an outside reader - see build/self_hpmp_final.md). The state\n"
         "# comes from the client's own low-health warning on your character; 'unknown' means that\n"
         "# bit could not be read and is never shown as 0. If a patch ever exposes the HUD bar's\n"
         "# ratio, the same readout draws a bar and a percentage instead, and a value read on an\n"
         "# earlier frame is kept and marked '?'.\n"},
        {"self_vitals_corner",
         "# Where the readout sits: 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right.\n"},
        {"self_vitals_width", "# Panel width in pixels.\n"},
        {"self_vitals_margin_px", "# Gap between the panel and the frame's edge, in pixels.\n"},
        {"color_self_bg", "# The panel behind the readout.\n"},
        {"color_self_hp", "# The HP state (and the bar, if a ratio is ever exposed).\n"},
        {"color_self_mp", "# The MP row (and its bar, if a ratio is ever exposed).\n"},
    };
    return kTable;
}

// The whole file, ready to write: header, then every comment/key pair in table order, then any key
// the table has never heard of.
inline std::string DefaultIniText() {
    const Config d;  // shipped defaults: the values that go in the file
    const auto pairs = d.Pairs();
    std::vector<char> used(pairs.size(), 0);
    std::string out = IniHeader();
    for (const auto& c : IniCommentTable()) {
        for (std::size_t i = 0; i < pairs.size(); ++i) {
            if (used[i] || pairs[i].first != c.Key) continue;
            if (c.Comment && *c.Comment) out += c.Comment;
            out += pairs[i].first;
            out += '=';
            out += pairs[i].second;
            out += '\n';
            used[i] = 1;
            break;
        }
    }
    bool any_uncommented = false;
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        if (used[i]) continue;
        if (!any_uncommented) {
            out += "\n# --- not described here yet -----------------------------------------------\n";
            any_uncommented = true;
        }
        out += pairs[i].first + "=" + pairs[i].second + "\n";
    }
    return out;
}

// Writes the file only when nothing is there yet: an existing config is never touched (the menu's
// "Save settings" is the way to rewrite one, and it preserves the user's comments and order).
inline bool WriteDefaultIni(const std::string& path) {
    if (std::FILE* exists = std::fopen(path.c_str(), "rb")) {
        std::fclose(exists);
        return false;
    }
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const std::string text = DefaultIniText();
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    return true;
}

}  // namespace aion
