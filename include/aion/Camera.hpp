// Camera.hpp — PlayerCameraManager -> FCameraCacheEntry -> FMinimalViewInfo extraction.
#pragma once
#include "Sdk.hpp"
#include "UObject.hpp"

namespace aion {

struct Camera {
    FVector Location;
    FRotator Rotation;
    float FOV = 90.f;         // current (POV) horizontal FOV, degrees
    float DesiredFOV = 90.f;
    float TimeStamp = 0.f;
    bool valid = false;
};

class CameraExtractor {
public:
    explicit CameraExtractor(Sdk* sdk) : sdk_(sdk) {}

    UObject CameraManager(addr_t player_controller) const {
        return {sdk_, sdk_->ptr(player_controller + off::aplayercontroller::PlayerCameraManager)};
    }

    Camera FromCameraManager(addr_t mgr) const {
        Camera c;
        if (!mgr) return c;
        const addr_t entry = mgr + off::aplayer_camera_manager::CameraCachePrivate +
                             off::fcamera_cache_entry::POV;
        c.TimeStamp = sdk_->mem->f32_at(mgr + off::aplayer_camera_manager::CameraCachePrivate);
        c.Location = sdk_->ReadVector(entry + off::fminimal_view_info::Location);
        c.Rotation = sdk_->ReadRotator(entry + off::fminimal_view_info::Rotation);
        c.FOV = sdk_->mem->f32_at(entry + off::fminimal_view_info::FOV);
        c.DesiredFOV = sdk_->mem->f32_at(entry + off::fminimal_view_info::DesiredFOV);
        if (!(c.FOV > 1.f && c.FOV < 179.f))
            c.FOV = sdk_->mem->f32_at(mgr + off::aplayer_camera_manager::DefaultFOV);
        if (!(c.FOV > 1.f && c.FOV < 179.f)) c.FOV = 90.f;
        c.valid = c.Location.Length() > 0.0;
        return c;
    }

    Camera FromPlayerController(addr_t pc) const {
        return FromCameraManager(CameraManager(pc).Addr());
    }

private:
    Sdk* sdk_;
};

}  // namespace aion
