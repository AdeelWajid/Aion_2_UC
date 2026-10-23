// Symbols.hpp — best-effort discovery of unresolved engine symbols.
//
// Two evidence-based techniques are used (no guessing, results carry a confidence):
//   1. Code cross-references: scan the module for RIP-relative instructions whose
//      resolved target equals a known global (e.g. GObjects) -> candidate accessors
//      such as StaticFindObject / AppendString.
//   2. Modal vtable slot: virtual functions that are rarely overridden (notably
//      UObject::ProcessEvent) resolve to the same address in nearly every class
//      vtable; the modal slot reveals them.
//
// Results are marked UNVALIDATED: they are strong candidates, but confirming a
// callable signature requires an in-process call, which is out of scope for the
// read-only external tool.
#pragma once
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "Sdk.hpp"

namespace aion {

struct CodeRef {
    addr_t Instruction = 0;  // address of the referencing instruction
    addr_t Target = 0;       // resolved absolute target
};

struct VtableSlotCand {
    int Slot = -1;
    addr_t Func = 0;
    int Count = 0;  // how many sampled objects share this slot/addr
};

class Symbols {
public:
    Symbols(Sdk* sdk, IMemory* mem) : sdk_(sdk), mem_(mem) {}

    // Scan the module image for [REX.W] {mov,lea} r64, [rip+disp32] resolving to target.
    std::vector<CodeRef> FindCodeRefs(addr_t target) {
        std::vector<CodeRef> out;
        const addr_t base = mem_->module_base();
        const std::size_t size = mem_->module_size();
        if (!base || !size) return out;
        std::vector<u8> buf(1 << 20);
        for (std::size_t off = 0; off + buf.size() <= size; off += buf.size()) {
            if (!mem_->read(base + off, buf.data(), buf.size())) continue;
            ScanChunk(buf.data(), buf.size(), base + off, target, size, base, out);
        }
        return out;
    }

    // Modal vtable slots across a spread sample of live UObjects.
    std::vector<VtableSlotCand> ModalVtableSlots(int max_slot = 256, int sample = 3000) {
        const i32 n = sdk_->objects.Num();
        if (n <= 0) return {};
        const int step = std::max(1, n / sample);
        std::vector<std::unordered_map<addr_t, int>> hist(max_slot);
        int objects = 0;
        for (i32 i = 0; i < n; i += step) {
            const addr_t o = sdk_->objects.ObjectAt(i);
            if (!o) continue;
            const addr_t vt = sdk_->ptr(o);
            if (!mem_->in_module(vt)) continue;
            ++objects;
            for (int s = 0; s < max_slot; ++s) {
                const addr_t fn = sdk_->ptr(vt + s * 8);
                if (fn && mem_->in_module(fn)) hist[s][fn]++;
            }
        }
        std::vector<VtableSlotCand> out;
        for (int s = 0; s < max_slot; ++s) {
            if (hist[s].empty()) continue;
            auto best = std::max_element(
                hist[s].begin(), hist[s].end(),
                [](const auto& a, const auto& b) { return a.second < b.second; });
            if (objects && best->second >= objects * 9 / 10)
                out.push_back({s, best->first, best->second});
        }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
            return a.Count > b.Count;
        });
        return out;
    }

private:
    static void ScanChunk(const u8* p, std::size_t n, addr_t chunk_addr, addr_t target,
                          std::size_t mod_size, addr_t base, std::vector<CodeRef>& out) {
        for (std::size_t i = 0; i + 7 <= n; ++i) {
            const u8 rex = p[i];
            if (rex < 0x48 || rex > 0x4F) continue;      // REX.W present
            const u8 op = p[i + 1];
            if (op != 0x8B && op != 0x8D) continue;       // mov / lea
            const u8 modrm = p[i + 2];
            if ((modrm & 0xC7) != 0x05) continue;         // [rip+disp32]
            i32 disp;
            std::memcpy(&disp, p + i + 3, 4);
            const addr_t instr = chunk_addr + i;
            const addr_t resolved = instr + 7 + (addr_t)(i64)disp;
            if (resolved == target)
                out.push_back({instr, resolved});
            (void)mod_size; (void)base;
        }
    }

    Sdk* sdk_;
    IMemory* mem_;
};

}  // namespace aion
