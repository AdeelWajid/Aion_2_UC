// Obf.hpp — the names that identify this tool, kept out of the shipped binary's string table.
//
// Three things a scan for this tool would look for, and which no other part of the program has to
// spell out: the client process it attaches to, its own window class and title, and the name of
// the log it writes. They are returned here, decrypted at runtime in the release build (see
// src/obf_strings.cpp), and the log lines that name the client take the value from here too
// instead of repeating the literal.
//
// What is deliberately *not* routed through here: the editable data-file names. `aion.ini` (which
// now also holds the boss/hunt/never-draw lists and the name rules) and `aion_names.txt` (the
// learned-name cache) are the tool's file API, and the generated config (DefaultIni.hpp) has to name
// them for the user to know what to edit - hiding them in the binary while writing them into a file
// on disk would hide them from nobody. The two list files that used to sit beside them
// (`aion_boss.txt`, `aion_target.txt`) are only read once, to import into `aion.ini`.
//
// Values are returned by value: Obfusk8 decrypts a literal into a buffer that lives for the
// statement only, so handing out a pointer into it would dangle.
#pragma once
#include <string>

namespace aion {

// The client process the overlay attaches to, in both forms the code needs: the process scan
// takes a wide name, the log lines print the narrow one.
std::string TargetProcessName();
std::wstring TargetProcessNameW();

// The overlay's own window identity - the cheapest way to find this tool on a running machine is
// to walk the window list looking for it.
std::string OverlayWindowClass();
std::string OverlayWindowTitle();

// The session log written next to the exe when log_to_file is on.
std::string LogFileName();

}  // namespace aion
