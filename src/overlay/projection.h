#pragma once
// World -> screen projection that mirrors how Unreal builds its view (FMinimalViewInfo::CalculateProjectionMatrixGivenView):
//   * the camera POV gives location, rotation (pitch/yaw/roll, degrees) and FOV;
//   * ULocalPlayer::AspectRatioAxisConstraint decides whether FOV is horizontal (MaintainXFOV) or
//     vertical (MaintainYFOV); MajorAxisFOV follows the longer side;
//   * a constrained aspect ratio (cutscenes) shrinks the view rectangle and always uses the X axis.
#include "game/scene.h"
#include "imgui.h"

#include <cmath>

namespace overlay
{
class Projection
{
public:
    Projection(const scene::View& view, float width, float height)
    {
        if (!view.valid || width < 1.0f || height < 1.0f) return;

        constexpr float kDeg = 3.14159265358979f / 180.0f;
        const float sp = sinf(view.rotation.pitch * kDeg), cp = cosf(view.rotation.pitch * kDeg);
        const float sy = sinf(view.rotation.yaw * kDeg),   cy = cosf(view.rotation.yaw * kDeg);
        const float sr = sinf(view.rotation.roll * kDeg),  cr = cosf(view.rotation.roll * kDeg);
        forward_ = { cp * cy, cp * sy, sp };
        right_   = { sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp };
        up_      = { -(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp };
        origin_  = view.location;

        float rectW = width, rectH = height, offX = 0.0f, offY = 0.0f;
        bool useX = view.axisConstraint == 1 || (view.axisConstraint == 2 && width > height);
        if (view.constrainAspect && view.aspect > 0.1f)
        {
            if (view.aspect < width / height) { rectW = height * view.aspect; offX = (width - rectW) * 0.5f; } // pillarbox
            else                              { rectH = width / view.aspect;  offY = (height - rectH) * 0.5f; } // letterbox
            useX = true;
        }
        scale_ = (useX ? rectW : rectH) * 0.5f / tanf(view.fov * kDeg * 0.5f);
        centerX_ = offX + rectW * 0.5f;
        centerY_ = offY + rectH * 0.5f;
        valid_ = true;
    }

    bool valid() const { return valid_; }
    const ue::Vec3& origin() const { return origin_; }
    const ue::Vec3& right() const { return right_; }
    const ue::Vec3& up() const { return up_; }

    // Distance along the view direction; < 0 means behind the camera.
    float depth(const ue::Vec3& p) const { return Dot(Sub(p, origin_), forward_); }

    // False when the point is behind (or too close to) the camera.
    bool project(const ue::Vec3& p, ImVec2& out, float* depthOut = nullptr) const
    {
        if (!valid_) return false;
        const ue::Vec3 d = Sub(p, origin_);
        const float z = Dot(d, forward_);
        if (z < 1.0f) return false;
        out.x = centerX_ + Dot(d, right_) / z * scale_;
        out.y = centerY_ - Dot(d, up_) / z * scale_;
        if (depthOut) *depthOut = z;
        return true;
    }

    // Screen-space length of a world length `len` at view depth `z`.
    float pixelsAt(float len, float z) const { return len / z * scale_; }

    static float Distance(const ue::Vec3& a, const ue::Vec3& b)
    {
        const ue::Vec3 d = Sub(a, b);
        return sqrtf(Dot(d, d));
    }

private:
    static ue::Vec3 Sub(const ue::Vec3& a, const ue::Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    static float Dot(const ue::Vec3& a, const ue::Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

    ue::Vec3 origin_, forward_, right_, up_;
    float scale_ = 1.0f, centerX_ = 0.0f, centerY_ = 0.0f;
    bool valid_ = false;
};
}
