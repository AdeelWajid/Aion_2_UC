// recon.cpp — read-only live validation/ reconnaissance tool for AION2 (external).
//
// Exercises the full UE layer (NamePool, GObjects, reflection, world graph, camera,
// projection, entity cache) against the running process. No writes, no injection.
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>

#include "aion/Actors.hpp"
#include "aion/Aion.hpp"
#include "aion/Camera.hpp"
#include "aion/EntityCache.hpp"
#include "aion/GObjects.hpp"
#include "aion/Logger.hpp"
#include "aion/Memory.hpp"
#include "aion/NamePool.hpp"
#include "aion/Offsets.hpp"
#include "aion/Projection.hpp"
#include "aion/Reflection.hpp"
#include "aion/Sdk.hpp"
#include "aion/Symbols.hpp"
#include "aion/UObject.hpp"
#include "aion/World.hpp"

#if defined(_WIN32)
#  include <windows.h>
static bool GameClientSize(std::uint32_t pid, int& w, int& h) {
    struct Ctx { std::uint32_t pid; HWND hwnd; } ctx{pid, nullptr};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            DWORD wp = 0;
            GetWindowThreadProcessId(h, &wp);
            if (wp == c->pid && IsWindowVisible(h)) {
                RECT r{};
                if (GetClientRect(h, &r) && (r.right - r.left) > 200) {
                    c->hwnd = h;
                    return FALSE;
                }
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    if (!ctx.hwnd) return false;
    RECT r{};
    if (!GetClientRect(ctx.hwnd, &r)) return false;
    w = r.right - r.left;
    h = r.bottom - r.top;
    return w > 0 && h > 0;
}
#else
static bool GameClientSize(std::uint32_t, int&, int&) { return false; }
#endif

using namespace aion;

int main(int argc, char** argv) {
    Logger::Instance().SetLevel(argc > 1 && std::string(argv[1]) == "-v" ? LogLevel::Debug
                                                                          : LogLevel::Info);
    const std::uint32_t pid = ExternalMemory::FindPidByName(L"AION2.exe");
    if (!pid) {
        AION_ERROR("AION2.exe not running");
        return 1;
    }
    ExternalMemory mem;
    if (!mem.Attach(pid)) return 2;
    AION_INFO("attached pid=%u base=0x%llX", pid, (unsigned long long)mem.module_base());

    Sdk sdk;
    if (!sdk.Init(&mem)) {
        AION_ERROR("Sdk init failed");
        return 3;
    }

    // ---- NamePool -----------------------------------------------------------
    std::printf("%s", aion::SectionForStdio("name pool").c_str());
    for (u32 i = 0; i < 6; ++i) printf("  name[%u] = '%s'\n", i, sdk.pool.Get(i).c_str());

    // ---- GObjects -----------------------------------------------------------
    std::printf("%s", aion::SectionForStdio("object array").c_str());
    printf("  NumElements=%d  MaxElements=%d\n", sdk.objects.Num(),
           mem.i32_at(sdk.gobjects + off::uobject_array::MaxElements));
    for (i32 i : {0, 1, 2, 1000, 400000, sdk.objects.Num() - 1}) {
        UObject o(&sdk, sdk.objects.ObjectAt(i));
        printf("  obj[%7d] = 0x%llX '%s' : '%s'\n", i, (unsigned long long)o.Addr(),
               o.GetName().c_str(), o.GetClassName().c_str());
    }

    // ---- Reflection ---------------------------------------------------------
    std::printf("%s", aion::SectionForStdio("reflection").c_str());
    Reflection refl(&sdk);
    const char* classes[] = {"Actor", "Pawn", "Character", "PlayerController",
                             "PlayerCameraManager", "World", "Level", "GameInstance",
                             "LocalPlayer", "AionCharacter", "PlayerState", "SceneComponent"};
    std::map<std::string, addr_t> cls;
    for (const char* cn : classes) {
        UObject c = refl.FindClass(cn);
        cls[cn] = c.Addr();
        if (c) {
            auto props = refl.EnumerateProperties(c.Addr());
            auto funcs = refl.EnumerateFunctions(c.Addr());
            printf("  %-22s 0x%-12llX props=%zu funcs=%zu full=%s\n", cn,
                   (unsigned long long)c.Addr(), props.size(), funcs.size(),
                   c.GetFullName().c_str());
        } else {
            printf("  %-22s NOT FOUND\n", cn);
        }
    }
    // Reflection-derived offsets: check a couple of known field names.
    if (cls["PlayerCameraManager"]) {
        for (const char* p : {"CameraCachePrivate", "DefaultFOV", "LastFrameCameraCachePrivate"}) {
            PropertyInfo pi = refl.GetProperty(cls["PlayerCameraManager"], p);
            printf("  PlayerCameraManager.%-26s off=0x%-5X size=%-5d type=%s\n", p, pi.Offset,
                   pi.Size, pi.Type.c_str());
        }
        printf("  PlayerController.MyHUD off=0x%X\n",
               refl.GetPropertyOffset(cls["PlayerController"], "MyHUD"));
        printf("  Pawn.PlayerState off=0x%X\n",
               refl.GetPropertyOffset(cls["Pawn"], "PlayerState"));
        printf("  Controller.ControlRotation off=0x%X\n",
               refl.GetPropertyOffset(refl.FindClass("Controller").Addr(), "ControlRotation"));
    }

    // ---- World graph --------------------------------------------------------
    std::printf("%s", aion::SectionForStdio("world").c_str());
    World world(&sdk);
    Level level(&sdk);
    UObject uworld = world.Get();
    printf("  UWorld=0x%llX '%s' : %s\n", (unsigned long long)uworld.Addr(),
           uworld.GetName().c_str(), uworld.GetClassName().c_str());
    UObject pl = world.PersistentLevel();
    printf("  PersistentLevel=0x%llX '%s'\n", (unsigned long long)pl.Addr(), pl.GetName().c_str());
    auto levels = world.Levels();
    printf("  Levels count=%zu\n", levels.size());
    auto actors = level.Actors(pl.Addr());
    printf("  Actors in persistent level=%zu\n", actors.size());
    std::map<std::string, int> hist;
    for (addr_t a : actors) {
        UObject o(&sdk, a);
        if (!o.Addr()) continue;
        hist[o.GetClassName()]++;
    }
    int shown = 0;
    for (auto& kv : hist) {
        if (shown++ >= 12) break;
        printf("     %-40s %d\n", kv.first.c_str(), kv.second);
    }

    // ---- Player chain -------------------------------------------------------
    std::printf("%s", aion::SectionForStdio("player chain").c_str());
    UObject gi = world.OwningGameInstance();
    printf("  GameInstance=0x%llX : %s\n", (unsigned long long)gi.Addr(),
           gi.GetClassName().c_str());
    auto local_players = mem.read_tarray<addr_t>(gi.Addr() + off::ugameinstance::LocalPlayers);
    printf("  LocalPlayers=%zu\n", local_players.size());
    addr_t pc = 0, pawn = 0;
    if (!local_players.empty()) {
        pc = mem.ptr_at(local_players[0] + off::uplayer::PlayerController);
        UObject pco(&sdk, pc);
        printf("  PlayerController=0x%llX : %s\n", (unsigned long long)pc,
               pco.GetClassName().c_str());
        pawn = mem.ptr_at(pc + off::aplayercontroller::AcknowledgedPawn);
        UObject po(&sdk, pawn);
        printf("  Pawn=0x%llX : %s '%s'\n", (unsigned long long)pawn, po.GetClassName().c_str(),
               po.GetName().c_str());
        addr_t hud = mem.ptr_at(pc + off::aplayercontroller::MyHUD);
        addr_t input = mem.ptr_at(pc + off::aplayercontroller::PlayerInput);
        printf("  HUD=0x%llX '%s'  Input=0x%llX '%s'\n", (unsigned long long)hud,
               UObject(&sdk, hud).GetClassName().c_str(), (unsigned long long)input,
               UObject(&sdk, input).GetClassName().c_str());
        addr_t ps = mem.ptr_at(pawn + off::apawn::PlayerState);
        printf("  PlayerState=0x%llX name='%s'\n", (unsigned long long)ps,
               mem.read_fstring(ps + off::aplayer_state::PlayerNamePrivate).c_str());
    }

    // ---- Camera + projection ------------------------------------------------
    std::printf("%s", aion::SectionForStdio("camera").c_str());
    CameraExtractor camx(&sdk);
    Camera cam = camx.FromPlayerController(pc);
    printf("  loc=(%.2f, %.2f, %.2f) rot=(P%.2f Y%.2f R%.2f) fov=%.2f (desired %.2f)\n", cam.Location.X,
           cam.Location.Y, cam.Location.Z, cam.Rotation.Pitch, cam.Rotation.Yaw, cam.Rotation.Roll,
           cam.FOV, cam.DesiredFOV);

    int sw = 0, sh = 0;
    if (!GameClientSize(pid, sw, sh)) { sw = 1920; sh = 1080; }
    printf("  viewport = %dx%d\n", sw, sh);
    Projection proj;
    proj.SetViewport(sw, sh);

    // ---- Entity cache -------------------------------------------------------
    std::printf("%s", aion::SectionForStdio("entity cache").c_str());
    EntityCache cache;
    cache.Update(&sdk, cam, proj);
    const auto& ents = cache.Entities();
    printf("  entities=%zu (sorted near->far)\n", ents.size());
    int n = 0;
    for (const auto& e : ents) {
        if (!e.OnScreen) continue;
        Actor a(&sdk, e.Addr);
        const bool self = (e.Addr == pawn);
        printf("  %-34s d=%8.1f scr=(%7.1f,%7.1f) box=(%.0f,%.0f)-(%.0f,%.0f)%s\n",
               e.ClassName.c_str(), e.Distance, e.Screen.X, e.Screen.Y, e.BoundsMin.X,
               e.BoundsMin.Y, e.BoundsMax.X, e.BoundsMax.Y, self ? "  <== SELF" : "");
        if (++n >= 20) break;
    }

    // ---- Aion systems -------------------------------------------------------
    std::printf("%s", aion::SectionForStdio("aion systems").c_str());
    {
        AionSystems aion(&sdk, &refl);
        UObject ui = aion.UISystem(), inp = aion.InputSystem(), wm = aion.WorldManager(),
                 gfs = aion.GameFlowSystem(), cs = aion.CharSession();
        printf("  UISystem=0x%llX %s\n", (unsigned long long)ui.Addr(), ui.GetClassName().c_str());
        printf("  InputSystem=0x%llX %s\n", (unsigned long long)inp.Addr(),
               inp.GetClassName().c_str());
        printf("  WorldManager=0x%llX %s  GameFlowSystem=0x%llX %s  CharSession=0x%llX %s\n",
               (unsigned long long)wm.Addr(), wm.GetClassName().c_str(),
               (unsigned long long)gfs.Addr(), gfs.GetClassName().c_str(),
               (unsigned long long)cs.Addr(), cs.GetClassName().c_str());
        printf("  LoadedDialogMap=0x%llX (+0x60 in CharSession, TMap<DialogKey,AionDialog*>, not an "
               "object)  ScreenMsgMgr=0x%llX %s  SystemMsgMgr=0x%llX %s\n",
               (unsigned long long)aion.LoadedDialogMapAddr(),
               (unsigned long long)aion.ScreenMessageManager().Addr(),
               aion.ScreenMessageManager().GetClassName().c_str(),
               (unsigned long long)aion.SystemMessageManager().Addr(),
               aion.SystemMessageManager().GetClassName().c_str());

        // Live widget inventory for skill / quickslot / telegraph / block UI.
        const char* keywords[] = {"QuickSlot", "SkillSlot", "Telegraph", "BlockWindow", "SkillBar"};
        for (const char* kw : keywords) {
            int count = 0;
            addr_t sample = 0;
            for (i32 i = 0; i < sdk.objects.Num(); ++i) {
                const addr_t o = sdk.objects.ObjectAt(i);
                if (!o) continue;
                const std::string cn = UObject(&sdk, o).GetClassName();
                if (cn.find(kw) != std::string::npos) {
                    if (!count) sample = o;
                    ++count;
                }
            }
            printf("  live instances with class containing '%s': %d%s\n", kw, count,
                   count ? "" : "");
            if (sample)
                printf("      e.g. %s '%s'\n", UObject(&sdk, sample).GetClassName().c_str(),
                       UObject(&sdk, sample).GetName().c_str());
        }
    }

    // ---- Symbol resolution (best-effort, evidence-based) --------------------
    std::printf("%s", aion::SectionForStdio("symbol resolution").c_str());
    Symbols sym(&sdk, &mem);
    struct GB { const char* name; addr_t addr; } globals[] = {
        {"GObjects", sdk.gobjects},
        {"GNames", sdk.gnames},
        {"GWorld", sdk.gworld},
    };
    for (auto& g : globals) {
        auto refs = sym.FindCodeRefs(g.addr);
        printf("  %-12s @0x%llX  code-refs=%zu", g.name, (unsigned long long)g.addr, refs.size());
        for (size_t i = 0; i < refs.size() && i < 4; ++i)
            printf("  0x%llX", (unsigned long long)refs[i].Instruction);
        printf("\n");
    }
    auto slots = sym.ModalVtableSlots(256, 3000);
    printf("  modal vtable slots (ProcessEvent candidate among these, UNVALIDATED):\n");
    for (size_t i = 0; i < slots.size() && i < 10; ++i)
        printf("     slot %3d -> 0x%llX  (shared by %d sampled objects)\n", slots[i].Slot,
               (unsigned long long)slots[i].Func, slots[i].Count);

    mem.Close();
    return 0;
}
