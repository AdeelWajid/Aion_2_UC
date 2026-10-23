// src/app/Frame.hpp — the frame loop, and the order the ESP's pipeline runs in.
//
// The order below is the contract, and the one place it is written down:
//
//   1. FollowClientWindow   the overlay tracks the client's rect, or idles when the game is behind
//   2. ReadState            local pawn + camera, through the same chain the ESP has always used
//   3. ApplyMenuVerbs       reload/save/dump-classes, the rules rebuild, INSERT, the menu's own frame
//   4. UpdateEntities       EntityCache::UpdateBulk, then the camera re-read and Reproject: after
//                           this the entity list is final - culled, classified, projected
//   5. DrawEsp              EspRenderer over that same list
//   6. EndFrame             present, pace, copy the menu's numbers, and honour soak/capture exits
//
// Step 4 must precede step 5: nothing is read twice, and nothing is re-classified. This file exists
// so that order is written down in one place instead of being spread through an orchestrator that
// also owns windows and config.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include <windows.h>

#include "Cli.hpp"

namespace aion {

struct Config;
class Sdk;
class ExternalMemory;
class EntityCache;
class NameTable;
class Menu;
class ImGuiLayer;
class D3D11Overlay;
class CameraExtractor;
class Projection;
class AutoDodge;
class Reflection;

namespace app {

// Everything the frame needs, by reference: the objects are owned by the orchestrator and outlive
// the loop. The two callbacks are the orchestrator's own closures, so the loop can drive the same
// code the menu's callbacks do without this module knowing what a menu callback is.
struct FrameContext {
    Sdk& sdk;
    ExternalMemory& mem;
    Config& cfg;
    CameraExtractor& camx;
    Projection& proj;
    EntityCache& cache;
    NameTable& names;
    Menu& menu;
    AutoDodge& auto_dodge;
    Reflection& refl;
    ImGuiLayer& imgui;
    D3D11Overlay& gfx;
    HWND overlay;
    std::uint32_t pid;
    const Options& opt;
    int present_fps;
    int display_hz;
    // Fixed when the window and swapchain were created, so a reload can only report that they need
    // a restart rather than appearing to have applied them.
    bool dcomp_at_start;
    bool vsync_at_start;
    bool& rules_dirty;
    std::function<void()> apply_rules;
};

// Runs until the overlay is asked to quit, or until a soak/capture run has finished.
void RunFrameLoop(FrameContext& fc);

}  // namespace app
}  // namespace aion
