// src/app/Launch.hpp — the process's own identity at runtime: what it is called, and the welcome
// banner that reports it.
//
// Windows lets a running image be renamed: the loader holds the file open for delete, so the
// process keeps executing from its original section while the name on disk changes. Doing it on
// every launch is the point - a name recorded in a log, a crash dump or a scanner's cache means
// nothing at the next start.
//
// Two things follow from the rename, and both are handled here:
//   - GetModuleFileNameW() keeps returning the path the process was *started* with, so the
//     directory - everything the config and data-file lookup needs - is unaffected;
//   - build.bat used to close a running overlay by image name (taskkill /IM aion_external.exe),
//     which can never match a process that renamed itself, so the pid is left in a file next to the
//     exe for it to read instead.
#pragma once
#include <string>

namespace aion {
namespace app {

struct LaunchName {
    std::string Name;      // the name the process is running under now
    bool Renamed = false;  // false: it still carries the name it was launched with
    std::string Note;      // why not, when Renamed is false
};

// Writes the pid file beside the exe and renames the image. Never fatal: on any failure the
// returned LaunchName carries the name it kept and the reason it kept it.
LaunchName RenameSelf();

// The welcome screen: what this build is, which client it expects, which config it reads, and what
// it calls itself this time. Printed through the logger so it cannot interleave with the first log
// lines.
void PrintBanner(const LaunchName& launch, const std::string& cfg_path);

}  // namespace app
}  // namespace aion
