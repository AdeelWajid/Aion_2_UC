// src/app/Window.hpp — the overlay's Win32 surface: the client-window probe, the DPI model, the
// window class, and the message handling the overlay answers.
//
// Split out of src/external_main.cpp so the orchestrator carries no Win32 plumbing. The frame loop
// needs two things from here (the client rect, and whether the window asked us to quit) and nothing
// else in the program needs the module at all.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>

#include <windows.h>

#include "aion/Config.hpp"    // Config
#include "aion/Overlay.hpp"   // FontConfig

namespace aion {

class ImGuiLayer;

namespace app {

// The client's top-level window for `pid`, or null while it has none. Enumerating the window list
// is the only way to pair a process with the window the user is actually looking at.
HWND FindGameWindow(std::uint32_t pid);

// The client's client-area rect in screen coordinates. False when the window has no client area
// yet (minimised, or created but not shown).
bool GameClientRect(HWND game, RECT& out);

// Once per process, and only while this process has no windows of its own: adopt the client
// monitor's DPI awareness. A failure leaves the default in place instead of assuming it worked.
void AdoptClientDpiModel(HWND game);

// Config -> overlay font settings. Glyphs are rasterized at exactly these sizes; client_h is the
// height of the surface the labels are drawn into, which is what the auto label size follows.
FontConfig FontsFromConfig(const Config& c, int client_h);

// Registers the window class and creates the layered, click-through, topmost overlay window.
// `client_rc` places and sizes it on the client before the device exists (null leaves the
// placeholder size). `exclusive` is false for the headless modes (a capture, a diagnostic): they need
// a surface to draw into, but they do not own the screen, so a running overlay must not block them.
// Null on failure, which the caller treats as fatal.
HWND CreateOverlayWindow(const RECT* client_rc, bool exclusive = true);

// Whether an overlay window already exists on this machine. The class name is fixed for the build
// (Obf.hpp), so a window scan finds another instance of this tool without any shared state - which
// is what a second launch has to be told about: two instances are two independently-timed copies of
// the same boxes, identical while the camera is still and split the moment it moves.
bool OverlayInstanceRunning();

// The overlay's message handler is the only writer of this flag; the frame loop is the only reader.
bool ShouldQuit();
void RequestQuit();

// The ImGui layer the window messages are forwarded to. Set when the layer initialises, cleared on
// shutdown: the WndProc is a free function, so this module owns the pointer it reaches the layer
// through.
void SetImGuiLayer(ImGuiLayer* layer);
ImGuiLayer* ActiveImGuiLayer();

}  // namespace app
}  // namespace aion
