// Projection.hpp — UE world-to-screen (LWC doubles, UE rotator convention).
#pragma once
#include <cmath>

#include "Sdk.hpp"

namespace aion {

class Projection {
public:
    void SetViewport(int w, int h) {
        width_ = w > 1 ? w : 1;
        height_ = h > 1 ? h : 1;
    }
    int Width() const { return width_; }
    int Height() const { return height_; }

    // Returns false when the point is behind the camera.
    bool WorldToScreen(const FVector& world, const FRotator& cam_rot, const FVector& cam_loc,
                       float fov_deg, FVector2D& out) const {
        const double d2r = 3.14159265358979323846 / 180.0;
        const double sp = std::sin(cam_rot.Pitch * d2r), cp = std::cos(cam_rot.Pitch * d2r);
        const double sy = std::sin(cam_rot.Yaw * d2r),   cy = std::cos(cam_rot.Yaw * d2r);
        const double sr = std::sin(cam_rot.Roll * d2r),  cr = std::cos(cam_rot.Roll * d2r);

        // UE rotation matrix rows: forward, right, up.
        const double fwd[3]   = {cp * cy, cp * sy, sp};
        const double right[3] = {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp};
        const double up[3]    = {-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp};

        const double dx = world.X - cam_loc.X;
        const double dy = world.Y - cam_loc.Y;
        const double dz = world.Z - cam_loc.Z;

        const double x = dx * fwd[0] + dy * fwd[1] + dz * fwd[2];      // depth
        const double y = dx * right[0] + dy * right[1] + dz * right[2];
        const double z = dx * up[0] + dy * up[1] + dz * up[2];
        if (x <= 1e-4) return false;

        const double half_tan = std::tan(fov_deg * d2r * 0.5);
        const double aspect = static_cast<double>(width_) / height_;
        // UE's FOV is the wider axis: horizontal while the viewport is at least as wide as it is
        // tall, vertical when it is not (a narrow windowed client, or a client on a portrait
        // display). Reading it as horizontal unconditionally squeezed the vertical extent and
        // spread the horizontal one on a portrait frame.
        const double half_x = aspect >= 1.0 ? half_tan : half_tan * aspect;
        const double half_y = aspect >= 1.0 ? half_tan / aspect : half_tan;

        const double ndc_x = (y / x) / half_x;
        const double ndc_y = (z / x) / half_y;

        out.X = static_cast<float>((width_ * 0.5) * (1.0 + ndc_x));
        out.Y = static_cast<float>((height_ * 0.5) * (1.0 - ndc_y));
        return true;
    }

private:
    int width_ = 1920;
    int height_ = 1080;
};

}  // namespace aion
