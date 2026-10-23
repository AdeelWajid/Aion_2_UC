// Actors.hpp — AActor helpers (position, rotation, mesh, identity).
#pragma once
#include "Sdk.hpp"
#include "UObject.hpp"

namespace aion {

class Actor {
public:
    Actor() = default;
    Actor(Sdk* sdk, addr_t addr) : sdk_(sdk), addr_(addr) {}
    explicit operator bool() const { return addr_ != 0; }
    addr_t Addr() const { return addr_; }

    UObject Object() const { return {sdk_, addr_}; }
    std::string Name() const { return Object().GetName(); }
    std::string ClassName() const { return Object().GetClassName(); }

    addr_t RootComponent() const { return sdk_->ptr(addr_ + off::aactor::RootComponent); }

    // Root components have no parent, so RelativeLocation/Rotation are world-space.
    FVector Location() const {
        const addr_t root = RootComponent();
        return root ? sdk_->ReadVector(root + off::uscene_component::RelativeLocation) : FVector{};
    }
    FRotator Rotation() const {
        const addr_t root = RootComponent();
        return root ? sdk_->ReadRotator(root + off::uscene_component::RelativeRotation) : FRotator{};
    }

    // ACharacter attachments when applicable.
    addr_t Mesh() const { return sdk_->ptr(addr_ + off::acharacter::Mesh); }
    addr_t CharacterMovement() const { return sdk_->ptr(addr_ + off::acharacter::CharacterMovement); }
    addr_t CapsuleComponent() const { return sdk_->ptr(addr_ + off::acharacter::CapsuleComponent); }

    // The client's own "do not render this actor" flag (AActor::bHidden, bit 7 of +0x078). The game
    // sets it on everything outside its relevancy set, which is where monsters that have not
    // appeared yet live: a real actor, with a real mesh and a real position, that the game does not
    // draw. Measured live, the cut sits at roughly 23-25 m from the player, far inside the ESP's
    // own range. A failed read is UNKNOWN, not hidden - only a confirmed 1 suppresses anything.
    bool ClientHidden() const {
        const auto b = sdk_->mem->try_read<u8>(addr_ + off::aactor::HiddenFlags);
        return b && ((*b & off::aactor::HiddenBit) != 0);
    }

    double DistanceTo(const FVector& p) const { return (Location() - p).Length(); }

private:
    Sdk* sdk_ = nullptr;
    addr_t addr_ = 0;
};

}  // namespace aion
