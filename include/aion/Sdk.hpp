// Sdk.hpp — shared runtime context: memory backend + resolved globals + helpers.
#pragma once
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include "GObjects.hpp"
#include "Memory.hpp"
#include "NamePool.hpp"
#include "Offsets.hpp"

namespace aion {

struct FName {
    u32 Index = 0;
    u32 Number = 0;
};

// UE5 LWC: world-space vectors/rotators are double precision (validated live).
struct FVector {
    double X = 0, Y = 0, Z = 0;
    FVector() = default;
    FVector(double x, double y, double z) : X(x), Y(y), Z(z) {}
    FVector operator+(const FVector& o) const { return {X + o.X, Y + o.Y, Z + o.Z}; }
    FVector operator-(const FVector& o) const { return {X - o.X, Y - o.Y, Z - o.Z}; }
    double Dot(const FVector& o) const { return X * o.X + Y * o.Y + Z * o.Z; }
    double Length() const { return std::sqrt(X * X + Y * Y + Z * Z); }
    FVector operator*(double s) const { return {X * s, Y * s, Z * s}; }
};

struct FRotator {
    double Pitch = 0, Yaw = 0, Roll = 0;
};

struct FVector2D {
    float X = 0, Y = 0;
};

// Runtime context. One instance per session; thread-safety is the caller's concern.
class Sdk {
public:
    IMemory* mem = nullptr;
    addr_t base = 0;
    addr_t gnames = 0, gobjects = 0, gworld = 0;

    NamePool pool;
    UObjectArray objects;
    bool ready = false;

    // log=false makes this a probe: the caller polls it while the client boots, then runs it
    // once with logging so the session records the resolved state.
    bool Init(IMemory* memory, bool log = true) {
        mem = memory;
        base = mem->module_base();
        if (!base) return false;
        gnames     = base + off::globals::GNames;
        gobjects   = base + off::globals::GObjects;
        gworld     = base + off::globals::GWorld;
        // GEngine and LocalPlayer are deliberately not resolved: on this build they pointed at a
        // UClass object and at a non-object, and nothing read them. See Offsets.hpp's globals block.

        const bool pool_ok = pool.Init(mem, gnames, log);
        const bool obj_ok = objects.Init(mem, gobjects, log);
        ready = pool_ok && obj_ok;
        if (log) AION_DEBUG("Sdk base=0x%llX ready=%d", (unsigned long long)base, (int)ready);
        if (ready) {
            // The path is printed once, and the build is compared rather than restated: the header
            // already carries the verified size, so repeating it on every start says nothing new.
            // The line to copy between machines is the offset verdict below.
            const std::string exe = mem->image_path();
            const std::uint64_t size = mem->image_file_size();
            if (log && !exe.empty()) AION_INFO("target: %s", exe.c_str());
            if (log && size != 0 && size != off::build::VerifiedExeSize)
                AION_WARN("client build differs: %llu bytes, verified %llu - offsets may not hold",
                          (unsigned long long)size,
                          (unsigned long long)off::build::VerifiedExeSize);
            if (log) VerifyOffsets();
        } else if (log) {
            Diagnose();
        }
        return ready;
    }

    // Reads every supplied global back through the live client and says, in the offset table's own
    // vocabulary, whether it holds. The tests are structural, not range-based: these pointers
    // address engine objects allocated by the client's own allocator, which need not lie inside the
    // mapped image, so an image-range check would call every working pointer UNVALIDATED. Each test
    // is the one the code downstream performs anyway - the pool has to decode, the array has to be
    // live, the world has to reach a level with actors - so a global that passes here is the one
    // the ESP will actually use. Only meaningful once ready; before that they legitimately read 0.
    void VerifyOffsets() const {
        const u32 cursor = rd<u32>(gnames + off::name_pool::CurrentByteCursor);
        const u32 block = rd<u32>(gnames + off::name_pool::CurrentBlock);
        const bool pool_live = mem->valid_ptr(ptr(gnames + off::name_pool::Blocks)) &&
                               cursor < off::name_pool::BlockSize &&
                               block < off::name_pool::MaxBlocks;
        AION_INFO("offset GNames      rva 0x%08X -> cursor=%u block=%u  %s", off::globals::GNames,
                  cursor, block,
                  pool_live ? "VALIDATED for this client" : "UNVALIDATED (no live pool here)");

        AION_INFO("offset GObjects    rva 0x%08X -> NumElements=%d  %s", off::globals::GObjects,
                  objects.Num(),
                  (objects.ok() && objects.Num() > 0) ? "VALIDATED for this client"
                                                      : "UNVALIDATED (no live object array here)");

        const addr_t world = ptr(gworld);
        const addr_t level = mem->valid_ptr(world) ? ptr(world + off::uworld::PersistentLevel) : 0;
        const std::size_t actors =
            mem->valid_ptr(level)
                ? mem->read_tarray<addr_t>(level + off::ulevel::Actors, 100000).size()
                : 0;
        AION_INFO("offset GWorld      rva 0x%08X -> 0x%016llX level=0x%016llX actors=%zu  %s",
                  off::globals::GWorld, (unsigned long long)world, (unsigned long long)level, actors,
                  actors ? "VALIDATED for this client" : "UNVALIDATED (no world with actors here)");

        // GEngine and LocalPlayer are not checked here any more, because they are not listed any
        // more: the class-name test that used to run here showed the first resolving to an object
        // whose class is 'Class' (a UClass, not a UEngine) and the second to an address with no
        // class at all on this client. Nothing read either, so both were removed.

        const addr_t pe = base + off::globals::ProcessEvent;
        // Any live object's vtable will do - every instance of a class shares it. Item 0 of the
        // array is often an empty slot, so take the first non-null object.
        addr_t vt = 0;
        for (i32 i = 1; i < 32 && !vt; ++i) {
            const addr_t obj = objects.ObjectAt(i);
            if (mem->valid_ptr(obj)) vt = ptr(obj);
        }
        constexpr int kSlots = 0x200;
        std::vector<addr_t> table(kSlots, 0);
        int found = -1;
        if (mem->valid_ptr(vt) && mem->read(vt, table.data(), table.size() * sizeof(addr_t)))
            for (int i = 0; i < kSlots; ++i)
                if (table[i] == pe) { found = i; break; }

        const std::uint32_t want = off::globals::ProcessEventIndex;
        if (!mem->valid_ptr(vt))
            AION_WARN("offset ProcessEvent rva 0x%08X index 0x%X -> UNKNOWN (no live vtable to "
                      "check it against)", off::globals::ProcessEvent, want);
        else if (found == static_cast<int>(want))
            AION_INFO("offset ProcessEvent rva 0x%08X index 0x%X -> VALIDATED (slot 0x%X of a live "
                      "UObject vtable)", off::globals::ProcessEvent, want, found);
        else if (found >= 0)
            AION_ERROR("offset ProcessEvent rva 0x%08X index 0x%X -> INVALID: a live vtable holds "
                       "that address at slot 0x%X", off::globals::ProcessEvent, want, found);
        else
            AION_WARN("offset ProcessEvent rva 0x%08X index 0x%X -> UNKNOWN (that address is not in "
                      "the first 0x%X slots of a live vtable)", off::globals::ProcessEvent, want,
                      kSlots);
    }

    // The client publishes GNames first and GObjects a few seconds later - measured live, 2.4 s
    // apart on the same install - so an overlay started during that window used to exit with
    // "SDK init failed" while the client was simply still booting. Poll quietly until both are
    // live, then resolve once with logging. A false return has already printed a diagnosis.
    bool WaitReady(IMemory* memory, int wait_ms, int poll_ms = 250) {
        const auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            if (Init(memory, /*log=*/false)) return Init(memory, /*log=*/true);
            const int waited = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0)
                    .count());
            if (waited >= wait_ms) return Init(memory, /*log=*/true);
            std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
            if (waited / 1000 != (waited + poll_ms) / 1000)
                AION_INFO("waiting for the client's runtime objects (%d/%d ms, objects=%d)", waited,
                          wait_ms, objects.Num());
        }
    }

    // Why Init() failed, in the two terms that matter on a foreign machine: did the reads
    // reach the client at all, and is this client the build the offsets were derived from?
    // A refused process handle leaves the same all-zero globals as a different build does,
    // so prove the reader with the image's own PE header first, then compare the file.
    void Diagnose() const {
        const u16 mz = rd<u16>(base);
        const u32 lfanew = rd<u32>(base + 0x3C);
        const bool hdr = mz == 0x5A4D && lfanew >= 0x40 && lfanew < 0x1000;

        AION_INFO("--- attach diagnosis ------------------------------------------------");
        AION_INFO("reader: pid=%u reopens=%u open_failed=%u last_open_err=%u stripped=%u",
                  mem->pid(), mem->reopen_count(), mem->open_fail_count(), mem->last_open_error(),
                  mem->stripped_count());
        AION_INFO("module: base=0x%llX size=0x%llX", (unsigned long long)base,
                  (unsigned long long)mem->module_size());
        if (hdr) {
            AION_INFO("pe: MZ ok machine=0x%X stamp=0x%08X SizeOfImage=0x%X - the client's own "
                      "header read back over ReadProcessMemory",
                      rd<u16>(base + lfanew + 0x04), rd<u32>(base + lfanew + 0x08),
                      rd<u32>(base + lfanew + 0x50));
        } else {
            AION_ERROR("pe: MZ=0x%04X lfanew=0x%X - the client's own image header did not read back",
                       mz, lfanew);
        }
        const std::string exe = mem->image_path();
        const std::uint64_t fsz = mem->image_file_size();
        if (!exe.empty())
            AION_INFO("client: %s (%llu bytes; verified build is %llu bytes)", exe.c_str(),
                      (unsigned long long)fsz, (unsigned long long)off::build::VerifiedExeSize);
        AION_INFO("globals: GNames block=%u cursor=%u block0=0x%llX | GObjects chunks=0x%llX "
                  "num=%d max=%d",
                  rd<u32>(gnames + off::name_pool::CurrentBlock),
                  rd<u32>(gnames + off::name_pool::CurrentByteCursor),
                  (unsigned long long)rd<addr_t>(gnames + off::name_pool::Blocks),
                  (unsigned long long)rd<addr_t>(gobjects + off::uobject_array::Objects),
                  rd<i32>(gobjects + off::uobject_array::NumElements),
                  rd<i32>(gobjects + off::uobject_array::MaxElements));
        // Read *status* per global, not just the value: a mapped page of zeros (the client has
        // not initialised the pool yet) and an unmapped one (the RVA is wrong for this image)
        // both print as 0.
        struct GlobalRef {
            const char* name;
            addr_t addr;
        };
        const GlobalRef globals[] = {{"GNames", gnames},
                                     {"GObjects", gobjects},
                                     {"GWorld", gworld}};
        for (const GlobalRef& g : globals) {
            const auto word = mem->try_read<u64>(g.addr);
            AION_INFO("global %-11s @0x%llX  %s  first word 0x%016llX", g.name,
                      (unsigned long long)g.addr, word ? "read ok    " : "READ FAILED",
                      (unsigned long long)(word ? *word : 0));
        }

        if (!hdr)
            AION_ERROR("verdict: the reader is not reaching the client - ReadProcessMemory is being "
                       "refused. Run the overlay elevated, and check that security software is not "
                       "blocking OpenProcess/ReadProcessMemory on the game.");
        else if (fsz && fsz != off::build::VerifiedExeSize)
            AION_ERROR("verdict: different client build - %llu bytes, the offsets were verified "
                       "against %llu. They cannot be used on this install.",
                       (unsigned long long)fsz, (unsigned long long)off::build::VerifiedExeSize);
        else
            AION_ERROR("verdict: the reader works and the client matches, so the globals are empty. "
                       "Check the read status above: if every read succeeded, the client has not "
                       "initialised the name pool yet (a NULL block table is pre-init) - attach "
                       "once the game is in the world; if they FAILED, these RVAs are not mapped "
                       "in this image.");
    }

    template <class T> T rd(addr_t a) const { return mem->read_or<T>(a); }
    addr_t ptr(addr_t a) const { return mem->ptr_at(a); }
    std::string Name(u32 idx) const { return pool.Get(idx); }
    std::string Name(const FName& n) const { return pool.Get(n.Index); }

    FVector ReadVector(addr_t a) const {
        FVector v;
        mem->read(a, &v, sizeof(FVector));
        return v;
    }
    FRotator ReadRotator(addr_t a) const {
        FRotator r;
        mem->read(a, &r, sizeof(FRotator));
        return r;
    }
    FVector2D ReadVector2D(addr_t a) const {
        FVector2D v;
        mem->read(a, &v, sizeof(FVector2D));
        return v;
    }

    // Globals live as pointer cells.
    addr_t World() const { return mem->ptr_at(gworld); }
    // Engine() and LocalPlayerGlobal() lived here; both read the removed stale globals. Callers
    // reach the local player the way the frame loop does - UWorld -> OwningGameInstance ->
    // LocalPlayers[0] -> PlayerController -> AcknowledgedPawn.
};

namespace off {
inline const char* StatusName(Status s) {
    switch (s) {
        case Status::Validated: return "VALIDATED";
        case Status::Unvalidated: return "UNVALIDATED";
        case Status::Invalid: return "INVALID";
        case Status::Unknown: return "UNKNOWN";
    }
    return "?";
}
}  // namespace off

}  // namespace aion
