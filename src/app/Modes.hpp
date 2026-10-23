// src/app/Modes.hpp — the headless, read-only modes.
//
// Each mode answers one question about the client from the client's own state, writes what it found
// to a file and a few log lines, and exits. None of them touches the frame loop, and none of them
// writes a byte to the client process.
//
// They share one context and one dispatcher. A mode that was not requested returns -1, which is how
// the orchestrator knows to carry on towards the frame loop.
#pragma once
#include <string>

#include <windows.h>

#include "Cli.hpp"

namespace aion {

class Sdk;
class ExternalMemory;
class EntityCache;
class NameTable;
class D3D11Overlay;

namespace app {

// Everything a mode may read. Passed by reference: the modes are the only readers, and the objects
// outlive them.
struct ModeContext {
    Sdk& sdk;
    ExternalMemory& mem;
    EntityCache& cache;
    NameTable& names;
    D3D11Overlay& gfx;
    HWND overlay;
    const Options& opt;
};

// Runs the first requested headless mode, in the order the command line has always checked them,
// and returns its exit code. -1 means no mode was requested.
int RunDumpModes(ModeContext& mc);

// --dump-atlas: rasterize the font atlas, save it and exit. Called before the menu and the cache
// exist, so it takes only what it needs. Returns the exit code, or -1 when no atlas was requested.
int SaveFontAtlas(D3D11Overlay& gfx, HWND overlay, ExternalMemory& mem, const std::string& path);

}  // namespace app
}  // namespace aion
