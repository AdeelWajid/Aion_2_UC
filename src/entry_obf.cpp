// entry_obf.cpp — the shipped exe's entry point.
//
// The real entry is aion::RunOverlay() in external_main.cpp; this file exists so the entry point is
// one small translation unit with its own compile settings, and it is where Obfusk8's _main macro
// belongs if it can be used.
//
// It cannot, not in this build. _main's prologue is not side-effect free: with it in the link, the
// obfuscated literals in obf_strings.cpp decrypt to garbage - the overlay looked for a process whose
// name was binary junk and therefore never found a running client. Measured both ways, twice each:
//
//   entry wrapped in _main  -> TargetProcessName() returns bytes like a4 c4 9e d8 94 4b 20 f4 ...
//   plain entry, same objects -> "AION2.exe" / "AionExternalOverlay" / "aion_external.log"
//
// The string/library objects were identical in both links; only the entry differed. So the obfuscated
// strings stay (they are the part a scan actually keys on, and they are verified by build.bat), and
// the entry point stays readable until the interaction is understood.
//
// With AION_OBFUSK8 undefined this file is the same plain entry, so one source tree builds both ways.
namespace aion {
// Defined in external_main.cpp.
int RunOverlay(int argc, char** argv);
}  // namespace aion

int main(int argc, char** argv) { return aion::RunOverlay(argc, argv); }
