// UObject.hpp — UObject field accessors built on Sdk.
#pragma once
#include <algorithm>
#include <string_view>

#include "Sdk.hpp"

namespace aion {

// A non-owning handle to a live UObject. Cheap to copy; no lifetime beyond engine.
class UObject {
public:
    UObject() = default;
    UObject(Sdk* sdk, addr_t addr) : sdk_(sdk), addr_(addr) {}

    explicit operator bool() const { return IsValid(); }
    addr_t Addr() const { return addr_; }
    Sdk* Context() const { return sdk_; }

    bool IsValid() const {
        if (!sdk_ || !sdk_->mem->valid_ptr(addr_)) return false;
        const addr_t vt = sdk_->ptr(addr_ + off::uobject::VTable);
        if (!sdk_->mem->in_module(vt)) return false;
        return sdk_->rd<u32>(addr_ + off::uobject::InternalIndex) <
               (u32)(std::max)(sdk_->objects.Num(), 1) * 4u;
    }

    u32 InternalIndex() const { return sdk_->rd<u32>(addr_ + off::uobject::InternalIndex); }
    u32 Flags() const { return sdk_->rd<u32>(addr_ + off::uobject::Flags); }
    // The client marks an actor it has destroyed - a gathered node, a despawned corpse, a mob that
    // was removed - with RF_MirroredGarbage, but leaves it in the level's actor array until the GC
    // runs. Anything that walks the level has to skip those, or it draws what the game has killed.
    bool IsGarbage() const { return (Flags() & off::uobject::RF_MirroredGarbage) != 0; }
    FName GetFName() const { return sdk_->rd<FName>(addr_ + off::uobject::Name); }

    std::string GetName() const {
        if (!addr_) return {};
        return sdk_->Name(sdk_->rd<u32>(addr_ + off::uobject::Name));
    }
    UObject GetClass() const { return {sdk_, sdk_->ptr(addr_ + off::uobject::Class)}; }
    std::string GetClassName() const { return GetClass().GetName(); }
    UObject GetOuter() const { return {sdk_, sdk_->ptr(addr_ + off::uobject::Outer)}; }

    std::string GetFullName() const {
        std::string parts[32];
        int n = 0;
        addr_t cur = addr_;
        for (; n < 32 && cur; ++n) {
            UObject o(sdk_, cur);
            std::string nm = o.GetName();
            if (nm.empty()) return {};
            parts[n] = std::move(nm);
            cur = sdk_->ptr(cur + off::uobject::Outer);
        }
        std::string out;
        for (int i = n - 1; i >= 0; --i) {
            if (!out.empty()) out.push_back('.');
            out += parts[i];
        }
        return out;
    }

    // True if this object's class chain contains `className`.
    bool IsA(std::string_view className) const {
        UObject c = GetClass();
        for (int i = 0; i < 64 && c.Addr(); ++i) {
            if (c.GetName() == className) return true;
            c = UObject(sdk_, sdk_->ptr(c.Addr() + off::ustruct::SuperStruct));
        }
        return false;
    }

private:
    Sdk* sdk_ = nullptr;
    addr_t addr_ = 0;
};

}  // namespace aion
