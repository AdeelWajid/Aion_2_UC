#include "aion/LiveAttach.hpp"

#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#endif

#include "aion/Logger.hpp"
#include "aion/Memory.hpp"
#include "aion/Obf.hpp"
#include "aion/Offsets.hpp"
#include "aion/Sdk.hpp"

namespace aion {

std::uint32_t AttachToLiveClient(ExternalMemory& mem, Sdk& sdk, const LiveAttachOptions& opt) {
    std::vector<std::uint32_t> candidates =
        ExternalMemory::FindPidsByName(TargetProcessNameW().c_str());
    if (candidates.empty()) return 0;

    std::unordered_map<std::uint32_t, std::string> cand_state;
    auto note_state = [&](std::uint32_t c, const std::string& s) {
        if (!opt.log_candidates) return;
        auto it = cand_state.find(c);
        if (it == cand_state.end() || it->second != s) {
            cand_state[c] = s;
            AION_INFO("candidate pid=%u: %s", c, s.c_str());
        }
    };

    const int poll_ms = 250;
    for (int waited = 0;; waited += poll_ms) {
        for (std::uint32_t c : ExternalMemory::FindPidsByName(TargetProcessNameW().c_str())) {
            bool known = false;
            for (std::uint32_t k : candidates) known = known || k == c;
            if (!known) {
                candidates.push_back(c);
                if (opt.log_candidates)
                    AION_INFO("new %s instance: pid=%u (%zu candidate(s))",
                              TargetProcessName().c_str(), c, candidates.size());
            }
        }

        for (std::size_t ci = 0; ci < candidates.size();) {
            const std::uint32_t c = candidates[ci];
            if (!ExternalMemory::IsAlive(c)) {
                if (opt.log_candidates)
                    AION_INFO("%s pid=%u exited; dropped", TargetProcessName().c_str(), c);
                candidates.erase(candidates.begin() + static_cast<std::ptrdiff_t>(ci));
                continue;
            }

            ExternalMemory probe;
            if (!probe.Attach(c, /*log=*/false)) {
                const unsigned err = probe.last_open_error();
                if (err == ERROR_INVALID_PARAMETER) {
                    candidates.erase(candidates.begin() + static_cast<std::ptrdiff_t>(ci));
                    continue;
                }
                note_state(c, err == ERROR_ACCESS_DENIED
                                  ? "cannot open (err=5 access denied - run elevated)"
                                  : ("cannot open (err=" + std::to_string(err) + ")"));
                ++ci;
                continue;
            }

            Sdk probe_sdk;
            if (probe_sdk.Init(&probe, /*log=*/false)) {
                note_state(c, "attached (runtime objects live)");
                if (!mem.Attach(c, opt.log_candidates)) return 0;
                sdk.Init(&mem, opt.log_candidates);
                return c;
            }
            note_state(c, "readable, runtime objects not live yet");
            ++ci;
        }

        if (waited >= opt.wait_ms) break;
#if defined(_WIN32)
        Sleep(static_cast<DWORD>(poll_ms));
#endif
        if (opt.log_candidates && (waited + poll_ms) % 1000 == 0)
            AION_INFO("waiting for live runtime objects (%d/%d ms, %zu candidate(s))",
                      waited + poll_ms, opt.wait_ms, candidates.size());
    }
    return 0;
}

void DiagnoseLiveAttachFailure(const LiveAttachOptions& opt) {
    const std::vector<std::uint32_t> candidates =
        ExternalMemory::FindPidsByName(TargetProcessNameW().c_str());
    for (std::uint32_t c : candidates) {
        ExternalMemory probe;
        if (!probe.Attach(c, /*log=*/false)) continue;
        Sdk probe_sdk;
        probe_sdk.Init(&probe, /*log=*/false);
        AION_INFO("candidate pid=%u: GNames block0=0x%llX GObjects num=%d (not live)", c,
                  (unsigned long long)probe_sdk.rd<addr_t>(probe_sdk.gnames + off::name_pool::Blocks),
                  probe_sdk.rd<i32>(probe_sdk.gobjects + off::uobject_array::NumElements));
    }
    if (candidates.empty()) return;
    ExternalMemory first;
    if (first.Attach(candidates[0], opt.log_candidates)) {
        Sdk first_sdk;
        first_sdk.Init(&first, /*log=*/true);
    }
}

}  // namespace aion
