// Strings.hpp — small case-insensitive helpers shared by classification and data lookup.
#pragma once
#include <cstring>
#include <string>
#include <vector>

namespace aion {

// Case-insensitive substring test.
inline bool HasI(const std::string& hay, const char* needle) {
    const std::size_t n = std::strlen(needle);
    if (!n || hay.size() < n) return false;
    for (std::size_t i = 0; i + n <= hay.size(); ++i) {
        std::size_t j = 0;
        for (; j < n; ++j) {
            const char a = hay[i + j], b = needle[j];
            const char la = (a >= 'A' && a <= 'Z') ? char(a + 32) : a;
            const char lb = (b >= 'A' && b <= 'Z') ? char(b + 32) : b;
            if (la != lb) break;
        }
        if (j == n) return true;
    }
    return false;
}

// Any of the patterns is a case-insensitive substring of `s`.
inline bool InList(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& p : v)
        if (!p.empty() && HasI(s, p.c_str())) return true;
    return false;
}

// Case-insensitive equality, same folding as HasI.
inline bool EqualsI(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char x = a[i], y = b[i];
        const char lx = (x >= 'A' && x <= 'Z') ? char(x + 32) : x;
        const char ly = (y >= 'A' && y <= 'Z') ? char(y + 32) : y;
        if (lx != ly) return false;
    }
    return true;
}

// Any of the patterns IS `s` (case-insensitive, whole string). Used by the boss and hunt lists:
// "Contaminated Swarm" must not drag in "Contaminated Swarm Egg", which is a different actor.
inline bool InListExact(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& p : v)
        if (!p.empty() && EqualsI(s, p)) return true;
    return false;
}

}  // namespace aion
