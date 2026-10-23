// src/app/Cli.hpp — the command-line surface, in one struct.
//
// Every headless mode the overlay has is selected here and nowhere else. The orchestrator reads the
// flags it acts on out of this struct rather than out of argv, so the frame loop no longer has to
// know that a command line exists at all.
#pragma once
#include <string>
#include <vector>

#include "aion/Logger.hpp"   // LogLevel, ParseLogLevel
#include "aion/Memory.hpp"   // addr_t

namespace aion {
namespace app {

// The defaults are the source's own: they are what a run with no flags at all does today.
struct Options {
    // Headless self-test: render N frames against the live game, save a BMP, exit.
    std::string capture_path;
    int capture_frames = 0;
    int run_seconds = 0;
    std::string atlas_path;

    // Console verbosity, in the logger's own vocabulary. The overlay used to fix the level at
    // Info, so a session that needed the attach or offset detail had no way to ask for it.
    aion::LogLevel log_level = aion::LogLevel::Info;
    bool log_level_set = false;

    // Draws an unmistakable magenta square: proves the overlay really reaches the screen (the
    // composed output), independent of the game's imagery.
    bool pattern_mode = false;

    bool dump_classes_mode = false;
    bool dump_nameplate_mode = false;
    // --dump-relation: per character, the nameplate's marker/guild-relation switch indices and the
    // client's own names for the switch's children - how the friend/foe marker is read off the
    // client instead of guessed. Read-only.
    bool dump_relation_mode = false;
    bool harvest_plates_mode = false;
    bool ai_dump_mode = false;
    int ai_detail = 3;    // controllers/characters dumped in full
    int ai_radius = 150;  // metres; 0 = no distance limit
    std::string find_name_text;
    std::vector<std::string> dump_filters;
    std::string scan_objects_substr, dump_raw_substr, dump_struct_substr, find_row_substr;
    std::string gates_substr;   // --dump-gates: actor-class substring to report on
    bool scan_dodge_mode = false;  // --scan-dodge: dodge telegraph discovery report
    int scan_samples = 6;       // 0 = list every matching object instead of per-class samples
    int dump_count = 3, dump_skip = 0;  // how many actors per --dump-props filter, and skip
    int dump_comps = 8;                 // how many components to dump per actor
    // --diag-shutter N: N frames of read-only measurement of the entity->draw path, then a report.
    // Answers, with numbers: is an actor in the list twice, do two actors share one screen position,
    // does one entity emit more than one box, how far does the drawn geometry sit from the camera it
    // would be drawn with now, and how fast does it move. Writes nothing to the client.
    int shutter_frames = 0;
    std::string shutter_path;
    // --find-hud: locate the client's own-character HUD widget and read its HP/MP bars, then exit.
    bool find_hud_mode = false;
    // --dialog-map: walk AionUISystem's reflected LoadedDialogMap (+0x0060, a MapProperty) - the one
    // container this client is known to hold live widget instances in - to answer whether the real
    // HP/MP bars exist anywhere at all. Read-only; the element layout is established by reflection.
    bool dialog_map_mode = false;
    std::string dump_has;               // only actors with this property set (non-null object)
    addr_t raw_bytes = 0x100;

    // Derived once the whole command line has been read.
    bool capture_mode = false;
};

// Parses argv exactly as the hand-written loop did: a flag that takes a value consumes the next
// argument, an unknown flag is ignored, and a malformed value falls back to atoi/strtoull's own
// answer. `--menu` is deliberately not here - it is applied where the menu exists.
Options ParseArgs(int argc, char** argv);

// Whether the command line selects a headless mode. Those never own the screen, so they are allowed
// to run while an interactive overlay is up: a diagnostic must not be blocked by the session it is
// about to diagnose.
bool Headless(const Options& o);

}  // namespace app
}  // namespace aion
