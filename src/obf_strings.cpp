// obf_strings.cpp — the one definition site of the tool's identifying names (Obf.hpp).
//
// Obfusk8 is header-only and this is the only translation unit compiled with it, so the cost of
// the library stays in one small file instead of every unit that mentions a file name. With
// AION_OBFUSK8 undefined the same functions return plain literals: that is what the verification
// targets and any debug build use, and the values are identical either way.
//
// Each literal is split into pieces of at most 12 characters, which is not cosmetic:
// AES8 encrypts a literal with AES at compile time only when it fits in a single 16-byte block.
// MSVC does not fold the multi-block path, and when it does not, the macro's runtime encrypt()
// reads the plaintext straight out of .rdata - the string ends up in the image exactly as if the
// macro had never been written (measured: /O2, every literal of 16 bytes or more survived a scan
// of the produced exe, every one of 15 bytes or less did not). So the pieces are short and joined
// at runtime, and build.bat scans the finished exe for the names and fails the build if one leaks.
#include <string>
#include <windows.h>

#include "aion/Obf.hpp"

#if defined(AION_OBFUSK8)
#include "Obfusk8/Instrumentation/materialization/state/Obfusk8Core.hpp"
#define AION_LITERAL(text) (OBFUSCATE_STRING(text).c_str())
#else
// Still a function call through the same definitions, so nothing else in the program changes when
// the obfuscator is switched off.
#define AION_LITERAL(text) (text)
#endif

namespace aion {
namespace {
std::wstring Widen(const char* s) {
    if (!s || !*s) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring out(static_cast<std::size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
    return out;
}
// Joins the pieces of a split name. The concatenated form exists only in memory.
std::string Join(const char* a, const char* b) { return std::string(a) + b; }
}  // namespace

std::string TargetProcessName() { return Join(AION_LITERAL("AION2"), AION_LITERAL(".exe")); }
std::wstring TargetProcessNameW() { return Widen(TargetProcessName().c_str()); }

std::string OverlayWindowClass() {
    return Join(AION_LITERAL("AionExternal"), AION_LITERAL("Overlay"));
}
std::string OverlayWindowTitle() { return Join(AION_LITERAL("AION2"), AION_LITERAL(" overlay")); }

std::string LogFileName() { return Join(AION_LITERAL("aion_external"), AION_LITERAL(".log")); }

}  // namespace aion
