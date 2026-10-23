// src/app/Cli.cpp — see Cli.hpp.
//
// The loop below is the original one, moved: every option is now a field of Options, so the
// branches are byte-identical apart from the qualification.
#include "Cli.hpp"

#include <cstdlib>
#include <string>

namespace aion {
namespace app {

Options ParseArgs(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--capture" && i + 1 < argc) opt.capture_path = argv[++i];
        else if (std::string(argv[i]) == "--frames" && i + 1 < argc) opt.capture_frames = atoi(argv[++i]);
        else if (std::string(argv[i]) == "--seconds" && i + 1 < argc) opt.run_seconds = atoi(argv[++i]);
        else if (std::string(argv[i]) == "--dump-atlas" && i + 1 < argc) opt.atlas_path = argv[++i];
        // Draws an unmistakable magenta square: proves the overlay really reaches
        // the screen (the composed output), independent of the game's imagery.
        else if (std::string(argv[i]) == "--pattern") opt.pattern_mode = true;
        // Console verbosity. --verbose is the short form for the level that shows stage detail.
        else if (std::string(argv[i]) == "--verbose") {
            opt.log_level = LogLevel::Debug;
            opt.log_level_set = true;
        } else if (std::string(argv[i]) == "--log-level" && i + 1 < argc) {
            const char* value = argv[++i];
            if (ParseLogLevel(value, opt.log_level)) {
                opt.log_level_set = true;
            } else {
                std::fprintf(stderr, "aion: --log-level: unknown level '%s' (trace|debug|info|"
                                     "warn|error|off)\n", value);
            }
        }
        // Headless: list every live actor class + mesh to aion_classes.txt and exit.
        else if (std::string(argv[i]) == "--dump-classes") opt.dump_classes_mode = true;
        // Headless, read-only: for every actor whose class name contains a substring, dump the
        // gates the client owns - bHidden, NetCullDistanceSquared, the mesh component's
        // CachedMaxDrawDistance/MinDrawDistance, SkinnedMeshComponent::bRecentlyRendered and
        // VisibilityBasedAnimTickOption - plus distance from the local pawn, to aion_gates.txt.
        // Answers "why is the game not drawing this" and "where is the render cut" from the
        // client's own state instead of from a distance guess.
        else if (std::string(argv[i]) == "--dump-gates" && i + 1 < argc) opt.gates_substr = argv[++i];
        else if (std::string(argv[i]) == "--scan-dodge") opt.scan_dodge_mode = true;
        // Headless: resolve actor display names from their nameplate widget.
        else if (std::string(argv[i]) == "--dump-nameplate") opt.dump_nameplate_mode = true;
        // Headless, read-only: per character, the nameplate's friend/foe marker switch - its active
        // index and the client's own names for the switch's children.
        else if (std::string(argv[i]) == "--dump-relation") opt.dump_relation_mode = true;
        // Headless: resolve every nameplate the client has built and merge the results into
        // the learned name table (aion_names.txt). Run it in a busy area.
        else if (std::string(argv[i]) == "--harvest-plates") opt.harvest_plates_mode = true;
        // Headless: search the objects that could hold an entity's name (data table rows,
        // string tables, object names) for a literal, e.g. --find-name "Lost Specter".
        else if (std::string(argv[i]) == "--find-name" && i + 1 < argc)
            opt.find_name_text = argv[++i];
        // Headless, read-only: survey the NPC AI object graph - the live controllers, their
        // brain/blackboard/perception sub-objects, the pawn's AI-interest components, and the
        // function names that look like attack/aggro/target on the controller and mob classes.
        else if (std::string(argv[i]) == "--dump-ai") opt.ai_dump_mode = true;
        else if (std::string(argv[i]) == "--ai-detail" && i + 1 < argc) opt.ai_detail = atoi(argv[++i]);
        else if (std::string(argv[i]) == "--ai-radius" && i + 1 < argc) opt.ai_radius = atoi(argv[++i]);
        // Headless: dump every reflected property of the first few actors per filter whose
        // class contains a substring (e.g. --dump-props BP_EnvObj,Nameplate,BPCh_Npc).
        else if (std::string(argv[i]) == "--dump-props" && i + 1 < argc) {
            const std::string spec = argv[++i];
            for (std::size_t b = 0; b < spec.size();) {
                std::size_t e = spec.find(',', b);
                if (e == std::string::npos) e = spec.size();
                if (e > b) opt.dump_filters.emplace_back(spec.substr(b, e - b));
                b = e + 1;
            }
        }
        // Headless: histogram of loaded object class names matching a substring, e.g.
        // --scan-objects Table. Answers "does the client hold this data as UObjects".
        else if (std::string(argv[i]) == "--scan-objects" && i + 1 < argc)
            opt.scan_objects_substr = argv[++i];
        else if (std::string(argv[i]) == "--scan-samples" && i + 1 < argc)
            opt.scan_samples = atoi(argv[++i]);
        else if (std::string(argv[i]) == "--dump-count" && i + 1 < argc)
            opt.dump_count = atoi(argv[++i]);
        else if (std::string(argv[i]) == "--dump-skip" && i + 1 < argc)
            opt.dump_skip = atoi(argv[++i]);
        else if (std::string(argv[i]) == "--dump-comps" && i + 1 < argc)
            opt.dump_comps = atoi(argv[++i]);
        else if (std::string(argv[i]) == "--dump-has" && i + 1 < argc)
            opt.dump_has = argv[++i];
        // Headless: hex + object-pointer interpretation of the first few objects whose
        // class or name contains the substring, for working out a native layout.
        else if (std::string(argv[i]) == "--dump-raw" && i + 1 < argc)
            opt.dump_raw_substr = argv[++i];
        // Headless: reflected fields of a struct or class, addressed by name. A data table
        // is described by its RowStruct, so this lists the columns it holds.
        else if (std::string(argv[i]) == "--dump-struct" && i + 1 < argc)
            opt.dump_struct_substr = argv[++i];
        // Headless: every data table row whose key matches, with the strings inside it.
        else if (std::string(argv[i]) == "--find-row" && i + 1 < argc)
            opt.find_row_substr = argv[++i];
        else if (std::string(argv[i]) == "--raw-bytes" && i + 1 < argc)
            opt.raw_bytes = static_cast<addr_t>(strtoull(argv[++i], nullptr, 0));  // 0x.. ok
        // Headless: N frames of read-only measurement of the entity list and the draw path, for the
        // "one entity renders as two boxes" report. See ModeShutterDiag.
        else if (std::string(argv[i]) == "--diag-shutter" && i + 1 < argc)
            opt.shutter_frames = atoi(argv[++i]);
        else if (std::string(argv[i]) == "--diag-out" && i + 1 < argc) opt.shutter_path = argv[++i];
        // Headless: find the client's own-character HUD widget and read its HP/MP bars.
        else if (std::string(argv[i]) == "--find-hud") opt.find_hud_mode = true;
        // Headless: walk AionUISystem's LoadedDialogMap by reflection (the last HP/MP route).
        else if (std::string(argv[i]) == "--dialog-map") opt.dialog_map_mode = true;
    }
    if (opt.capture_frames <= 0) opt.capture_frames = opt.capture_path.empty() ? 0 : 60;
    opt.capture_mode = !opt.capture_path.empty();
    return opt;
}

bool Headless(const Options& o) {
    return o.capture_mode || o.run_seconds > 0 || o.pattern_mode ||
           o.shutter_frames > 0 || o.find_hud_mode || o.dialog_map_mode ||
           o.dump_classes_mode || o.dump_nameplate_mode || o.dump_relation_mode ||
           o.harvest_plates_mode || o.ai_dump_mode ||
           !o.atlas_path.empty() || !o.gates_substr.empty() || !o.find_name_text.empty() ||
           !o.dump_filters.empty() || !o.scan_objects_substr.empty() || !o.dump_raw_substr.empty() ||
           !o.dump_struct_substr.empty() || !o.find_row_substr.empty();
}

}  // namespace app
}  // namespace aion
