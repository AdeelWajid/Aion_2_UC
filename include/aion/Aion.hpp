// Aion.hpp — AION2-specific systems: manager graph, UI widgets, skills/quickslots and input.
//
// What is here is the manager graph and the reflection helpers; there is no telegraph or
// block-window code, and the input side is two supplied, still-unvalidated constants.
//
// Offsets here are three kinds:
//   * reflection-validated manager offsets on AionGameInstance (confirmed live),
//   * supplied constants marked UNVALIDATED where the class exposes no reflected
//     properties (so the value could not be confirmed and MUST be probed at runtime),
//   * name-resolved fields read through Reflection (no hard-coded offset).
#pragma once
#include <string>
#include <vector>

#include "Reflection.hpp"
#include "Sdk.hpp"
#include "UObject.hpp"
#include "World.hpp"

namespace aion {

// ------------------------------------------------------------------ managers --
// AionGameInstance reflected properties (harvested live via Reflection).
namespace aion_gi {
constexpr std::uint32_t PakGroupList            = 0x360;  // VALIDATED
constexpr std::uint32_t UISystem                = 0x3C8;  // VALIDATED
constexpr std::uint32_t WorldManager            = 0x3D0;  // VALIDATED
constexpr std::uint32_t MemoryManager           = 0x3D8;  // VALIDATED
constexpr std::uint32_t GCManager               = 0x3E0;  // VALIDATED
constexpr std::uint32_t NetworkManager          = 0x3E8;  // VALIDATED
constexpr std::uint32_t SoundManager            = 0x3F0;  // VALIDATED
constexpr std::uint32_t DevPacketManager        = 0x3F8;  // VALIDATED
constexpr std::uint32_t CameraManager           = 0x400;  // VALIDATED
constexpr std::uint32_t LobbyManager            = 0x408;  // VALIDATED
constexpr std::uint32_t GameFlowSystem          = 0x410;  // VALIDATED
constexpr std::uint32_t InputSystem             = 0x418;  // VALIDATED
constexpr std::uint32_t DevHelper               = 0x420;  // VALIDATED
constexpr std::uint32_t UserSession             = 0x428;  // VALIDATED
constexpr std::uint32_t CharSession             = 0x430;  // VALIDATED
constexpr std::uint32_t SettingManager          = 0x470;  // VALIDATED
constexpr std::uint32_t ChatSystem              = 0x4B8;  // VALIDATED
constexpr std::uint32_t UIResources             = 0x4D8;  // VALIDATED
}  // namespace aion_gi

namespace aion_uisystem {
// Validated by live scan of the AionUISystem instance (2026-10-06) and re-measured 2026-10-10 by
// reflection (--dialog-map, build/self_hpmp_final.md): +0x060 is not a UObject pointer. It is the
// `LoadedDialogMap` the SDK dump declares there - a TMap<DialogKey, AionDialog*> of 0x50 bytes. Read
// as an object it hands back the map's own array pointer, so it has to be walked as a container.
constexpr std::uint32_t LoadedDialogMap         = 0x060;  // VALIDATED (TMap, not an object)
constexpr std::uint32_t ScreenMessageManager    = 0x380;  // VALIDATED
constexpr std::uint32_t SystemMessageManager    = 0x388;  // VALIDATED
// Supplied 0x680 does NOT point at a WidgetTree in this build (no such pointer in
// the object); treat as INVALID and resolve the widget tree per-UI-object instead.
constexpr std::uint32_t WidgetTree_Supplied     = 0x680;  // INVALID
}  // namespace aion_uisystem

namespace aion_inputsystem {
constexpr std::uint32_t MappingContext = 0x198;  // UNVALIDATED (supplied)
}  // namespace aion_inputsystem

namespace aion_playerinput {
constexpr std::uint32_t Key1_Mask = 0x02;  // UNVALIDATED (supplied)
}  // namespace aion_playerinput

// --------------------------------------------------------------- skill data --
struct SkillData {
    addr_t SlotAddr = 0;
    int SlotId = -1;
    int SkillId = -1;
    std::string Name;
    int Level = -1;
    float Cooldown = -1.f;
    float CooldownRemaining = -1.f;
    std::string Hotkey;
    std::string Icon;
    bool Casting = false;
    addr_t Target = 0;
    bool Valid = false;
};

// A widget/component field resolved by name through Reflection (no hard-coded offset).
struct NamedField {
    std::string Name;
    std::string Type;
    i32 Offset = -1;
    i32 Size = 0;
    bool resolved = false;
};

class AionSystems {
public:
    AionSystems(Sdk* sdk, Reflection* refl) : sdk_(sdk), refl_(refl) {}

    UObject GameInstance() const {
        World w(sdk_);
        return w.OwningGameInstance();
    }

    // Manager access; returns null object if the game instance is not ready.
    UObject UISystem() const {
        return ObjAt(GameInstance().Addr(), aion_gi::UISystem);
    }
    UObject InputSystem() const {
        return ObjAt(GameInstance().Addr(), aion_gi::InputSystem);
    }
    UObject WorldManager() const { return ObjAt(GameInstance().Addr(), aion_gi::WorldManager); }
    UObject GameFlowSystem() const {
        return ObjAt(GameInstance().Addr(), aion_gi::GameFlowSystem);
    }
    UObject SettingManager() const {
        return ObjAt(GameInstance().Addr(), aion_gi::SettingManager);
    }
    UObject CharSession() const { return ObjAt(GameInstance().Addr(), aion_gi::CharSession); }

    // Local player input object (APlayerController::PlayerInput).
    UObject PlayerInput(addr_t player_controller) const {
        return {sdk_, sdk_->ptr(player_controller + off::aplayercontroller::PlayerInput)};
    }

    // AionUISystem validated sub-objects.
    // The TMap itself, by address - it is not a UObject. Its values are AionDialog instances keyed
    // by DialogKey (established by reflection over the map property's own Key/Value properties; see
    // build/self_hpmp_final.md). It holds dialogs, so it is not an HP/MP route.
    addr_t LoadedDialogMapAddr() const {
        return UISystem().Addr() ? UISystem().Addr() + aion_uisystem::LoadedDialogMap : 0;
    }
    UObject ScreenMessageManager() const {
        return ObjAt(UISystem().Addr(), aion_uisystem::ScreenMessageManager);
    }
    UObject SystemMessageManager() const {
        return ObjAt(UISystem().Addr(), aion_uisystem::SystemMessageManager);
    }

    // --- reflection-driven field access -------------------------------------
    // Find a live object whose class chain includes className (first match).
    UObject FindLiveByClass(const std::string& className) const {
        UObject cls = const_cast<Reflection*>(refl_)->FindClass(className);
        if (!cls.Addr()) return {};
        for (i32 i = 0; i < sdk_->objects.Num(); ++i) {
            const addr_t o = sdk_->objects.ObjectAt(i);
            if (!o) continue;
            const addr_t c = sdk_->ptr(o + off::uobject::Class);
            if (c == cls.Addr()) return {sdk_, o};
        }
        return {};
    }

    // Resolve a named UMG field on a widget class (reflection), then read it.
    NamedField ResolveField(const std::string& className, const std::string& field) const {
        NamedField nf;
        nf.Name = field;
        UObject cls = const_cast<Reflection*>(refl_)->FindClass(className);
        if (!cls.Addr()) return nf;
        PropertyInfo pi = const_cast<Reflection*>(refl_)->GetProperty(cls.Addr(), field);
        if (pi.Offset < 0) return nf;
        nf.Offset = pi.Offset;
        nf.Size = pi.Size;
        nf.Type = pi.Type;
        nf.resolved = true;
        return nf;
    }

    // Read a UTextBlock-style widget's FString text.
    std::string ReadWidgetText(addr_t widget, const std::string& className,
                               const std::string& field) const {
        NamedField nf = ResolveField(className, field);
        if (!nf.resolved || !widget) return {};
        return sdk_->mem->read_fstring(widget + nf.Offset);
    }

    // Collect the object-pointer fields whose name starts with `slotFieldPrefix` from a QuickSlot
    // container widget, and return their addresses. Only SlotAddr and Valid are filled in: the rest
    // of SkillData (SkillId, Name, Cooldown, Hotkey, Icon, ...) is not read anywhere, so this is a
    // slot enumerator and not the working skill reader its callers would expect. No caller exists
    // yet - the live slot widgets are the HUD_* blueprint subclasses, not this native class, and the
    // fields a UI reader needs (SlotId_Set, P_HotKey, P_Content) are on those subclasses.
    std::vector<SkillData> ReadQuickSlots(addr_t container, const std::string& containerClass,
                                          const std::string& slotFieldPrefix) const {
        std::vector<SkillData> out;
        if (!container) return out;
        UObject cls = const_cast<Reflection*>(refl_)->FindClass(containerClass);
        if (!cls.Addr()) return out;
        const auto props = const_cast<Reflection*>(refl_)->EnumerateProperties(cls.Addr());
        for (const auto& p : props) {
            if (p.Name.rfind(slotFieldPrefix, 0) != 0) continue;  // prefix match
            const addr_t slot = sdk_->ptr(container + p.Offset);
            if (!sdk_->mem->valid_ptr(slot)) continue;
            SkillData sd;
            sd.SlotAddr = slot;
            sd.Valid = true;
            out.push_back(std::move(sd));
        }
        return out;
    }

private:
    UObject ObjAt(addr_t base, std::uint32_t off) const {
        if (!base) return {};
        return {sdk_, sdk_->ptr(base + off)};
    }
    Sdk* sdk_;
    Reflection* refl_;
};

}  // namespace aion
