// src/app/Client.hpp — finding the live client, and reading the whole object list it holds.
//
// A stale instance of the client from an earlier launch can sit in the process list beside the live
// one, and a snapshot returns whichever comes first - attaching to the wrong one reads a client that
// never initialises anything, which is indistinguishable from a broken install. So every instance is
// considered and only the one that actually becomes ready is attached to.
#pragma once
#include <cstdint>
#include <string>

namespace aion {

class Sdk;
struct Config;
class EntityCache;

namespace app {

// Attaches to nothing: it finds the instance whose runtime objects are live and returns its pid.
// The return value is the process exit code the orchestrator should use when it is non-zero, so the
// two distinct failures stay distinct: 1 = the client is not running at all, 3 = it is running but
// never became live (and every candidate is then diagnosed in the log).
int AttachToClient(const Config& cfg, std::uint32_t& pid);

// Dump every live actor's class (all streaming levels) to a file so unknown classes (e.g. chests)
// can be identified in-game and pinned via aion.ini. Returns the one-line status for the log and the
// menu's status row.
std::string DumpActorClasses(Sdk& sdk, EntityCache& cache);

}  // namespace app
}  // namespace aion
