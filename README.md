# AION2 External UE Runtime & Anti-Cheat Bypass Framework

*by ZixMatrix*

A standalone external runtime framework for **AION2.exe** (Steam build, PID-attached), focused on Unreal Engine 5 reverse engineering, anti-cheat analysis, bypass research, and ESP development.

External architecture using `ReadProcessMemory` for runtime inspection and a custom layered, click-through D3D11 overlay over the game's borderless window.

## Core Modules

- **Anti-Cheat Analysis:** Identify detection mechanisms, integrity checks, process/module monitoring, debugging detection, kernel drivers, callbacks, and communication channels.
- **Bypass Research:** Analyze enforcement logic, detection surfaces, trust boundaries, and potential weaknesses; validate findings through controlled testing.
- **UE5 Runtime:** Analyze `GNames`, `GObjects`, `GWorld`, `GEngine`, `GUObjectArray`, actors, components, and reflection structures.
- **ESP:** Actor enumeration, entity classification, player tracking, health/status data, skeletal ESP, distance filtering, and world-to-screen projection.
- **Memory Engine:** Reliable process attachment, validated memory reads, pointer-chain resolution, signature scanning, offset management, and build detection.
- **Overlay:** D3D11 rendering, click-through interaction, entity labels, bounding boxes, skeletons, distance indicators, and configurable visibility filters.
- **Optimization:** Batched reads, caching, efficient actor updates, minimal allocations, multithreading where beneficial, and low-latency rendering.
- **Reliability:** Strict validation, graceful error handling, structured logging, crash prevention, and automatic compatibility checks.

## Requirements

- Analyze before implementing.
- Verify every offset and structure against the current build.
- Prioritize working implementations over assumptions.
- Keep modules independent, maintainable, and performance-focused.
- Test changes and report actual results.
- Avoid unnecessary complexity and redundant operations.

**Goal:** A fast, stable, modular external UE5 framework with comprehensive anti-cheat analysis, bypass research, and ESP capabilities.

## Components

| Target | Purpose |
|---|---|
| `build/aion_external.exe` | The product: external ESP overlay + INSERT menu. This is the only target the release build produces. |
| `build/aion_recon.exe` | Read-only live validator / reconnaissance dump (offsets, reflection, world graph, camera, entities, symbol search). |
| `build/overlay_test.exe` | Standalone D3D11 verification (synthetic camera/entities, no game): asserts box geometry, range units and the distance origin against a recording `IOverlay`, then writes `overlay_test.bmp` with the ESP, menu and performance panel in it. Exit code 1 on a failed assertion. |

`aion_recon.exe` and `overlay_test.exe` are verification tools, built separately by
`build_test.bat`; they are not part of the release build and have no obfuscation.

## Usage

```
build\aion_recon.exe                 # validate offsets against the running game
build\aion_external.exe              # run the external overlay (INSERT toggles menu)
build\aion_external.exe --menu       # start with the menu open
build\aion_external.exe --capture out.bmp --frames 60   # headless self-test, saves a BMP
build\aion_external.exe --seconds 30    # soak run: logs camera/entity/handle-reopen health
build\aion_external.exe --verbose        # the same run, with the stage detail (raw globals, attach)
build\aion_external.exe --log-level warn # trace|debug|info|warn|error|off; error = failures only
```

**Verbosity.** The console prints one heading per phase (*target*, *initialization*, *overlay*,
*session*) and one line per event, ending in a single session summary. `--verbose` (equivalent to
`--log-level debug`) adds the raw runtime-object state, the attach internals and the internal
diagnostics log - the detail the default view deliberately keeps out of the way; `--log-level
error` reduces a session to its failures. Headings are printed only when the level would show the
lines under them, so a quiet session is never a list of empty headings.

**Launch identity.** `aion_external.exe` renames itself on startup to 16 random characters
(`[0-9A-Za-z]`, from the OS RNG) and keeps running under that name, so a name recorded in a log,
a crash dump or a scanner's cache means nothing at the next start. Windows allows renaming a
running image, which is what makes it possible at all. Two consequences:

* `build\aion_external.exe` is *consumed* by a launch - the file that ran is now
  `build\<random>.exe`. Rebuild (or copy the exe to a fresh name) before launching again.
* the running session's pid is left in `build\.aion_external.pid`, because `taskkill /IM
  aion_external.exe` can no longer match it. `build.bat` reads that file and closes the session
  before it links, which is what keeps "close it and build again" true.
If the rename fails (read-only folder, a lock) the overlay logs why and runs under its original
name - it is never fatal.

**Menu (`INSERT`)**: the menu is ImGui, vendored under `third_party/imgui` and built on the
overlay's own device, so it is drawn into the same frame and the same present - no second window and
no second swapchain. Three columns: the rail on the left is what you switched to (**ESP** /
**CONFIG** / **DEBUG**), the middle is that pane, and the right is the session - what this overlay is
attached to (game, process, pid, base address, verified image size, engine, present path, display
rate, its own build date) and its health (attach, elevation, camera, fps, kept/drawn, handle
re-opens). The session column sits under every pane because "what am I attached to" should not need
a tab of its own, and it holds only the slow-changing facts: the per-frame numbers stay in **DEBUG**.

Where a setting lives is a rule rather than a decoration: **ESP** changes the picture (the master
switch, the category grid with each category's live count, the element rows with their colours, the
lines and markers, the limits); **CONFIG** is what is written to `aion.ini` or set once per session
(the lists, the model-to-name rules, the per-class patterns behind a collapsing header, the display
knobs); **DEBUG** is read-only numbers (frame timings, entity counts, the camera, the name table, the
per-category table). Nothing appears twice and nothing gets a pane of its own, which is what keeps
the rail from growing an entry per feature. Row labels stay short because the section heading above
them carries the context, units live in the values (`Corner 22%`, `Thickness 0.20 px`), and an
element's colour sits on the element's own row. ImGui's keyboard navigation is enabled, so `INSERT`,
the arrows and `ENTER` drive it with no mouse.

**Lists** are edited as text: one pattern per line, and a comma separates entries too, which is the
form the ini itself uses (the value is stored on one line - a value spanning lines would be cut in
two when the file is read back). A pattern is matched against the model, the object name, the class
and the learned on-screen name, so typing the name the client shows is enough. When that name is
unknown, **CONFIG -> Discover** lists what the overlay has actually seen - display name, model,
class, category, and how many are live - and adds the ticked rows to the list you pick, by model,
which is the key that survives a rename.

Opening the menu is the one moment the overlay stops being click-through: it drops
`WS_EX_TRANSPARENT` and `WS_EX_NOACTIVATE`, takes the foreground from the client and frees the
pointer, because text fields and a usable cursor need real window messages and no ALT press. Taking
the foreground is the one thing Windows refuses to a background process, so it is attempted twice:
first by sharing the foreground thread's input queue, then, if that is still refused, after a
synthetic ALT press, which is the documented way to become the process that owns the last input
event. The result is logged either way - a menu without the foreground receives no keys, and its
first click only activates it, which is exactly what an X that "does nothing" looks like. Closing
hands the foreground and the pointer back. While it is open `skip_when_unfocused` is suspended -
editing is precisely what makes the game unfocused, and the rule would otherwise hide the window
being typed into.

The three actions report back on the status line inside the panel, which is the only place a
double-clicked overlay can say anything: the dump writes a live class histogram to a file, the
reload re-reads `aion.ini` (and `aion_names.txt`) and re-derives the rules so a monster name can be
added mid-session (the metadata cache is dropped too, otherwise the new rules would only reach each
actor when it next came up for revalidation), and the save writes every setting back to `aion.ini`.
Keys fixed when the window is created (`overlay_dcomp`, `overlay_vsync`) still need a restart, and
the reload says so rather than appearing to have worked.

### When it is on screen

`skip_when_unfocused` (menu: **Hide when unfocused**, on by default) hides the overlay window
outright while the game is not the window in front - alt-tabbing to a browser, a launcher or the
desktop - instead of leaving ESP boxes floating over the other app, and brings it back when the
game does. "In front" is decided by process id, not by window handle: the client is one UE window
among several of its own process, and a child coming to the front does not change the window the
user is looking at. A minimised game counts as not in front, which is the case this exists for.
The read/render/present loop is skipped while hidden, so an alt-tab costs nothing. The config file
wins over the default: a copied `aion.ini` carrying `skip_when_unfocused=0` turns the behaviour off,
which is how the overlay came to sit on top of other windows in the first place.

## Copying to another machine

One file: `build\aion_external.exe`. Offsets are compiled in, so there is no SDK/offset file to
carry, and the image links the CRT statically - `dumpbin /dependents` shows only inbox OS libraries
(`WINMM`, `ADVAPI32`, `d3d11`, `D3DCOMPILER_47`, `dcomp`, `GDI32`, `USER32`, `KERNEL32`,
`IMM32`, `SHELL32` - the last two for the IME and `ShellExecuteW`): no VC++
redistributable, no Python, no `AionRuntime.dll` (that one is a leftover from the injection-based
attempt and is not imported). What matters:

* **Settings.** `aion.ini` (settings, class lists and name rules) and the data files it points at
  travel with the exe: copy them next to it or into its parent folder, which is where the exe looks
  first. With neither, the first run writes `aion.ini` beside the exe - every key, at its shipped
  default, each with the comment that explains it - and runs on that. `aion_names.txt` is written
  beside the exe as names are learned from the game; it is a cache, and everything that decides what
  is shown lives in `aion.ini`, so one file carries the settings. An install that still has the old
  `aion_boss.txt` / `aion_target.txt` list files has them folded into `list_boss=` / `list_target=`
  once, on the first run. A fresh machine starts empty, with model names in the labels instead of
  localized ones, and fills up as you play.
* **Elevation.** The log says which it got (`elevated=0/1`). A non-elevated overlay cannot read an
  Reads can also be refused while `OpenProcess` still succeeds: measured on a freshly launched
  client, 0 of 200 `ReadProcessMemory` calls succeeded at every size from 8 bytes to 4 MiB, with the
  client's handle opening fine and the shell unelevated. `err=5` on the reads while the attach itself
  reports success is therefore an elevation/protection mismatch on the client's side, not a bad
  offset: compare the log's `elevated=` against the level the game was launched at before touching
  the offset table.
  elevated game - the symptom is `err=5` on every ReadProcessMemory - so match the game's level,
  normally both as administrator or both not.
* **Client build.** The offsets belong to one `AION2.exe`: the 2026-10-14 Steam patch,
  156,590,880 bytes, sha256 `699f42cd…ddafd8`. The three revisions before it were 156,557,752 bytes
  (`3842fc51…aba953`, 10-09), 156,604,184 bytes (`d93b050d…37f1ba`, 10-07) and 156,554,016 bytes
  (`59d33a2d…a8ac21`). The overlay prints what it found, so compare one line instead of guessing:
  `client: C:\...\AION2.exe (156590880 bytes; verified build is 156590880 bytes)`. A Steam update
  changes the size and the offsets must be re-derived on that machine; copying the whole `build\`
  folder does not carry the game. `tools\find_shift.py` is the re-derivation - it prints the
  candidates a patch left behind and validates `GWorld` structurally, but on 10-14 its "apply the
  `GNames` shift to the other globals" step was wrong for two of the three: the globals did not move
  by one shift, and each had to be located on its own evidence (see "Offset validation"). `tools\verify_globals.py`
  confirms a candidate pair against the running client.

The overlay waits for the client rather than exiting. It polls for `AION2.exe` to appear and
then for `GNames`/`GObjects` to come up (`sdk_wait_seconds`, default 60; 0 disables the wait).
Measured live, the client publishes `GNames` a good 2.4 s before `GObjects`, so starting the
overlay while the game is still launching is normal and used to end in `SDK init failed` when
nothing was wrong.

When `Sdk::Init` fails it prints an attach diagnosis instead of a bare `SDK init failed`,
because a refused process handle and a different client build otherwise look identical — both
leave every global reading zero:

* `pe: MZ ...` — the client's own PE header read back over `ReadProcessMemory`. If it reports
  `MZ=0x0000`, the reads are not reaching the client: run elevated and check that security
  software is not blocking `OpenProcess`/`ReadProcessMemory`. The `reader:` line carries the
  failed-open count and the Win32 error that caused it.
* `client: ... (N bytes; verified build is 156590880 bytes)` — the build comparison.
* `verdict:` — which of the two it is.

`NamePool ok` alone does not settle it: a cursor of 0 and a block of 0 are both legal, so an
all-zero pool passed the old check. It now also requires the block table to point somewhere.

If two machines disagree, dump both and diff. `tools\win_settings_dump.bat` is a **read-only**
snapshot (it changes nothing) of what can plausibly matter: VBS/HVCI and core isolation, UAC and
process integrity level, Defender state and exclusions, per-app compatibility flags such as "run
this program as administrator", Smart App Control, system exploit protection, and the client's own
path, hash, command line, parent process and loaded modules (`bb64`/NCGuard, Steamworks) — the
last of which distinguishes "the client never really booted" from "the reader is being refused".
It writes `win_settings_<COMPUTERNAME>.txt` beside itself, plus `win_mitigation_<COMPUTERNAME>.xml`
— the schema Windows' own Exploit protection GUI exports — so the two machines diff line by line:

```powershell
Compare-Object (Get-Content .\win_settings_A.txt) (Get-Content .\win_settings_B.txt)
fc.exe /n win_mitigation_A.xml win_mitigation_B.xml
```

## Anti-cheat handle stripping (why the overlay used to die after ~2s)

`AION2.exe` (NCGuard) closes the process handle handed out by `OpenProcess` every few
seconds. Measured: the handle lives ~1-3 s, then every `ReadProcessMemory` fails with
`ERROR_INVALID_HANDLE` (6); a fresh `OpenProcess` works immediately. The old code kept
the dead handle, so `GWorld` read as null, the camera went invalid and the overlay sat on
"waiting for local player..." forever.

`ExternalMemory` now detects that error, re-opens the handle and retries inside the same
read. Verified with a 30 s soak: `cam_valid=1` for every sample, **9 transparent
re-opens**, entity count steady at 260-340.

## Kernel memory path (kernmem.sys + kernmemd.exe)

Re-opening a stripped handle works, but it is a race against a process that is actively
watching us. The alternative is not to need a handle at all: a kernel driver can copy out
of a process' address space without `OpenProcess`, which is what this path does.

Three pieces, with one shared contract (`include/aion/KernMemProtocol.hpp` - included from
kernel mode and user mode, so the IOCTLs and structs cannot drift apart):

| piece | holds | never does |
|---|---|---|
| `kernel/kernmem.sys` | the device `\Device\KernMem`, `MmCopyVirtualMemory` reads/writes by pid, unload support | policy, config, anything game-specific |
| `kernel/kernmemd.exe` | the only handle to that device, the shared section `Local\KernMem` | sit in the game's process |
| `include/aion/KernMem.hpp` | what the overlay links: a mapping of the section and two events | open a driver, or a handle to the game |

Build: `kernel\build.cmd` (installed WDK for the driver, SDK for the rest).

Run: start `kernmemd.exe` before the overlay. `ExternalMemory` looks for the broker's
section when it attaches; when one is publishing, every read and write goes through it and
falls back to `OpenProcess`/`ReadProcessMemory` otherwise - so the overlay behaves
identically whether or not the broker is up, and `kernmem_active()`/`kernmem_fail_count()`
say which path is in use.

`kernmemd.exe --emulator <port>` serves the section from a **kernemul** guest instead of a
driver, which is how the whole chain is developed and checked without loading anything:

```
python tools\kernemul_bench.py          # 9 checks: driver, guest client, memory service, host chain
python tools\kernemul_mem.py --launch D:\Games\kernemul\build-x64\kernemul.exe --image kernmem.sys modules
```

That bench is a development tool: kernemul emulates its own machine, so it proves the
driver's interface, the broker, the section and the overlay's own header - it does **not**
read the real game.

### Running it: on the real machine, under Test Mode

The driver runs loaded on the real machine, under **Test Mode** (`bcdedit /set testsigning on`, one
restart). x64 refuses a kernel image that does not chain to a root it trusts, so `kernmem.sys` is
signed with a locally-trusted root while test signing is on, and that is the load path this project
uses. Test signing survives reboots; a rebuild invalidates the signature, so the image is re-signed
before the next `sc start`.

Two defects in the image itself were invisible under kernemul and would have stopped the load on a
real machine:

| what | was | why it matters |
|---|---|---|
| `.reloc` | absent | the kernel chooses where a driver is mapped, never the address the linker had in mind, so the image has to be relocatable. Every driver on this machine carries `.reloc`; this one had none, because a small x64 driver contains no absolute addresses of its own and the linker had nothing to emit. The dispatch table is static data now - which is where it belonged - and the image carries 44 bytes of fixups |
| `IMAGE_FILE_DLL` | absent | **not** a defect: `werkernel.sys` and `tcpip.sys` do not set it either. Read out of their PE headers, not assumed |

`kernel\untrust.ps1` reverses the Test Mode setup: it removes the locally-trusted root from the three
certificate stores, strips the image's signature and turns test signing off.

## Rendering: DirectComposition swapchain, real antialiased text

The overlay used `SetLayeredWindowAttributes(..., LWA_COLORKEY)` with black as the
transparency key. That makes every non-black pixel *fully opaque*, so antialiased glyph
edges were stamped as hard grey blocks - the "ugly text". That was then replaced with an
offscreen target + `UpdateLayeredWindow(ULW_ALPHA)`, which fixed the text but cost
**~27 ms per frame** (GPU readback of an 11 MB surface + full-window DWM hand-off); the
overlay ran at ~30 fps and burned a core.

It now renders into a **DirectComposition composition swapchain**
(`CreateSwapChainForComposition` + `IDCompositionVisual::SetContent`), so DWM blends the
surface with per-pixel alpha *on the GPU*. No readback, no DIB, no `UpdateLayeredWindow`.
Measured on a 216-entity scene (13 k vertices, 2560x1440):

| phase | before | after |
|---|---|---|
| entity update | ~4 ms | **0.9 ms** |
| draw submit | 0.04 ms | 0.06 ms |
| present | **27 ms** | **0.2-0.4 ms** |

`UpdateLayeredWindow` remains as an automatic fallback (the host then adds
`WS_EX_LAYERED`). Set **`overlay_dcomp=0`** in `aion.ini` to force that fallback without
rebuilding - use it if the DirectComposition output is not visible on a given machine.
Flip-model swapchains rotate their buffers, so the RTV is re-acquired after every
`Present` - holding a stale one draws into a buffer that is never shown.

**Click-through is load-bearing and comes only from `WS_EX_LAYERED | WS_EX_TRANSPARENT`.**
`WS_EX_TRANSPARENT` alone affects painting order, not hit-testing, so a window without
`WS_EX_LAYERED` stays the mouse target even though it is invisible and topmost: every
left/right click went to the overlay and the game received none, which stops attacks and
looting and makes the game release the cursor (it reappears mid-screen). Returning
`HTTRANSPARENT` from `WM_NCHITTEST` is *not* a substitute - it only forwards to windows in
the same thread, and the game is another process. Both were measured:
`tools/click_through_test.py` injects a click through the overlay at a decoy window
underneath and asserts it arrives.

Two further loop fixes: `timeBeginPeriod(1)` (without it `Sleep()` rounds up to the
15.6 ms system timer quantum and pacing silently ran at ~30 fps) and a pacing sleep
instead of the previous 8 ms per frame.

## Text: supersampled glyph atlas

Glyphs are rasterized at **4x the native size and box-filtered down** (`RasterizeBlock`).
GDI at native size snaps stems to whole pixels, which is what makes small text look like a
bitmap font; averaging 16 sub-samples per pixel restores the true outline coverage.
Measured atlas: **249 distinct grey levels, 70 % of ink pixels are antialiased edges**.
Per-glyph advances come from `GetCharABCWidthsA` (proportional, not monospaced) and every
glyph origin is rounded to a whole pixel, so a 1:1 quad covers whole texels - no bilinear
softening. A 1 px drop shadow (`text_shadow`) keeps labels readable over bright scenes.

Font size, face and weight come from `aion.ini` (`esp_font_px`, `menu_font_px`,
`font_face`, `font_bold`) and the atlas is rasterized at exactly those pixel sizes, i.e.
text is never resampled. The menu's **Font size** item rebuilds the atlas live.

### Resolution and DPI

Nothing about the client's size is assumed, so there is nothing to configure for it. The window is
moved onto the client's rect whenever that rect changes, the D3D target is re-created for the new
size (`EnsureSize`, one client rect read per frame) and the projection is re-bound to the surface
every frame (`proj.SetViewport(gfx.Width(), gfx.Height())`). 1366x768, 1080p, the 16:10 panels
(1920x1200, 2560x1600, 2880x1800), 1440p, 4K and ultrawide all land correctly: the FOV is applied
to the wider axis, as UE does, so a portrait window is not stretched either, and `overlay_test`
asserts the centre, the equal pixel scale in both axes and the FOV axis for each of those sizes.

Because the bitmap atlas is drawn 1:1, the label size has to follow the frame too. `esp_font_auto`
(on by default) reads `esp_font_px` as the size at a 1080p-tall client and rasterizes it larger on a
taller one - 1440p gives 19 px, 4K gives 28 px - never smaller, so a smaller or Windows-scaled
client keeps the size that was picked. The atlas is re-rasterized when the client's height actually
changes, not per frame.

The process also **adopts the client's own DPI model** before it creates its window. A DPI-unaware
process is handed virtualised coordinates, so on a 4K panel at 150 % scaling the client's real
3840x2160 rect comes back as 2560x1440 and the overlay would render at two thirds of the resolution
and let DWM stretch it. Matching the client's model - rather than always demanding per-monitor-v2 -
is the part that keeps the boxes aligned: an aware overlay over an unaware client would be offset by
the scaling factor. The log says what was adopted:
`dpi: client=per-monitor, this process=per-monitor`.

### Colours

Every category, plus `list_boss` and `list_target`, has an editable colour - it paints the actor's
box, its labels and its snapline. In the menu's **Colours** section, LEFT/RIGHT walks a 16-entry
named palette (`red`, `pale red`, `orange`, `yellow`, `lime`, `green`, `pale green`, `cyan`, `blue`,
`violet`, `magenta`, `pink`, `white`, `light grey`, `dark grey`, `black`), the row draws the swatch
next to the `RRGGBB` it would save, and ENTER opens three channel rows (R, G, B - 0..255 in steps of
8) for anything the palette does not cover. The same values live in `aion.ini` as `color_*=RRGGBB`;
**Save settings** reconciles the two by rewriting the `key=value` lines in place, so the comments and
the file's order survive - that file is the documentation, and regenerating it from the code would
throw that away. A `color_*` value that is not six hex digits is ignored, which leaves the shipped
default in place rather than painting the category black.

Four more cover the label text itself: `color_label` (the name line), `color_distance`,
`color_class` and `color_class_all` (the class line in SHOW ALL, where it is the real class). They
were literals in the renderer until the menu had to offer them, which is why they are the newest keys
in this group.

## Entity identity: the mesh asset, not the object name

UE names every spawned actor after its class, so `actor.Name()` is useless for identity:
every NPC is literally `BPCh_NpcBase_C`, every gatherable is `BP_EnvObj_Common_C`.
Verified live with `tools/probe_identity.py`:

```
actor 0x25787c010 class=BPCh_NpcBase_C object='BPCh_NpcBase_C'
    +0x3c0 comp=AionSkeletalMeshComponent 'CharacterMesh0'
        +0x5f8 SkeletalMesh  'MOB_WindEle_01'      <- the actual creature
```

So the entity's display name is resolved from the **mesh asset**:
`ACharacter::Mesh` (0x3C0) -> `USkeletalMeshComponent::SkeletalMesh` (**0x5F8, VALIDATED
live**), or, for non-characters, the first `*MeshComponent` in
`BlueprintCreatedComponents`/`InstanceComponents` -> `StaticMesh` (0x600). The resolved
object is type-checked (`StaticMesh`/`SkeletalMesh`) so a wrong offset can never produce a
bogus name. Observed models: `MOB_WindEle_01`, `MOB_Fossa_01`, `Mob_OdSpider_02_V02`,
`GF_RootMesh` (player).

The *localized* display name is not stored on the actor (no `PlayerState` on NPCs, no string
property anywhere) and no data table is keyed by the mesh with a name column: the 147 loaded
`DataTable`s are appearance and resource tables. The client puts the resolved name in the
widget it draws as the entity's nameplate, and that is where it is read from:

```
actor + 0x2810  ->  WC_NamePlate_C widget      (a pooled helper)
       + 0x5F0  ->  NamePlate_C widget         (per entity)
       + 0x268  ->  WidgetTree -> +0x30 RootWidget
                ->  walk UPanelWidget::Slots / UPanelSlot::Content
                ->  AionTextBlock -> +0x1B0    (the text; 0x1A8 does not fit this class)
```

`Nameplate.hpp` does this walk and each entity is resolved once and cached. Coverage does not
stop at "the client is showing a nameplate right now":

* **The learned table.** A name belongs to the *model*, so every resolved name is written to
  `aion_names.txt` as `model=Name` and read back on the next start: one sighting of
  `MOB_GraveSoul_01` names every instance of it, at any distance, in every later session.
  Fill it in one pass with `aion_external.exe --harvest-plates` (run it in a busy area), or
  edit the file by hand - it is plain text and takes precedence over `name_map`.
  What the overlay should *show* for a model is a setting rather than something learned, and it
  lives in `aion.ini` as `name_rules=`: one entry per rule, `force:pattern=Label` always shows
  that label, `auto:pattern` shows the name the client reports live (never a stored one), and
  `hide:pattern` draws the entity but never its name. A forced label beats anything learned.

  Only a model that identifies its entity may key the table. This matters more than it
  sounds: every player of a gender renders one mesh (`GF_RootMesh`), and a pet's name belongs
  to its owner, so a sighting of "Gawinda" and of "Alexa" on `GF_RootMesh` is not one model
  with two names. Such models are dropped and listed on an `ambiguous=` line instead, and
  players are never taken from the table at all - their name comes from their `PlayerState`
  (`APawn::PlayerState` 0x358, `PlayerNamePrivate` 0x3D0), which exists at any distance and
  is the character name rather than whatever the nameplate is showing. Pets use the nameplate.
* **The per-actor name field, when the client keeps one.** Two sightings of different actors
  are enough to locate a member that reproduces each one's own name (an FString or an FText),
  tried twice. In this build there is none - `--find-name` finds no table holding the name
  either, so the client resolves it into the widget and the learned table is what carries
  coverage.
* **The pooled helper.** The actor's `WC_NamePlate_C` still holds the last nameplate the
  client built for it, so it is read when the per-entity plate is detached (out of range).
* **World props** (gathering nodes, chests, drops) are named by their model too, but the
  client only builds a nameplate for them when *its own* option for that category is on:
  `AionWidget_Setting_GatherNamePlate` is a four-way radio in the game's settings, next to
  the monster and player ones, and `OnUpdateGatherNamePlateOption` reacts to it. With the
  option off, `BP_EnvObj_Common_C::NamePlateComponent` (+0x4F8, a reflected property) is null
  and there is nothing to read, which is why a gathering node shows its `name_map` entry or
  its mesh. `NameplateReader::Widget` resolves the nameplate member per class (characters use
  the validated `NameplateWidgetComponent`), so the moment the client does show one, the name
  is read and learned.

Precedence: PlayerState (players) > the client's own per-actor name (nameplate, or the learned
per-actor field when the client has one) > learned model name > `name_map` > mesh > object name.

The per-actor name comes first because it is the only source that can tell two monsters apart when
they share one mesh - a "Baby" variant *is* the same mesh, scaled, so a model-keyed label gives both
whichever name was learned first. The learned table stays the fallback for the actors whose
nameplate the client has not built, which is the reach it exists for: measured, the client builds
none beyond roughly 25 m (the same distance at which it stops rendering them, see `esp_hidden`), so
for those the model name is the best anything can do.

```
name_map=MOB_Fossa_01=Fossa;SM_Morfa=Morfa
```

Matched (case-insensitive substring) against model, then object name, then class name.
**Menu -> Dump actor classes** writes `aion_classes.txt` as `count  class  model` so the
model names can be read in-game. The file is relative to the process working directory (so
`build\aion_classes.txt` when the overlay is launched from there); its header carries the
absolute path and the time it was written, the log line repeats the path, and the menu shows
the same result on its status line until the next action replaces it.

## Looking for the client's own name source

The nameplate path works, but the authoritative source (if the client has one) would name
everything with no sighting at all. One headless, read-only tool looks for it:

```
aion_external.exe --find-name "Lost Specter"   -> aion_findname.txt
```

`--find-name` searches objects named after the text, every data table row (raw, one read per
row) and the memory of string-table objects, reporting the object, key and offset where the
literal sits.

## ESP entity categories

Actor collection is independent from drawing; every actor is classified by class name,
mesh name and the optional `aion.ini` substring lists that take precedence. The shipped
defaults are the master toggle plus **boxes, names, distance, class and snaplines** on, and
**players, NPCs, pets and items** off - a town is mostly those four and they drown out the
rest. Monsters, gathering nodes, interactables, unopened chests and portals are on; world objects and other
actors are opt-in. Everything is a menu toggle, plus one rule of its own: **a container is
drawn only while the client reports it unopened** - see the table below.

| Category | Live classes observed | Colour |
|---|---|---|
| Player | `BPCh_PcBase_C` | green |
| NPC | `BPCh_NpcBase_C`, `AionSimpleCharacter` - `AionEffectCharacter` is never drawn | cyan |
| Pet | `BP_Pet_C` | light green |
| Monster | `MOB_*` / `Mob_*` in the class, object name or mesh name, **on a creature class only** - tested *before* the generic NPC classes below. `MOB_Invisible`, the invisible-placeable marker asset, is never a monster | pale red, or solid red when it is in `list_boss` |
| Item | `DroppedObj_C` (`Item_Drop` mesh), `HomingDropObj_C` | yellow |
| Chest | chests, lootboxes, bags and the rest of that family, matched on **class or mesh**: `Chest`, `Treasure`, `RewardBox`, `LootBox`, `Lockbox`, `Strongbox`, `Bag`, `Pouch`, `Satchel`, `Backpack`, `Sack`, `Crate`, `Barrel`, `Container` - deliberately *not* bare `Box`/`Cube`, which would take the reward cubes. Drawn **only while the client reports the container unopened**, so a looted chest leaves the screen by itself; the client's own act containers (`AionMapEnvActContainerActor`) are mechanism actors and are deliberately not in the family | amber |
| Gather | `Gather_*` meshes (`Gather_Jewelry_L_Lv1_02` = the "Diamond" node), `OdSmall` (the small resource node, 4-8 live, class `BP_EnvObj_Common_C`), `Food_Lv1_*` (the "Morfa" vegetable node), `VegetableMela` and `TreeBetua` (vegetable and tree nodes on the same `BP_EnvObj_Common_C` class), `Cotton_Lv*` (the fibre node, matched on the bare material word `Cotton` so numbered variants are covered), `ShellSherry` (a shell gatherable - the class name is the only stable key, since its display name varies per placement; one is shown as "Conide"), herb/ore/plant model names - `AbsorbObj_C` is never drawn | magenta |
| Interact | `Protect_*` meshes (e.g. "Empryan Trace: Altgard" style quest objects), `list_interact` | violet |
| Cube | `HiddenCube` / `Hidden_Cube` / `EnergyCube` / `OdEnergyCube` in the class, object or mesh name, `list_cube` - checked before Gather's `Cube` pattern and before Interact, so cubes stop sharing their colours | yellow (255,220,0), the same as Item - drops are usually switched off, so a cube should read as loot; the class label and the *Hidden cubes* toggle still tell them apart |
| Portal | `Portal_*` meshes - `Portal_Space_01` is the "Lost Ruin Entrance" (name it with `name_map=Portal_Space_01=Lost Ruin Entrance`) | white |
| Object | any other real actor (opt-in, `cat_object`) | grey |
| Noise | volumes, nav/`WindPath` splines, audio, nameplates, controllers, cameras, projectiles, bare `Actor` | never drawn |

A census of all 691,780 objects / 7,773 classes found no `Chest`, `Treasure`, `Box` or
`Cube` actor class: every container carries the shared `BP_EnvObj_Common_C` class and is
identified **only by its mesh asset**. Read live from the running client, the chest in a
starting-area camp is mesh `RewardBox_02` on that class, and the state that says whether it
has been opened is `AionEnvObjAnimInstance::MainState`, an `EAnimInteractionState` whose
values read straight out of the live `UEnum` are `Available` (0) and `Finish` (1). Measured
across one zone's world objects, the 15 untouched ones read `Available` and the 2 already
consumed ones `Finish` - and `MainState` is the only byte inside a declared object size
(`AionEnvObjAnimInstance` is `0x630`) that separates those two groups.
`bInactiveCondition`, the byte the spent-**node** check reads, does not move for a container
at all, which is why containers could not be told apart before. So the family is drawn while
`MainState` still reads `Available` and dropped once the client reports it `Finish`; a
container whose state cannot be read is left visible, because unknown is not "opened". Hide
more classes or meshes with `never_show=`.

### Identifying unknown actors: `esp_show_all`

Aion spawns creatures from one generic character class
(`AionSimpleCharacter`/`NpcBase`), so a class name alone often cannot say what an actor
is - and scenery, chests and gathering nodes are easy to mis-guess. `esp_show_all=1`
(or the menu item **SHOW ALL + real class**) makes the classifier stop hiding anything:

* every actor is drawn, including what is normally classified as `Noise` (volumes, nav
  splines, decals, landscape, audio) - and chests a looted run has left behind, since the
  spent check is bypassed with everything else;
* the second label line shows the actor's **real UObject class** instead of the guessed
  category - the box colour still encodes the guess, so a wrong guess is obvious at a
  glance;
* the list stays distance sorted, and `max_entities_drawn` / `max_distance_m` still apply,
  so the draw limit keeps the *nearest* actors rather than an arbitrary subset.

Actors that are a sub-object of another actor rather than an entity of their own are
never drawn, in *any* mode - controller classes (`AionDummyController`,
`AionPetAIController`, `AionDefaultAIController`, `AionPlayerController`), projectiles
(`AionProjectileBase`) and footstep actors (`AionFootstepActor`), plus the bare engine
`Actor`/`Object`/`DefaultPawn`, all of which sit on or duplicate the actor that spawned
them. Add your own with `never_show=` in `aion.ini` (comma separated class names; exact
match for the bare engine names, substring otherwise). Mesh names work too, which is what
these need, since a shared blueprint class carries many identities: `Honeycomb` and
`DeadVillager*` (environment) are never drawn by default, and of the spiders only the single
scenery variant `Mob_OdSpider_02_V02` is - the rest of the family (including
`Mob_OdSpider_02`) are monster spiders and classify as such through the `mob_` asset check.
Class rules cover the ones whose class is what identifies them:
`AbsorbObj_C` (gathering nodes), `AionWorldActPlayActor` (world act-play props, 22-26 live,
no mesh), `AionEffectCharacter` (effect stand-ins - they mirror a real entity, so they drew a
second box on it; seen carrying `MOB_WindEle_02`, a duplicate of a live monster) and
`AionTargetDecalActor` (the target decal), `StaticMeshActor` and `InstancedFoliageActor`
(engine-placed decoration), `PointLight`, `AionBGMVolume` and `AkAmbientSound` (light, BGM
volume and ambient sound - no identity at all) and `TrainingScarecrow` (the training dummy).
The elemental spirits (`MOB_FireEle_*`, `MOB_EarthEle_*`, `MOB_WaterEle_*`, `MOB_WindEle_*`)
are there for a different reason: their asset name starts with `MOB_`, so the classifier's
mob_ check makes them **Monster**, but they are ambient and not monsters, and the generic
`AionSimpleCharacter` class cannot separate them from a real one - the asset family is the
only key. Prefix match, so every numbered variant is covered. `MOB_TraDummy_01` (the training
dummy, on the generic `BPCh_NpcBase_C` class) is hidden the same way.
The `mob_` check itself now only counts **on a creature class**. `MOB_` is Aion's *mobile placeable*
asset family, not a hostiles-only one - `BP_Pet_C` carries `MOB_DranaVar_03`, and the client's invisible
marker carries `MOB_Invisible` - so a prop wearing one (a portal or a shard on `BP_EnvObj_Common_C`)
falls through to the portal/gather/interact rules, whose mesh patterns are what actually identify it.
`MOB_Invisible` is the asset the client puts on an actor whose visuals come from another actor (the
abyss portal, trap and laser emitters, skill shards); the actor has no mesh at all, so a box on it marks
nothing. Every one of them used to be a monster - one zone held 62, and they were that zone's whole
Monster category, which is the screen full of garbage they made under the Monsters toggle. They are
`Noise` now, which `esp_show_all` still lists.
The engine-scaffolding entries (`StaticMeshActor`, `InstancedFoliageActor`, `PointLight`,
`AionBGMVolume`, `AkAmbientSound`) are already `Noise`, which hides them normally, but
`esp_show_all` draws Noise as well, so they used to show up in the diagnostic with nothing but
a yellow distance label - they are hidden in every mode now. All are dropped before the
`esp_show_all` bypass, so they are absent from the diagnostic list too - `--dump-classes`
labels them `NEVER DRAWN`. See `AlwaysHidden`.

Pair it with the menu's **Dump actor classes** (`aion_classes.txt`, one line per
`count  class  mesh`), then pin what you found with `list_gather=`,
`list_monster=`, `list_cube=` or a display name via `name_map=` - all of which take
precedence over the built-in patterns.

## Bosses are declared, not detected

Bosses and mini-bosses keep the solid red the ESP used for every monster; ordinary monsters
are pale red. Which is which comes from `list_boss=` in `aion.ini`, edited in the menu's **Lists**
tab. A file next to the exe used to declare them (`aion_boss.txt`, one pattern per line, `#`
comments); an install that still has one has its entries added to `list_boss=` once, on the first
run, and written into the ini - from then on the ini is the one list, and **Reload** brings a newly
typed name in mid-session. A whole name there matches (case-insensitively, exactly - so
`Contaminated Swarm` does not claim `Contaminated Swarm Egg`),
in order: the mesh/model (`MOB_DranaVar_03`), the engine object name, **the name on the ESP
label** (the client's own localized name, `aion_names.txt` - so you can list `Drana Predator`,
which is the only name in front of you on screen), then the class. The worked example is the
Drana Predator mini-boss, `MOB_DranaVar_03`, which aggros on sight - the very monster that looked
identical to the ordinary Congealed Drana (`MOB_DranaVar_06`) next to it.

### The same list, in the drop colour: `list_target`

`list_target=` takes the same whole names and matches them in the same order, but paints those
actors in the yellow the ESP already uses for dropped items. It is the hunt list rather than
the danger list - a short, deliberate "this is what I came for" - and sharing the drop colour
makes it read the same way loot does at a glance. An actor on both lists takes the target
colour: adding it there is the more recent intent. `esp_target=0` (or the menu's **Monster
Target** row) hides the highlight without emptying the list. `aion_target.txt` was the file
counterpart of this list and is imported into `list_target=` the same one-time way as `aion_boss.txt`.

It is a declared list because the client does not know the answer either. What was ruled out,
live:

* **Aggro is the server's decision.** `AggroInfoData` is `{ObjectId, HostilePoint}` -
  per-player runtime hostility - and it reaches the client in
  `DevPacketData_GameServer_NpcFirstAggro_NT` once the server has already picked a target.
  Earlier work here established the same ceiling from the packet inventory
  (`NpcFirstAggro_NT`, `SetMainTarget_RQ/RS`).
* **No rank property.** The property dumps for `BPCh_NpcBase_C` / `AionCharacter` hold no
  grade, rank, tier or boss field, and a scan of ~460k live objects found no boss flag on any
  monster class.
* **No table row either.** `--find-name` finds the localized name in 0 of ~25k rows across ~118
  data tables and in 0 string tables, i.e. `Drana Predator` is nowhere in the client's data. The
  model does appear - `MOB_DranaVar_03` as 56 *object* names (mesh, skeleton, material, actor),
  `DranaVar` as 675 - but as assets, never as a row: there is no NPC record to carry a rank,
  an aggro range or an attack type, so nothing on disk or in memory says "this one is a mini
  boss". The name arrives per actor and is identical for every instance of a model, which is
  why `aion_names.txt` can learn model -> name at all.
* **No rank in the names.** Only `Monster/M_Normal/...` exists in the monster asset tree, and
  every boss-named object in the client is UI or dungeon-challenge scaffolding
  (`AionWidget_HUD_TargetInfo_Boss`, `BossChallengeCategoryTableItem`, `AionDungeonBossZone`),
  not a monster identity.

The one remaining candidate is the nameplate: a monster's plate is assembled from widgets that
include `P_Txt_NpcGrade` and the `Can_Grade_*` / `Img_GradeBG_*` set, and the ESP already walks
nameplate widgets to learn real names. Whether a plate's *grade* is readable per actor is
unverified - it needs a monster's widget dump taken while standing next to a boss.

The flag is resolved once per actor, alongside the rest of the identity in the metadata cache,
so it costs nothing per frame. Geometry and category are untouched - only the colour changes,
and a monster still answers to the **Monsters** toggle.

## What the game shows vs what it merely knows: `esp_hidden`

The client keeps every actor it is sent, long past the point where it stops drawing it. The flag
that means "not drawn" is **not** `AActor::bHidden`, which is what this section used to claim -
`--dump-gates` measured it: characters read `bHidden=1` *while* the renderer's own
`SkinnedMeshComponent::bRecentlyRendered` read `1` (an NPC at 34 m, drawn and flagged hidden at the
same time), and characters past 128 m read `bHidden=0` with nothing drawn past **45 m**. The client
draws characters to roughly 45 m, and `bHidden` is not what separates the drawn from the undrawn.

So the rule the ESP applies to a `bHidden` actor is "hidden **and** the renderer did not draw it":
an actor the client is actually showing is never dropped on the byte alone - that cost on-screen
NPCs their boxes - while a hidden, genuinely undrawn actor still is. A failed read of either byte is
UNKNOWN and leaves the actor drawn, the same contract `bDead` and the spent-node checks follow.

`esp_hidden=0` (the default) drops the actors that are flagged hidden and undrawn. Set
`esp_hidden=1` to keep them and draw them in `color_hidden` out to `hidden_range_m` - an ESP that
reaches past what the game itself shows, without pretending those actors are on screen.

| key | default | meaning |
| --- | --- | --- |
| `esp_hidden` | `0` | draw actors flagged hidden that the renderer did not draw (`bHidden` + `bRecentlyRendered`) |
| `hidden_range_m` | `600` | how far those reach, in metres, when drawn |
| `color_hidden` | `8A8A8A` | their colour, before the category colour |

Only a confirmed `bHidden == 1` suppresses an actor: a failed read is UNKNOWN and the actor is
drawn, so a transient read failure can never blank something that is on screen. The bit is read out
of the identity block the metadata cache already fetches, so it costs no extra syscalls, and
`esp_show_all` still shows everything.

## Why the ESP stops at roughly 150 m

Nothing in the ESP imposes that: `max_distance_m` is a *draw* filter (200 m by default) applied
after the entities are collected, so raising it only draws more boxes. The limit is the client's
actor population. Measured live, in one frame with a valid camera:

| type | within 100 m | 100-200 m | 200 m - 3 km | farthest |
| --- | --- | --- | --- | --- |
| Monster | 17 | 1 | 0 | 1 at 3.1 km (`MOB_Invisible`, the Abyss Portal) |
| Player | 2 | 1 | 0 | 147 m |
| Pet | 2 | 3 | 0 | 127 m |
| Npc | 1 | 0 | 0 | 17 m |
| Object | 13 | 5 | 18 | 3.2 km |

Characters *do* exist further out, and always did: the same client holds characters at 1-3 km, and
`AActor::bHidden` reads `0` on most of them. The table above describes what the client was
**rendering**, which `bHidden` does not mark - `--dump-gates` later measured `bHidden=1` on actors
being drawn, so it is not the render gate it had been taken for. Those far characters were never
absent from the process, and the ESP was drawing them; `esp_hidden` (above) is that switch now. Two different radii are in play: what the client **draws**
(measured with `--dump-gates`: characters drawn out to 45 m, none drawn past 128 m) and what the
server **sends**. Gone is the third state the ESP used to invent: sent, positioned, and invisible.
What no client-side setting can do is draw an actor the process does not have - raising
`max_distance_m` cannot conjure one.

**Where this section's "~150 m" actually comes from, measured:** every character carries
`AActor::NetCullDistanceSquared` = 150 m (live read; 150 m on every Pet, NPC and Player sampled) -
the server's per-actor replication radius. Past it the client is not sent updates, and the stale
copies it still holds are the ones the table above finds at 1-3 km: real objects with frozen data.

## Performance model: cached actor metadata

Reading identity per actor per frame cost ~7 `ReadProcessMemory` calls (block, name, root,
movement, transform...) = ~2000 syscalls/frame. `EntityCache::UpdateBulk` now keeps a
persistent per-actor metadata cache (name, class, type, model, root, display) and only
re-reads the transform each frame, re-validating identity every 64 frames. That is
**~0.9 ms/frame for 216 entities** including the model lookup.

Two pitfalls found the hard way, both now encoded in the code:

* **Failures must be cached too.** Validation is budgeted per frame (96 actors). Actors
  that fail (managers/infos have no `RootComponent`) were erased and so re-consumed the
  budget *every* frame; the sweep never advanced past the head of the level's actor list
  and only a handful of entities ever appeared (symptom: "only my pet has ESP").
* **Budget the sweep.** Re-validating all ~1000 actors in one frame produced 100-170 ms
  hitches every ~2 s.

### A category that is off costs no reads

The per-frame transform read is the only cost that scales with the actor count, and the
default category set (players, NPCs, pets, items **off**) means a town is mostly actors
nothing draws. `UpdateBulk` now consults `CategoryVisible` *before* the transform read, so
those actors cost nothing at all - and, because the draw limit keeps the nearest
`max_entities_drawn` entries of the collected list, they no longer push the actors you do
want past that limit either. The local pawn is exempt (its position is the distance
origin). Capture mode and `recon` pass `cull_hidden=false`: a census must list everything.

### Project against the camera the frame was read with

The camera is read at the top of the frame, then one transform syscall per actor is spent,
and only then is everything projected - so the frame used to be drawn against a camera that
was a whole read pass older than the positions beside it. `read_pose()` is now called again
after the transform pass and `EntityCache::Reproject` re-runs `WorldToScreen` for the
already-collected positions (no memory reads, microseconds). The geometry drawn is then
consistent with the newest camera instead of lagging the world when it turns. The soak line
reports the cost as `reproj=`.

### Where the frame time went

With **Debug info** on, `Hud.hpp` draws a panel that answers the only question worth asking
about an uncapped overlay: which phase is late. It shows the five phases
(update / reproj / draw / flush / present) as a stacked bar with their milliseconds, each
coloured against the display's own frame budget (`1000/Hz` - "slow" is a different number on
a 144 Hz panel than on a 60 Hz one), plus the actor counters (`listed` → `kept`, `culled`,
`validated`, `drawn`), the vertex count, handle re-opens, the present path actually in use
(DirectComposition can fall back at init) and the camera state. It replaces the one-line
`ents=.. fov=.. loc=..` print that used to sit at the same corner.

### What the live client actually showed

A 15 s soak in the field, 27 drawn entities out of `listed=6942`: `validated=48` (the
validation budget, saturated on every frame), `culled=71`, `update` 0.6-1.4 ms, `reproj`
0.01-0.02 ms, `draw` 0.04-0.08 ms, `flush` ~0.03 ms, `present` 0.1-0.5 ms, 370-540 fps. Two
things follow. The re-projection costs nothing measurable, and the frame is dominated by the
memory-read phase - which is what the culling exists to shrink.

That phase is not flat: single frames of **36-48 ms** recur every 1-3 s, ~40x a normal one.
They are not the projection (0.02 ms) and not the draw. The counters also rule out the
validation budget as the trigger - `validated` reads 48 on slow frames exactly as on fast ones
- which leaves the read path itself: the anti-cheat strips the process handle (`reopens`
climbed 0 -> 5 in 15 s) and every re-open re-establishes access. Those spikes are the likeliest
source of any remaining hitch and are the next thing to measure.

A capture from the same session (2560x1440, 84 actors, 69 boxable) verified the drawing on
real entities: a 120x241 px monster box drew 27 px arms at both ends of its top edge with a
68 px gap between them, and its left edge only at the two corner tips - brackets, not an
outline. The local pawn reads exactly `0m`, which is the whole point of the origin change: a
camera-relative range shows the camera's distance from the character instead.

## Hiding dead characters

AION2 exposes **no reflected health or death property on the character** (`AionCharacter` has 216
UPROPERTYs, none matching dead/hp/health/die), but the client's own anim instance does carry one:
`AionCharAnimInstance::bDead` (`+0x80C`, a reflected `BoolProperty`). The ESP reaches it by name
through reflection - `Character::Mesh` (`0x3C0`) -> `SkeletalMeshComponent::AnimScriptInstance`
(`0x988`) -> `bDead` - so a client patch that moves the field keeps working, and both hops are cached
per class.

The old signal was `UCharacterMovementComponent::MovementMode == MOVE_None`, which was wrong twice
over: `MOVE_None` also means "no movement mode yet" (a spawning or paused pawn), and because the
byte read returned 0 on a *failed* read, one transient read failure was indistinguishable from
`MOVE_None` - with **Hide dead** on by default, a momentary failure hid a live character.

The rules now:

* **Hide dead hides only on a CONFIRMED `bDead == 1`.** A read that fails leaves the actor
  **not known dead**, so it stays visible. Uncertainty never hides anything.
* **The death is latched.** `bDead` is the client's *death animation* state, not a property of the
  body: measured live (2026-10-12, four monster types), it reads 1 only for the ~10-20 s that
  animation runs, and is then cleared while the corpse is still lying in the level. A corpse whose
  latch has not been taken is invisible to every other field the ESP reads - it reports
  `AActor::bHidden 1`, while the renderer's own `bRecentlyRendered` still reads 1 (a flag that never
  clears for a component the client stopped drawing), so the "`bHidden` AND not drawn" gate keeps it,
  and the box came back on a dead monster. So a confirmed `bDead == 1` is remembered, and released
  again only by a successful read of `UCharacterMovementComponent::MovementMode` that is not
  `MOVE_None`: a corpse holds `MOVE_None`, and a respawn or a revive walks the character again.
  This is the **only** place `MOVE_None` is trusted, and only once the client has confirmed the
  death first - which is exactly what the old signal above got wrong.
* A **failed** read - of `bDead` or of `MovementMode` - drops the latch and hides nothing, the same
  way a failed read never creates a death.
* If the anim instance, `bDead`, or `MovementMode` cannot be resolved, the character has no dead
  signal (or no way back out of one), so it is never treated as dead.

That read is a syscall per character, so it is spent only where a draw depends on it: characters on
screen, plus - with off-screen markers on - the ones within `offscreen_range_m` of the camera, so a
corpse behind you is not marked as a live enemy. Characters further off stay unread.

## Logging

`Logger` writes from a **background thread** with a bounded queue. A synchronous write to
a console in QuickEdit mode (or a redirected/blocked stdout) can stall for seconds and
hitch the render loop; log calls now never block it.

Lines carry a wall clock and a level-coloured tag, and warnings and errors colour their own text:

```
[17:42:06.913][INFO ] Sdk base=0x7FF6A1C40000 ready=1
[17:42:06.914][INFO ] offset GObjects    rva 0x0EC8C9A0 -> NumElements=418233  VALIDATED for this client
```

Colour is emitted only when stdout is a real console that accepted virtual terminal processing
(Windows Terminal has it on already; classic conhost is asked). Redirecting to a file therefore
yields plain text with no escape codes - as does the file sink, which never gets colour.

The startup banner goes through the same queue, so the welcome screen cannot interleave with the
first log lines:

```
 ###   #####  ###   #   #   ###
#   #    #   #   #  ##  #  #   #
#####    #   #   #  # # #  #   #
#   #    #   #   #  #  ##  #   #
#   #  #####  ###   #   #   ###
======================================================================
  E X T E R N A L   O V E R L A Y   for AION2 (UE5)
  by ZixMatrix   read-only: no injection, no writes to the client
----------------------------------------------------------------------
  build        Oct  9 2026 15:51:23  |  MSVC 19.51.36243  |  C++20  |  Obfusk8
  client       AION2.exe  |  verified build 156590880 bytes
  config       D:\Games\aion2\build\aion.ini
  launched as  7Kq2mZpR4tVbN1xw.exe  (renamed at startup)
======================================================================
```

## Verification tooling

```
build\aion_external.exe --pattern          # magenta square: proves the overlay reaches the screen
build\aion_external.exe --dump-atlas a.bmp # the rasterized glyph atlas (font quality)
build\aion_external.exe --seconds N        # soak: fps + per-phase cost + AC handle re-opens
build\aion_external.exe --dump-gates BPCh_ # why the client is not drawing those actors: bHidden,
                                           # bRecentlyRendered, draw distances, replication radius,
                                           # distance - plus the anim-instance gates the ESP resolves
                                           # by name (bDead, bHpDangerAlert), to aion_gates.txt
python tools\find_shift.py                 # patch moved the globals: report the shift, validate each
python tools\verify_globals.py G_OBJ G_NAM # confirm a candidate pair against the running client
python tools\find_globals.py               # shape-scan fallback (pool + array pair, image only)
python tools\check_offsets.py             # validate the RVAs in Offsets.hpp against the client, no build
python tools\probe_identity.py             # actor identity: object name vs mesh asset
python tools\probe_mesh.py                 # mesh-component and mesh-asset offsets per class
python tools\screen_shot.py shot.bmp       # BitBlt the desktop (overlay visible on screen?)
powershell tools\menu_probe.ps1            # menu focus + its X + ESCAPE, driven with real input
```

## Build

Release only. MSVC (Visual Studio 2026 Build Tools) is auto-detected:

```
build.bat            # -> build\aion_external.exe, the product
build_test.bat       # -> build\aion_recon.exe, build\overlay_test.exe (verification)
```

### ImGui (vendored)

The menu is [ImGui](https://github.com/ocornut/imgui) 1.91.9b, vendored under `third_party/imgui`
(core plus the DX11 and Win32 backends; the upstream tag is recorded in
`third_party/imgui/UPSTREAM.txt`). It compiles as its own `cl` pass at `/W0` into `build\`, guarded by
a marker file named after the version - so re-vendoring a different ImGui rebuilds those six objects
instead of silently linking the old ones.

Nothing is patched: the backend's blend state already writes the alpha channel with
`SourceBlendAlpha = ONE`, which is exactly what a premultiplied-alpha layered window expects (the
stock `SRC_ALPHA` would square it and darken every translucent pixel). That is worth knowing because
it is why this copy is stock and can be re-fetched from upstream verbatim.

### Obfuscation (Obfusk8)

The release exe is built with [Obfusk8](https://github.com/x86byte/Obfusk8), vendored under
`third_party/Obfusk8` (header-only, MSVC; the upstream commit is recorded in
`third_party/Obfusk8/UPSTREAM.txt`). What is obfuscated is the tool's own names:
`src/obf_strings.cpp` defines the accessors in `aion/Obf.hpp` — `TargetProcessName()`,
`TargetProcessNameW()`, `OverlayWindowClass()`, `OverlayWindowTitle()`, `LogFileName()` — with
`OBFUSCATE_STRING`, so `AION2.exe`, `AionExternalOverlay`, `AION2 overlay` and the log name are
decrypted at runtime instead of sitting in the string table. The log lines name the client through
the same accessors, so they read exactly as before without repeating the literal.

Three constraints are load-bearing, each measured rather than assumed:

* **`/O2` is required.** AES8 encrypts a literal at compile time only when the optimiser folds its
  constexpr path. At `/Od` it does not fold: the literal stays in the image as plaintext *and* the
  runtime path produces rubbish, so the overlay looked for a process whose name was binary junk and
  never found a running client.
* **A literal must fit one AES block.** AES8 folds a single 16-byte block; a longer literal takes a
  multi-block path MSVC does not fold either (measured: at `/O2`, every literal of 16 bytes or more
  survived a scan of the produced exe, every one of 15 bytes or less did not). So each literal in
  `obf_strings.cpp` is at most 12 characters, split from the name it spells and joined at runtime.
* **`build.bat` verifies it.** It scans the finished exe for those four names and fails the build if
  one is present. Without that check a leak is invisible — the binary compiles, links and runs, it
  just describes itself.

The entry point is deliberately **not** wrapped in Obfusk8's `_main` macro, although that is the
obvious use for it: with `_main` in the link, the obfuscated literals decrypt to garbage — the
prologue is not side-effect free. Measured with the identical objects linked two ways, twice:

| link | `TargetProcessName()` returns |
|---|---|
| entry wrapped in `_main` | bytes like `a4 c4 9e d8 94 4b 20 f4 …` |
| plain entry, same objects | `AION2.exe` |

`src/entry_obf.cpp` keeps the entry in its own translation unit and records that measurement, so the
macro can be brought back once the interaction is understood. Until then, the names are hidden and
the entry point is not.

`AION_OBFUSK8` is the switch, defined only for `aion_external.exe`. `build_test.bat` compiles
`obf_strings.cpp` without it, so `aion_recon.exe` and `overlay_test.exe` keep plain literals — a
failure there has to stay debuggable, and the names they return are identical to the release build's.

What is deliberately **not** obfuscated: the data-file names (`aion.ini`, `aion_boss.txt`,
`aion_target.txt`, `aion_names.txt`) — the generated config has to name them for the user to know
what to edit, and hiding them in the binary while writing them into a file on disk would hide them
from nobody; the `--dump-*` probe outputs, likewise named in this README; the log *wording*; and the
import/API surface. There is no IAT stealth and no decoy PE sections: this hides the identifying
strings, not what the process does.

## Architecture

```
include/aion/
  Logger.hpp      leveled, thread-safe, console+file sink
  Memory.hpp      IMemory abstraction: ExternalMemory (RPM) + InternalMemory (direct)
  Offsets.hpp     centralized, status-classified offsets (no scattered magic numbers)
  Sdk.hpp         runtime context: module base, resolved globals, FVector/FRotator (LWC doubles)
  NamePool.hpp    FNamePool decoder (FNameEntryId -> string)
  GObjects.hpp    FUObjectArray chunked object-table access
  UObject.hpp     IsValid/GetName/GetClass/GetOuter/GetFullName/IsA
  Reflection.hpp  FindClass/FindObject/FindFunction, EnumerateFunctions/Properties,
                  GetPropertyOffset (FProperty chain), GetSuperClass
  Symbols.hpp     best-effort ProcessEvent/StaticFindObject discovery (xref scan + modal vtable)
  World.hpp       UWorld -> ULevel(Levels) -> Actors; OwningGameInstance
  Actors.hpp      AActor helpers (root, world location/rotation, mesh, distance)
  Camera.hpp      PlayerCameraManager -> CameraCachePrivate -> POV
  Projection.hpp  UE world-to-screen (LWC doubles, UE rotator convention)
  EntityCache.hpp actor collection, independent of rendering (per-actor + bulk paths)
  Renderer.hpp    IOverlay primitive interface + EspRenderer (no graphics API knowledge)
  Overlay.hpp     D3D11 immediate-mode overlay (layered, per-pixel alpha, 2 bitmap fonts)
  Menu.hpp        ImGui menu (ESP / CONFIG / DEBUG rail + session column) bound to Config
  ImGuiLayer.hpp  ImGui context, dark flat style with #F34B7D, fonts, focus hand-off
  Aion.hpp        AION2 manager graph, UI/widget reflection access, skill slots
  Config.hpp      runtime config (aion.ini)
src/
  recon.cpp, overlay_test.cpp, external_main.cpp, Overlay.cpp, Menu.cpp, ImGuiLayer.cpp
```

### ESP

Entity collection (`EntityCache`) is fully independent from rendering (`EspRenderer` +
`IOverlay`). Entities expose: address, class, name, location, rotation, distance, mesh,
root component, screen position, bounding box, on-screen flag. Rendering draws the box
(either a closed outline or an L bracket at each corner, `esp_box_corners` /
`esp_corner_frac`), name, distance and class labels and the snapline, all in the category's colour
(`color_*` - see **Colours** above). The snapline's origin is
a four-way choice (`esp_snapline`, menu row **Snaplines**): `off`, `bottom` and `top` are the
midpoint of that screen edge, and `player` is your own character's box centre - the edge-anchored
ones are anchored to the view and slide sideways when the camera pans, while `player` stays on
the character the line comes from. `player` falls back to the bottom edge on a frame where the
local pawn is not in the entity list, so the line never jumps into a corner. There is no skeleton or
health setting: the renderer has no bone or health source, so neither would draw anything, and a row
that draws nothing is worse than no row.

**Anything the camera is not showing used to vanish.** A screen-space box needs a projection, and
`Projection::WorldToScreen` returns false for a point behind the camera: an enemy ten metres behind
you had an empty box and drew nothing, though it was in the entity list the whole time. Markers
(`esp_offscreen`, reach `offscreen_range_m`, both in the menu's ESP section) close that. An entity
inside the marker range that is behind the camera - or in front of it but outside the viewport, where
a box has no visible pixels - gets an arrow pinned to the screen border in its direction, labelled
with the name and distance its box would have carried, in its category's colour. The direction is
resolved on the yaw plane, which is the plane the question is asked in: forward is up the frame and
right is right, so something behind you pins to the bottom edge - the one case a projection cannot
produce, because every point behind the camera collapses into that single direction. Ahead-but-off
to the side lands on that side's edge, which is also where an off-viewport box belongs.
**Marker snaplines** (`esp_offscreen_snapline`, menu row **Marker snaplines**) draw the line a box
would have got - from the same `esp_snapline` origin, out to the marker's tip on the border. The arrow
says which way to turn; the line is what makes one marker findable when several share an edge. It
needs `esp_snapline` set to anything but `off`.
The category toggles, the local-player filter and Hide dead apply to a marker exactly as they do to a
box, so a marker is a box you cannot see yet; markers count against `max_entities_drawn` like boxes,
and the list is distance sorted, so the nearest win. The reach is shorter than `max_distance_m` on
purpose - a marker is read without turning, and at 200 m the frame would be nothing but arrows.

Box geometry, the range filter and the distance origin are asserted numerically by
`overlay_test.exe` against a recording `IOverlay`; the same frame is also rasterized to
`overlay_test.bmp`, which is the only way to look at the stroke feathering, the menu panel
and the performance panel.

Distances are in metres, measured **from your character**, not from the camera: the
third-person camera sits behind and above the pawn and zooms out tens of metres, so a
camera-relative range overstates every distance by the zoom level. `Entity::Distance` stays
in world units (cm); the UI divides by 100 and `max_distance_m` is metres.

Strokes are feathered. A stroke's quad is half a pixel wider on each side than the stroke
itself and carries a perpendicular coordinate plus the stroke's half-width, so the pixel
shader blends the boundary band by the coverage the stroke really has there (`Overlay.cpp`).
An axis-aligned 1px stroke still evaluates to exactly 1.0 on the pixel it covers and 0.0
beside it, so box edges keep the crisp hairline they always had, while a long diagonal -
every snapline - stops hairline-sawing. For the same reason the snapline runs to the
*unrounded* box centre: a target snapped to whole pixels makes the line twitch as the camera
swings, which is a 1px step at the far end of a 700px line.

## Offset validation (against the LIVE process)

Classified by following real data and by UE reflection (`FProperty::Offset_Internal`),
never guessed. `Supplied` = value from the task brief.

### Globals

Current values are for the 2026-10-14 client (156,590,880 bytes), derived from the running client.
The three data globals did **not** move by one shift this time: `GObjects` came out at 10-09 `+0xB000`,
`GNames` at `+0xB180` and `GWorld` at `+0xB070`. The "one layout shift, apply it to everything"
premise that the 10-07 (`+0x12000`) and the 10-09 (`-0x14000`) patches both satisfied is therefore not
a rule, and each value was located on its own evidence:

* `GObjects` is the only array-shaped location within +-16 MiB with `MaxElements` 2,162,688 (33 chunks
  - the same capacity 10-07 and 10-09 had), a live chunk table, and a first item that decodes to the
  `/Script/CoreUObject` `Package`. It held 284,261 live objects over 5 chunks when it was checked.
* `GNames` is the pool-shaped location that decodes *those* objects' names: 64 of 64 sampled, with
  index 3 `ByteProperty` and 0x5BD `/Script/CoreUObject` - the same two probes the 10-09 entry was
  checked with. This is the check `tools\find_globals.py` cannot make: its pool probe reads entry
  indexes 0x10001/0x10002, which sit in the middle of a block-1 entry, so it rejects the real pool and
  reports no pair at all on this client.
* `GWorld` is the only slot within +-512 KiB of the previous RVA holding a heap `UObject` of class
  `World` with a resolvable level and actor count: `UWorld 'World_D_Starter'` (outer
  `/Game/Map/World/World_D/world_d_starter/World_D_Starter`) -> `PersistentLevel` -> live actor array.
  `tools\find_shift.py` checks this one structurally, which is why the non-uniform shift surfaced
  instead of being applied silently.

`ProcessEvent` is code, and code does not move with the data region either: `+0x730` on this patch,
against the data globals' `+0xB0xx`. It is checked the way the code uses it - slot `0x4D` of a live
object's vtable. On this build the same pointer sits at `0x4D` in 34 of 36 distinct sampled vtables,
it is the modal value of no other slot from `0x00` to `0x120`, and the 12 bytes at it are a real
function entry (`push rbp; push rsi; push rdi; push r12-r15`) - the test the 10-07 value failed.

`GEngine` and `LocalPlayer` are no longer in the table at all: the 10-09 values landed on a `UClass`
and on an address with no class, and nothing (overlay, recon, survey) read either one, so 2026-10-11
removed them instead of carrying two globals that had never been derived for a build.

History, 10-07 (156,604,184 bytes): the same three globals advanced by `0x12000`, and `GObjects` had to
be corrected by `-0x10` - the supplied `0xEC8C9A0` addressed the `ObjObjects` sub-object, so read as a
struct base it made the code treat the `Objects` pointer as the chunk table and every count came out
nonsense. Subtracting `0x10` put the counters back on the `+0x20/+0x24/+0x2C` this code reads; the
note in `Offsets.hpp` keeps the details.

The tests are structural, not range-based - these pointers address engine objects allocated by the
client's own allocator, which need not lie inside the mapped image - so each one is checked the way
the code downstream uses it: the pool has to decode names, the object array has to be live, the world
has to reach a level with actors, and `ProcessEvent` has to appear at slot `0x4D` of a live object's
vtable.

When a patch moves the globals, `python tools\find_shift.py` reports the shift and validates each
value, and `python tools\verify_globals.py GOBJECTS_RVA GNAMES_RVA` confirms a candidate pair against
the running client. Both need the client running **and in the world** — before that the globals are
legitimately empty, which is also why an overlay started at the launcher waits rather than failing:

| Symbol | RVA | Status | Note |
|---|---|---|---|
| GNames | `0xF12B5C0` | re-derived (`+0xB180`) — VALIDATED | `FNamePool` **by value** (no pointer indirection); index 3 decodes to `ByteProperty`, 0x5BD to `/Script/CoreUObject`, and it names all 400 objects of the array's first chunk |
| GObjects | `0xEC83990` | re-derived (`+0xB000`) — VALIDATED | `FUObjectArray` head; MaxElements 2,162,688 = 33 chunks, holding 284,261 live objects over 5 chunks when checked. The address the engine references is `0x10` into the struct (see the history above) |
| GWorld | `0xEF8EC38` | re-derived (`+0xB070`) — VALIDATED | `UWorld*` -> `World_D_Starter` (class `World`) -> `PersistentLevel` -> live actors. UWorld is swapped while travelling, so a single read can catch it mid-zone |
| ProcessEvent | `0x04206400` | re-derived (`+0x730`) — VALIDATED | not called anywhere; confirmed at slot `0x4D` of a live `UObject` vtable, where 34 of 36 distinct sampled vtables hold this same pointer and no other slot `0x00`-`0x120` does |
| ProcessEventIndex | `0x4D` | VALIDATED | the vtable slot the RVA is reached through |
| GEngineLoop / StaticFindObject / AppendString | `0` | UNKNOWN / unused | see `Symbols.hpp`; `AppendString` is deliberately unused — names are decoded from the `FNamePool` (`NamePool.hpp`), so there is no such dependency to resolve |

FUObjectArray layout (validated): `ObjFirstGCIndex 0x00`, `Objects 0x10`, `MaxElements 0x20`,
`NumElements 0x24`, `NumChunks 0x2C`, `ElementsPerChunk 0x10000`, `FUObjectItem.Size 0x18`.

### Struct offsets

| Field | Real | Supplied | Status |
|---|---|---|---|
| UObject VTable/Flags/InternalIndex/Class/Name/Outer | 0/8/0xC/0x10/0x18/0x20 | same | VALIDATED |
| UStruct SuperStruct / Children / ChildProperties / PropertiesSize | 0x40/0x48/0x50/0x58 | 0x40/0x48/–/– | VALIDATED |
| UField::Next | 0x28 | 0x28 | VALIDATED |
| FProperty::Offset_Internal / ElementSize | 0x44 / 0x34 | – | VALIDATED |
| UWorld PersistentLevel / Levels / OwningGameInstance | 0x30 / **0x180** / 0x1E8 | 0x30 / 0x178 / 0x1E8 | Levels corrected |
| ULevel Actors / WorldSettings | 0xA0 / 0x2C0 | 0xA0 / 0x2C0 | VALIDATED |
| UGameInstance LocalPlayers / UISystem / InputSystem | 0x38 / 0x3C8 / 0x418 | same | VALIDATED |
| UPlayer::PlayerController | 0x30 | 0x30 | VALIDATED |
| APlayerController Player / AcknowledgedPawn / MyHUD / PlayerCameraManager / PlayerInput | 0x3E0/0x3E8/0x3F0/0x3F8/0x4B8 | –/0x378(HUD,HUD+named MyHUD)/0x3F0/0x3F8/– | VALIDATED |
| AController::ControlRotation | **0x3B8** | 0x320 | INVALID (corrected) |
| APawn BaseEyeHeight / PlayerState / Controller | **0x344 / 0x358 / 0x370** | 0x2B4 / 0x2C8 / 0x2D8 | INVALID (corrected) |
| AActor CustomTimeDilation / RootComponent | 0x84 / 0x1C0 | same | VALIDATED |
| AActor InstanceComponents / BlueprintCreatedComponents | 0x288 / 0x298 | 0x280 / 0x290 | INVALID (corrected) |
| ACharacter Mesh / CharacterMovement / CapsuleComponent | 0x3C0/0x3C8/0x3D0 | same | VALIDATED |
| AionCharacter::CharActPlayRateControl | 0x22B0 | 0x22B0 | VALIDATED |
| APlayerCameraManager DefaultFOV / CameraCachePrivate | **0x350 / 0x1A70** | 0x2C0 / 0x1410 | INVALID (corrected) |
| FCameraCacheEntry::POV / FMinimalViewInfo FOV/DesiredFOV | 0x10 / 0x30 / 0x34 | same | VALIDATED |
| APlayerState::PlayerNamePrivate | **0x3D0** | 0x340 | INVALID (corrected) |
| USceneComponent RelativeLocation/Rotation/Scale3D | **0x148/0x160/0x178** | 0x128/0x140/0x158 | INVALID (corrected) |
| UCharacterMovementComponent JumpZVelocity/MovementMode/MaxWalkSpeed | 0x198/0x221/0x268 | same | VALIDATED |
| UPrimitiveComponent::BodyInstance | **0x3E8** | 0x348 | INVALID (corrected) |
| UCapsuleComponent HalfHeight/Radius | **0x5D8/0x5DC** | 0x508/0x50C | INVALID (corrected) |
| USkeletalMeshComponent GlobalAnimRateScale / CachedComponentSpaceTransforms | 0xAF0 / **0xA40** | 0xAF0 / 0x960 | Transforms corrected |
| UStaticMeshComponent::StaticMesh | **0x600** | 0x528 | INVALID (corrected) |
| UUserWidget::WidgetTree | 0x268 | 0x268 | VALIDATED |

**Key UE5/engine facts discovered:** world vectors/rotators are **doubles** (LWC);
`GNames` is an `FNamePool` **by value**; `GObjects` head is at a different RVA than supplied;
Aion player state name is an `FString` at `0x3D0`.

## Aion-specific systems

`Aion.hpp` exposes the validated `AionGameInstance` manager graph (all confirmed live):

| Manager | Offset | Class observed |
|---|---|---|
| UISystem | 0x3C8 | AionUISystem |
| WorldManager | 0x3D0 | AionWorldManager |
| NetworkManager | 0x3E8 | — |
| GameFlowSystem | 0x410 | AionGameFlowSystem |
| InputSystem | 0x418 | AionInputSystem |
| CharSession | 0x430 | AionCharSession |
| SettingManager | 0x470 | — |
| ChatSystem | 0x4B8 | — |

`AionUISystem` sub-objects (validated live): `LoadedDialogMap 0x60`, `ScreenMessageManager 0x380`,
`SystemMessageManager 0x388`. **`0x60` is not a UObject**: it is the `LoadedDialogMap` the SDK dump
declares there, a `TMap<DialogKey, AionDialog*>` (0x50 bytes, element stride 24, value at +16 - all
established by reflection over the map property's own properties). It holds `AionDialog` instances and
no widgets, which is why it is not an HP/MP route: see `build/self_hpmp_final.md`. The supplied
**`AionUISystem::WidgetTree 0x680` is INVALID** — no such pointer exists in this build.

Live widget inventory (runtime object scan):
* `QuickSlot*` classes: **105 instances**
* `SkillSlot*` classes: **57 instances**
* `Telegraph*`, `BlockWindow*`, `SkillBar*`: **0** — these class names do not exist in
  this build, so the supplied entries are unverified/unresolved naming, not confirmed
  structures.

UI widget field offsets (supplied `HUD_QuickSlot*`, `AionWidget_*`, `AionUIComponent_*`)
are resolved **by name** through reflection at runtime (`AionSystems::ResolveField`) rather
than hard-coded, so they survive layout shifts. Members not exposed as UPROPERTYs (e.g.
most `AionUISystem`/`AionInputSystem` internals) remain UNVALIDATED and are probed, not
assumed.

## Limitations / unresolved

* **Offset currency**: the values in `Offsets.hpp` belong to one client build - `Sdk::VerifyOffsets()`
  decides per session whether they still hold, and `tools\find_shift.py` re-derives them when they do
  not. `GEngine`/`LocalPlayer` could not be located for the 2026-10-09 client: they do not follow the
  data region's shift, and no client has validated them since 10-06. They feed only the survey's
  distances, so they are reported as not updated rather than guessed at.
* **NCGuard unpacking**: AION2.exe is packed - on disk the image is nothing but `.ncg0..6` (`.ncg0` is
  145 MB raw, inflating to a 407 MB image, entropy 7.99), so there is no code to scan in the file at
  all: AOB and xref tooling on the executable finds nothing. In memory the code pages are protected
  too (bands around `0x2000000`, `0xA000000` and `0xC000000` refuse `ReadProcessMemory`), which is why
  `ProcessEvent` is confirmed through a live object's vtable slot rather than by scanning for it. Only
  the data region (roughly `0xEC00000` upward) is readable, and that is where the globals live.
* **Health/state**: no reliable health field located yet; exposed as -1.
* **Skill/telegraph/block-window data**: manager graph and reflection access are in place;
  the concrete skill-record layout is not yet fully reverse-engineered and is reported as
  discovered rather than assumed.

## Safety

Read-only by default. The tool never injects, hooks the game, or writes game memory. Nothing
in the external overlay patches code or game data.
