// Reflection.hpp — UClass/UFunction/FProperty discovery and introspection.
#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "UObject.hpp"

namespace aion {

struct PropertyInfo {
    std::string Name;
    std::string Type;    // FFieldClass name, when resolvable
    i32 Offset = -1;
    i32 Size = 0;
    addr_t Addr = 0;
};

struct FunctionInfo {
    std::string Name;
    addr_t Addr = 0;      // UFunction object address
    addr_t NativeFunc = 0;  // UFunction::Func (native entry point), if any
    u32 Flags = 0;
};

class Reflection {
public:
    explicit Reflection(Sdk* sdk) : sdk_(sdk) {}

    void SetSdk(Sdk* sdk) { sdk_ = sdk; ClearCache(); }
    void ClearCache() { class_cache_.clear(); }

    // ---- lookups -------------------------------------------------------------
    UObject FindClass(const std::string& name) {
        auto it = class_cache_.find(name);
        if (it != class_cache_.end()) return {sdk_, it->second};
        addr_t found = 0;
        for (i32 i = 0; i < sdk_->objects.Num(); ++i) {
            const addr_t o = sdk_->objects.ObjectAt(i);
            if (!o) continue;
            if (NameOf(o) != name) continue;
            const addr_t cls = sdk_->ptr(o + off::uobject::Class);
            if (NameOf(cls) == "Class") { found = o; break; }
        }
        class_cache_[name] = found;
        return {sdk_, found};
    }

    UObject FindObject(const std::string& name, const std::string& className = {}) {
        for (i32 i = 0; i < sdk_->objects.Num(); ++i) {
            const addr_t o = sdk_->objects.ObjectAt(i);
            if (!o || NameOf(o) != name) continue;
            if (!className.empty()) {
                const addr_t cls = sdk_->ptr(o + off::uobject::Class);
                if (NameOf(cls) != className) continue;
            }
            return {sdk_, o};
        }
        return {};
    }

    // ---- class structure -----------------------------------------------------
    UObject GetSuperClass(addr_t cls) const {
        return {sdk_, sdk_->ptr(cls + off::ustruct::SuperStruct)};
    }

    std::vector<FunctionInfo> EnumerateFunctions(addr_t cls) const {
        std::vector<FunctionInfo> out;
        if (!cls) return out;
        addr_t cur = sdk_->ptr(cls + off::ustruct::Children);
        for (int i = 0; i < 20000 && cur; ++i) {
            FunctionInfo fi;
            fi.Addr = cur;
            fi.Name = NameOf(cur);
            fi.Flags = sdk_->rd<u32>(cur + off::ufunction::FunctionFlags);
            fi.NativeFunc = sdk_->ptr(cur + off::ufunction::Func);
            if (!fi.Name.empty()) out.push_back(std::move(fi));
            cur = sdk_->ptr(cur + off::ufield::Next);
        }
        return out;
    }

    UObject FindFunction(addr_t cls, const std::string& func_name) const {
        if (!cls) return {};
        addr_t cur = sdk_->ptr(cls + off::ustruct::Children);
        for (int i = 0; i < 20000 && cur; ++i) {
            if (NameOf(cur) == func_name) return {sdk_, cur};
            cur = sdk_->ptr(cur + off::ufield::Next);
        }
        return {};
    }

    std::vector<PropertyInfo> EnumerateProperties(addr_t cls) const {
        std::vector<PropertyInfo> out;
        if (!cls) return out;
        addr_t cur = sdk_->ptr(cls + off::ustruct::ChildProperties);
        for (int i = 0; i < 20000 && cur; ++i) {
            PropertyInfo pi;
            pi.Addr = cur;
            pi.Name = sdk_->Name(sdk_->rd<u32>(cur + off::ffield::NamePrivate));  // FName
            pi.Offset = sdk_->rd<i32>(cur + off::fproperty::Offset_Internal);
            pi.Size = sdk_->rd<i32>(cur + off::fproperty::ElementSize);
            pi.Type = FieldTypeName(cur);
            if (!pi.Name.empty()) out.push_back(std::move(pi));
            cur = sdk_->ptr(cur + off::ffield::Next);
        }
        return out;
    }

    // Search property offset on cls and its superclasses.
    i32 GetPropertyOffset(addr_t cls, const std::string& prop) const {
        for (int depth = 0; depth < 64 && cls; ++depth) {
            for (const auto& pi : EnumerateProperties(cls))
                if (pi.Name == prop) return pi.Offset;
            cls = sdk_->ptr(cls + off::ustruct::SuperStruct);
        }
        return -1;
    }

    PropertyInfo GetProperty(addr_t cls, const std::string& prop) const {
        for (int depth = 0; depth < 64 && cls; ++depth) {
            for (const auto& pi : EnumerateProperties(cls))
                if (pi.Name == prop) return pi;
            cls = sdk_->ptr(cls + off::ustruct::SuperStruct);
        }
        return {};
    }

    bool IsA(addr_t obj, const std::string& className) const {
        addr_t cls = sdk_->ptr(obj + off::uobject::Class);
        for (int i = 0; i < 64 && cls; ++i) {
            if (NameOf(cls) == className) return true;
            cls = sdk_->ptr(cls + off::ustruct::SuperStruct);
        }
        return false;
    }

private:
    std::string NameOf(addr_t o) const {
        if (!o) return {};
        return sdk_->Name(sdk_->rd<u32>(o + off::uobject::Name));
    }

    std::string FieldTypeName(addr_t field) const {
        const addr_t fc = sdk_->ptr(field + off::ffield::ClassPrivate);
        if (!sdk_->mem->valid_ptr(fc)) return {};
        // FFieldClass begins with FName Name (validated by live probe).
        return sdk_->Name(sdk_->rd<u32>(fc));
    }

    Sdk* sdk_;
    std::unordered_map<std::string, addr_t> class_cache_;
};

}  // namespace aion
