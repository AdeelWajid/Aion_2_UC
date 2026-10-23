// GObjects.hpp — FUObjectArray / chunked object table access.
#pragma once
#include "Memory.hpp"
#include "Offsets.hpp"

namespace aion {

class UObjectArray {
public:
    bool Init(IMemory* mem, addr_t array_addr, bool log = true) {
        mem_ = mem;
        addr_ = array_addr;
        chunks_ = mem_->ptr_at(addr_ + off::uobject_array::Objects);
        num_ = mem_->i32_at(addr_ + off::uobject_array::NumElements);
        max_ = mem_->i32_at(addr_ + off::uobject_array::MaxElements);
        ok_ = mem_->valid_ptr(chunks_) && num_ > 0 && num_ <= max_;
        if (log)
            AION_DEBUG("GObjects @0x%llX NumElements=%d MaxElements=%d chunks=0x%llX ok=%d",
                      (unsigned long long)addr_, num_, max_, (unsigned long long)chunks_, (int)ok_);
        return ok_;
    }

    bool ok() const { return ok_; }
    i32 Num() const { return num_; }
    addr_t Chunks() const { return chunks_; }

    // Absolute address of the i-th FUObjectItem, or 0 if out of range.
    addr_t ItemAt(i32 i) const {
        if (i < 0 || i >= num_ || !ok_) return 0;
        const i32 chunk = i / (i32)off::uobject_array::ElementsPerChunk;
        const i32 within = i % (i32)off::uobject_array::ElementsPerChunk;
        const addr_t base = mem_->ptr_at(chunks_ + chunk * 8);
        if (!mem_->valid_ptr(base)) return 0;
        return base + (addr_t)within * off::uobject_item::Size;
    }

    addr_t ObjectAt(i32 i) const {
        const addr_t item = ItemAt(i);
        return item ? mem_->ptr_at(item + off::uobject_item::Object) : 0;
    }

private:
    IMemory* mem_ = nullptr;
    addr_t addr_ = 0, chunks_ = 0;
    i32 num_ = 0, max_ = 0;
    bool ok_ = false;
};

}  // namespace aion
