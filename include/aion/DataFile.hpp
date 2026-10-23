// DataFile.hpp - where the overlay's editable data files live (aion.ini, aion_names.txt).
//
// The overlay is built into build\, and Windows gives a process the working directory of whatever
// launched it: the exe's own folder when it comes from Explorer, the shell's directory otherwise.
// A bare "aion.ini" therefore found the config on one launch and silently ran on built-in
// defaults on another - which looks exactly like a broken setting (empty boss list, no learned
// names, the shipped fonts and distances) and was reported as one. One lookup, used by every data
// file, so all of them agree.
#pragma once
#include <cstdio>
#include <string>
#include <vector>
#if defined(_WIN32)
#  include <windows.h>
#endif

namespace aion {

// True for "C:\x", "C:/x", "\\server\share" and "/x": those are already where the user meant.
inline bool IsAbsoluteDataPath(const std::string& p) {
    if (p.size() >= 2 && p[1] == ':') return true;
    return !p.empty() && (p[0] == '\\' || p[0] == '/');
}

// Next to the exe, then in its parent (build\ -> the project root), then the name as given.
// With nothing there yet, next to the exe - so a file the overlay creates lands beside itself
// instead of in whatever directory it happened to be started from.
inline std::string ResolveDataFile(const std::string& name) {
    if (name.empty() || IsAbsoluteDataPath(name)) return name;
#if defined(_WIN32)
    const auto to_utf8 = [](const std::wstring& w) {
        char out[1024] = {};
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out, sizeof(out), nullptr, nullptr);
        return std::string(out);
    };
    wchar_t exe[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return name;
    std::wstring dir(exe);
    const std::size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return name;
    dir.resize(slash);
    const std::size_t up = dir.find_last_of(L"\\/");
    const std::wstring parent = (up == std::wstring::npos) ? dir : dir.substr(0, up);
    wchar_t leaf[512] = {};
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, leaf, 512);
    for (const std::wstring& d : {dir, parent})
        if (GetFileAttributesW((d + L"\\" + leaf).c_str()) != INVALID_FILE_ATTRIBUTES)
            return to_utf8(d + L"\\" + leaf);
    return to_utf8(dir + L"\\" + leaf);
#else
    return name;
#endif
}

// One pattern per line, '#' comments and blank lines ignored, each line trimmed of surrounding
// whitespace. A missing file is an empty list: this reads the retired aion_boss.txt / aion_target.txt
// (one pattern per line, no comma escaping) to fold what they declared into the ini's own list_boss /
// list_target, and having no file means nothing was declared - not an error, not worth a warning.
inline std::vector<std::string> ReadPatternFile(const std::string& path) {
    std::vector<std::string> out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), f)) {
        std::string s(buf);
        const std::size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos || s[a] == '#') continue;
        const std::size_t b = s.find_last_not_of(" \t\r\n");
        s = s.substr(a, b - a + 1);
        if (!s.empty()) out.push_back(std::move(s));
    }
    std::fclose(f);
    return out;
}

}  // namespace aion
