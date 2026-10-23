// src/app/Client.cpp — see Client.hpp.
#include "Client.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

#include "aion/Config.hpp"
#include "aion/EntityCache.hpp"
#include "aion/LiveAttach.hpp"
#include "aion/Logger.hpp"
#include "aion/Memory.hpp"
#include "aion/Obf.hpp"
#include "aion/Offsets.hpp"
#include "aion/Sdk.hpp"
#include "aion/World.hpp"

namespace aion {
namespace app {

int AttachToClient(const Config& cfg, std::uint32_t& pid_out) {
    std::vector<std::uint32_t> candidates =
        ExternalMemory::FindPidsByName(TargetProcessNameW().c_str());
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

    LiveAttachOptions opt;
    opt.wait_ms = cfg.sdk_wait_seconds * 1000;
    ExternalMemory probe_mem;
    Sdk probe_sdk;
    const std::uint32_t pid = AttachToLiveClient(probe_mem, probe_sdk, opt);
    if (!pid) {
        AION_ERROR("no %s instance has live runtime objects", TargetProcessName().c_str());
        DiagnoseLiveAttachFailure(opt);
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
    const char* const kDumpFile = "aion_classes.txt";
    char abs_path[FILENAME_MAX] = {0};
    if (!_fullpath(abs_path, kDumpFile, sizeof abs_path) || !abs_path[0])
        std::snprintf(abs_path, sizeof abs_path, "%s", kDumpFile);
    const std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
    localtime_s(&tm_buf, &now);
    char when[32];
    std::snprintf(when, sizeof when, "%04d-%02d-%02d %02d:%02d:%02d", tm_buf.tm_year + 1900,
                  tm_buf.tm_mon + 1, tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
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
    }
    AION_WARN("could not write %s", abs_path);
    char bad[512];
    std::snprintf(bad, sizeof bad, "could not write %s", abs_path);
    return bad;
}

}  // namespace app
}  // namespace aion
