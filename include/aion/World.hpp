// World.hpp — UWorld / ULevel graph traversal.
#pragma once
#include <vector>

#include "UObject.hpp"

namespace aion {

class World {
public:
    explicit World(Sdk* sdk) : sdk_(sdk) {}

    UObject Get() const { return {sdk_, sdk_->World()}; }

    UObject PersistentLevel() const {
        return {sdk_, sdk_->ptr(Get().Addr() + off::uworld::PersistentLevel)};
    }

    std::vector<addr_t> Levels() const {
        return sdk_->mem->read_tarray<addr_t>(Get().Addr() + off::uworld::Levels, 20000);
    }

    UObject OwningGameInstance() const {
        return {sdk_, sdk_->ptr(Get().Addr() + off::uworld::OwningGameInstance)};
    }

private:
    Sdk* sdk_;
};

class Level {
public:
    explicit Level(Sdk* sdk) : sdk_(sdk) {}

    std::vector<addr_t> Actors(addr_t level) const {
        return sdk_->mem->read_tarray<addr_t>(level + off::ulevel::Actors, 100000);
    }
    UObject WorldSettings(addr_t level) const {
        return {sdk_, sdk_->ptr(level + off::ulevel::WorldSettings)};
    }

private:
    Sdk* sdk_;
};

}  // namespace aion
