// src/app/Launch.cpp — see Launch.hpp.
#define _CRT_RAND_S   // rand_s: must be asked for before <stdlib.h> is reached
#include "Launch.hpp"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "aion/Logger.hpp"    // MakeBanner, BannerField
#include "aion/Obf.hpp"       // TargetProcessName
#include "aion/Offsets.hpp"   // off::build::VerifiedExeSize

namespace aion {
namespace app {

// ------------------------------------------------------------ launch identity ---
// Windows lets a running image be renamed: the loader holds the file open for delete, so the
// process keeps executing from its original section while the name on disk changes. Doing it on
// every launch is the point - a name recorded in a log, a crash dump or a scanner's cache means
// nothing at the next start.
//
// Two things follow from the rename and are handled here:
//   - GetModuleFileNameW() keeps returning the path the process was *started* with, so the
//     directory - everything the config and data-file lookup needs - is unaffected;
//   - build.bat used to close a running overlay by image name (taskkill /IM aion_external.exe),
//     which can never match a process that renamed itself, so the pid is left in a file next to
//     the exe for it to read instead.
static std::string NarrowUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    char out[MAX_PATH * 2] = {};
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out, sizeof(out), nullptr, nullptr);
    return out;
}

// 16 characters of [0-9A-Za-z] from the OS RNG. A seeded LCG would hand the same sequence to
// every copy of the build, which is exactly what a name-based detector would then match.
static std::string RandomLaunchName() {
    static const char kAlphabet[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::string name(16, '0');
    for (char& c : name) {
        unsigned int v = 0;
        if (rand_s(&v) != 0)  // never seen to fail; a predictable name beats no name
            v = static_cast<unsigned int>(GetTickCount64());
        c = kAlphabet[v % (sizeof(kAlphabet) - 1)];
    }
    return name;
}

LaunchName RenameSelf() {
    LaunchName r;
    wchar_t exe[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        r.Note = "GetModuleFileName failed";
        return r;
    }
    const std::wstring path(exe);
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        r.Note = "no directory in the module path";
        return r;
    }
    const std::wstring dir = path.substr(0, slash);
    r.Name = NarrowUtf8(path.substr(slash + 1));
    if (std::FILE* f = _wfopen((dir + L"\\.aion_external.pid").c_str(), L"wb")) {
        std::fprintf(f, "%lu\n", (unsigned long)GetCurrentProcessId());
        std::fclose(f);
    }
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::string fresh = RandomLaunchName();
        const std::wstring wide(fresh.begin(), fresh.end());
        const std::wstring target = dir + L"\\" + wide + L".exe";
        if (_wcsicmp(target.c_str(), path.c_str()) == 0) continue;  // same name: roll again
        // No MOVEFILE_REPLACE_EXISTING: a collision must fail, never overwrite a file.
        if (MoveFileExW(path.c_str(), target.c_str(), 0)) {
            r.Renamed = true;
            r.Name = fresh + ".exe";
            return r;
        }
        const DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS && err != ERROR_FILE_EXISTS) {
            r.Note = "MoveFileEx failed (error " + std::to_string(err) + ")";
            return r;
        }
    }
    r.Note = "no free name after 8 attempts";
    return r;
}

// The welcome screen: what this build is, which client it expects, which config it reads, and
// what it calls itself this time. Printed through the logger so it cannot interleave with the
// first log lines.
void PrintBanner(const LaunchName& launch, const std::string& cfg_path) {
    char build[128];
    std::snprintf(build, sizeof build, "%s %s  |  MSVC %d.%02d.%05d  |  C++20%s", __DATE__,
                  __TIME__, _MSC_VER / 100, _MSC_VER % 100, _MSC_FULL_VER % 100000,
#if defined(AION_OBFUSK8)
                  "  |  Obfusk8"
#else
                  ""
#endif
    );
    char client[160];
    std::snprintf(client, sizeof client, "%s  |  verified build %llu bytes",
                  TargetProcessName().c_str(),
                  (unsigned long long)off::build::VerifiedExeSize);
    std::vector<BannerField> fields = {
        {"build", build},
        {"client", client},
        {"config", cfg_path},
        {"launched as", launch.Name + (launch.Renamed ? "  (renamed at startup)"
                                                      : "  (kept: " + launch.Note + ")")},
    };
    AION_RAW(MakeBanner(fields));
}

}  // namespace app
}  // namespace aion
