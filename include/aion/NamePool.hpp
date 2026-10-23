// NamePool.hpp — FNamePool decoder (UE5).
//
// GNames is the pool BY VALUE at the symbol address (validated); there is no extra
// pointer indirection. Entry header: uint16 { bIsWide:1, ProbeHash:5, Len:10 }.
#pragma once
#include <string>
#include <unordered_map>
#include <mutex>

#include "Memory.hpp"
#include "Offsets.hpp"

namespace aion {

class NamePool {
public:
    NamePool() = default;
    bool Init(IMemory* mem, addr_t pool_addr, bool log = true) {
        mem_ = mem;
        pool_ = pool_addr;
        const u32 cursor = mem_ ? mem_->u32_at(pool_ + off::name_pool::CurrentByteCursor) : 0;
        const u32 block  = mem_ ? mem_->u32_at(pool_ + off::name_pool::CurrentBlock) : 0;
        // Block and cursor alone cannot fail on a pool that read back as all zeros - 0 is a
        // legal block and a legal cursor - which is exactly how a refused process handle was
        // reported as ok=1. The block table must point somewhere: that read is the one that
        // proves the pool is live.
        const addr_t block0 = mem_ ? mem_->ptr_at(pool_ + off::name_pool::Blocks) : 0;
        const bool ok = mem_ && cursor < off::name_pool::BlockSize &&
                        block < off::name_pool::MaxBlocks && mem_->valid_ptr(block0);
        if (log)
            AION_DEBUG("NamePool @0x%llX CurrentBlock=%u Cursor=%u block0=0x%llX ok=%d",
                      (unsigned long long)pool_, block, cursor, (unsigned long long)block0, (int)ok);
        return ok;
    }

    // Decode FNameEntryId -> string. Returns empty on failure.
    std::string Get(u32 index) const {
        if (!index || !mem_) return {};
        {
            std::lock_guard<std::mutex> lk(cache_mu_);
            auto it = cache_.find(index);
            if (it != cache_.end()) return it->second;
        }
        std::string s = Decode(index);
        std::lock_guard<std::mutex> lk(cache_mu_);
        if (cache_.size() < 200000) cache_.emplace(index, s);
        return s;
    }

private:
    std::string Decode(u32 index) const {
        const u32 block = index >> 16;
        const u32 offset = (index & 0xFFFF) * off::name_pool::EntryStride;
        if (block >= off::name_pool::MaxBlocks) return {};
        const addr_t base = mem_->ptr_at(pool_ + off::name_pool::Blocks + block * 8);
        if (!mem_->valid_ptr(base)) return {};
        const u16 header = mem_->u16_at(base + offset);
        const bool wide = (header & 1) != 0;
        const u32 len = header >> 6;
        if (len == 0 || len > 250) return {};
        std::string out;
        out.resize(len);
        if (wide) {
            std::vector<char16_t> w(len);
            if (!mem_->read(base + offset + 2, w.data(), len * 2)) return {};
            for (u32 i = 0; i < len; ++i) out[i] = w[i] < 0x80 ? (char)w[i] : '?';
        } else {
            if (!mem_->read(base + offset + 2, out.data(), len)) return {};
            for (char& c : out)
                if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) return {};
        }
        return out;
    }

    IMemory* mem_ = nullptr;
    addr_t pool_ = 0;
    mutable std::mutex cache_mu_;
    mutable std::unordered_map<u32, std::string> cache_;
};

}  // namespace aion
