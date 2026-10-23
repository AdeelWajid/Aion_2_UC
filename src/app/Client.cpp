// src/app/Client.cpp — see Client.hpp.
#include "Client.hpp"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

#include "aion/Config.hpp"
#include "aion/EntityCache.hpp"
#include "aion/Logger.hpp"
#include "aion/Memory.hpp"
#include "aion/Obf.hpp"
#include "aion/Offsets.hpp"
#include "aion/Sdk.hpp"
#include "aion/World.hpp"

namespace aion {
namespace app {

int AttachToClient(const Config& cfg, std::uint32_t& pid_out) {
    // A stale AION2.exe from an earlier launch can sit in the process list beside the live one,
    // and a snapshot returns whichever comes first - attaching to the wrong one reads a client
    // that never initialises anything, which is indistinguishable from a broken install. So
    // consider every instance and attach to the one that actually becomes ready.
    // Track every instance from now on, not only the ones alive at the first scan: a relaunch, or
    // a launcher starting the client twice, can add one later. The one that becomes ready is the
    // live client; the other just sits there mapped, readable and permanently zero.
    std::vector<std::uint32_t> candidates = ExternalMemory::FindPidsByName(TargetProcessNameW().c_str());
    if (candidates.empty() && cfg.sdk_wait_seconds > 0)
        AION_INFO("%s not running yet - waiting up to %d s", TargetProcessName().c_str(),
                 cfg.sdk_wait_seconds);
    for (int waited = 0; candidates.empty() && waited < cfg.sdk_wait_seconds * 1000;
         waited += 500) {
        Sleep(500);
        candidates = ExternalMemory::FindPidsByName(TargetProcessNameW().c_str());
    }
    if (candidates.empty()) {
        AION_ERROR("%s not running. Launch the game first.", TargetProcessName().c_str());
        return 1;
    }
    // The census is detail; the per-candidate state lines below already name the pid that matters,
    // and a candidate the search never reaches has no bearing on the outcome.
    for (std::uint32_t c : candidates)
        AION_DEBUG("%s candidate pid=%u", TargetProcessName().c_str(), c);

    const int wait_ms = cfg.sdk_wait_seconds * 1000;
    std::uint32_t pid = 0;
    // One line per candidate per state change. A silent Attach success next to a failing candidate
    // otherwise reads as "it only ever tries the one that fails": report what happened to each.
    std::unordered_map<std::uint32_t, std::string> cand_state;
    auto note_state = [&](std::uint32_t c, const std::string& s) {
        auto it = cand_state.find(c);
        if (it == cand_state.end() || it->second != s) {
            cand_state[c] = s;
            AION_INFO("candidate pid=%u: %s", c, s.c_str());
        }
    };
    for (int waited = 0;; waited += 250) {
        for (std::uint32_t c : ExternalMemory::FindPidsByName(TargetProcessNameW().c_str())) {
            bool known = false;
            for (std::uint32_t k : candidates) known = known || k == c;
            if (!known) {
                candidates.push_back(c);
                AION_INFO("new %s instance appeared: pid=%u (%zu candidate(s) now)",
                          TargetProcessName().c_str(), c,
                          candidates.size());
            }
        }
        for (std::size_t ci = 0; ci < candidates.size();) {
            const std::uint32_t c = candidates[ci];
            // A bootstrap that has handed off is gone; stop polling it. IsAlive costs nothing and
            // saves an open attempt, and the error code below catches the same case if it races us.
            if (!ExternalMemory::IsAlive(c)) {
                AION_INFO("%s instance pid=%u exited while waiting; dropped",
                              TargetProcessName().c_str(), c);
                candidates.erase(candidates.begin() + (std::ptrdiff_t)ci);
                continue;
            }
            ExternalMemory probe;
            if (!probe.Attach(c, /*log=*/false)) {
                const unsigned err = probe.last_open_error();
                if (err == ERROR_INVALID_PARAMETER) {  // 87: the pid is gone
                    AION_INFO("%s instance pid=%u exited while waiting; dropped",
                              TargetProcessName().c_str(), c);
                    candidates.erase(candidates.begin() + (std::ptrdiff_t)ci);
                    continue;
                }
                // 5 = ERROR_ACCESS_DENIED: the process exists but its anti-cheat refuses us a
                // handle. That is a privilege problem, not a bug - an elevated overlay can read it.
                note_state(c, err == ERROR_ACCESS_DENIED
                                  ? "cannot open (err=5 access denied - protected client; run "
                                    "the overlay elevated)"
                                  : ("cannot open (err=" + std::to_string(err) + ")"));
                ++ci;
                continue;
            }
            Sdk probe_sdk;
            if (probe_sdk.Init(&probe, /*log=*/false)) {
                note_state(c, "attached");
                pid = c;
                break;
            }
            note_state(c, "readable, runtime objects not live yet");
            ++ci;
        }
        if (pid) break;
        if (waited >= wait_ms) break;
        Sleep(250);
        if ((waited + 250) % 1000 == 0)
            AION_INFO("waiting for an instance with live runtime objects (%d/%d ms, %zu candidate(s))",
                      waited + 250, wait_ms, candidates.size());
    }
    if (!pid) {
        AION_ERROR("no %s instance has live runtime objects (%zu candidate(s) seen)",
                   candidates.size());
        // State each instance's runtime objects, then diagnose the first in full.
        for (std::uint32_t c : candidates) {
            ExternalMemory probe;
            if (!probe.Attach(c)) {
                AION_INFO("candidate pid=%u: could not open the process", c);
                continue;
            }
            Sdk probe_sdk;
            probe_sdk.Init(&probe, /*log=*/false);
            AION_INFO("candidate pid=%u: GNames block0=0x%llX GObjects num=%d (not live)", c,
                      (unsigned long long)probe_sdk.rd<addr_t>(probe_sdk.gnames +
                                                               off::name_pool::Blocks),
                      probe_sdk.rd<i32>(probe_sdk.gobjects + off::uobject_array::NumElements));
        }
        ExternalMemory first;
        if (!candidates.empty() && first.Attach(candidates[0])) {
            Sdk first_sdk;
            first_sdk.Init(&first, /*log=*/true);
        }
        return 3;
    }

    pid_out = pid;
    return 0;
}

std::string DumpActorClasses(Sdk& sdk, EntityCache& cache) {
    // The classifier is the cache's own - the copy RulesFromConfig fed it, which every reload and
    // rule change re-feeds. Reading it here instead of taking a second copy is what keeps this
    // census and the boxes in step.
    const ClassRules& rules = cache.rules();
        std::unordered_map<addr_t, std::string> ccache;
        std::unordered_map<std::string, int> hist;
        World w(&sdk);
        Level lv(&sdk);
        auto collect = [&](addr_t lvl) {
            if (!lvl) return;
            for (addr_t a : lv.Actors(lvl)) {
                if (!sdk.mem->valid_ptr(a)) continue;
                const addr_t cls = sdk.mem->ptr_at(a + off::uobject::Class);
                auto it = ccache.find(cls);
                if (it == ccache.end())
                    it = ccache.emplace(cls, sdk.Name(sdk.mem->u32_at(cls + off::uobject::Name)))
                             .first;
                const std::string model = cache.ModelOf(&sdk, a, it->second);
                const EntityType type = ClassifyEntity(it->second, {}, &rules, model);
                char line[320];
                std::snprintf(line, sizeof line, "%-34s %-26s %-9s %s", it->second.c_str(),
                              model.empty() ? "-" : model.c_str(), EntityTypeName(type),
                              AlwaysHidden(it->second, model, &rules)
                                  ? "NEVER DRAWN"
                                  : (type == EntityType::Chest
                                         ? "drawn only while unopened"
                                         : (type == EntityType::Noise ? "SHOW-ALL only" : "")));
                ++hist[line];
            }
        };
        collect(w.PersistentLevel().Addr());
        for (addr_t l : w.Levels()) collect(l);
        std::vector<std::pair<std::string, int>> v(hist.begin(), hist.end());
        std::sort(v.begin(), v.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        // The name is relative to the process working directory, which is not the repo when
        // the overlay is launched from build\ - so the absolute path goes into the file header
        // and into the log line. The timestamp is there because the first question about a
        // dump file is whether it is the one you just asked for.
        const char* const kDumpFile = "aion_classes.txt";
        char abs_path[FILENAME_MAX] = {0};
        if (!_fullpath(abs_path, kDumpFile, sizeof abs_path) || !abs_path[0])
            std::snprintf(abs_path, sizeof abs_path, "%s", kDumpFile);
        const std::time_t now = std::time(nullptr);
        std::tm tm_buf{};
        localtime_s(&tm_buf, &now);
        char when[32];
        std::snprintf(when, sizeof when, "%04d-%02d-%02d %02d:%02d:%02d", tm_buf.tm_year + 1900,
                      tm_buf.tm_mon + 1, tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min,
                      tm_buf.tm_sec);
        if (std::FILE* f = std::fopen(kDumpFile, "wb")) {
            std::fprintf(f, "# Live actors of every streaming level, most frequent first.\n");
            std::fprintf(f, "# written %s to %s\n", when, abs_path);
            std::fprintf(f, "#   count  class                              mesh/identity"
                          "              category   note\n");
            std::fprintf(f, "# NEVER DRAWN    hidden in every mode - extend with never_show= in"
                          " aion.ini\n");
            std::fprintf(f, "# SHOW-ALL only  scenery: drawn only while esp_show_all is on\n");
            std::fprintf(f, "# To fix a category: list_gather= / list_interact= /"
                          " list_monster= ...\n");
            std::fprintf(f, "# To fix a label:    name_map=mesh=Display Name (both take"
                          " precedence over the patterns)\n");
            for (const auto& p : v) std::fprintf(f, "%6d  %s\n", p.second, p.first.c_str());
            std::fclose(f);
            AION_INFO("dumped %zu distinct actor classes to %s (levels=%zu, written %s)", v.size(),
                      abs_path, w.Levels().size(), when);
            char ok[512];
            std::snprintf(ok, sizeof ok, "%zu classes -> %s", v.size(), abs_path);
            return ok;
        } else {
            AION_WARN("could not write %s", abs_path);
            char bad[512];
            std::snprintf(bad, sizeof bad, "could not write %s", abs_path);
            return bad;
        }
}
}  // namespace app
}  // namespace aion
