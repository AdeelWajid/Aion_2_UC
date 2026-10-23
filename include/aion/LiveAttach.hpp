// LiveAttach.hpp — pick the AION2 instance whose runtime objects are actually live.
//
// A stale bootstrap or an earlier launch can leave a second AION2.exe in the process list; the
// first snapshot entry is not necessarily the world you are playing in. The overlay and recon
// both need the same selection logic.
#pragma once

#include <cstdint>

namespace aion {

class ExternalMemory;
class Sdk;

struct LiveAttachOptions {
    int wait_ms = 60000;
    bool log_candidates = true;
};

// Returns the chosen pid, or 0. On success, `mem` is attached and `sdk.Init(..., log)` succeeded.
std::uint32_t AttachToLiveClient(ExternalMemory& mem, Sdk& sdk, const LiveAttachOptions& opt = {});

// When AttachToLiveClient returns 0, attach to the first readable candidate and run Sdk::Diagnose().
void DiagnoseLiveAttachFailure(const LiveAttachOptions& opt = {});

}  // namespace aion
