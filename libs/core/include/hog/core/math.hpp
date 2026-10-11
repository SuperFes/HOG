#pragma once

// GLM is configured for the whole project through hog_core's compile definitions (Vulkan depth
// range, explicit constructors, size_t lengths); include GLM through this header.
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace hog {

using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;
using DVec3 = glm::dvec3;
using IVec2 = glm::ivec2;
using IVec3 = glm::ivec3;
using Quat = glm::quat;
using DQuat = glm::dquat;
using Mat3 = glm::mat3;
using Mat4 = glm::mat4;

// A point in planet-centred space, in metres. Doubles keep sub-millimetre precision anywhere on
// an Earth-sized planet; floats lose that beyond a few kilometres, so anything sent to the GPU is
// first made relative to the camera with relative_to().
class WorldPosition {
public:
    WorldPosition() = default;
    explicit WorldPosition(DVec3 metres) noexcept : metres_(metres) {}
    WorldPosition(double x, double y, double z) noexcept : metres_(x, y, z) {}

    [[nodiscard]] DVec3 metres() const noexcept { return metres_; }

    WorldPosition& operator+=(DVec3 offset) noexcept {
        metres_ += offset;
        return *this;
    }
    WorldPosition& operator-=(DVec3 offset) noexcept {
        metres_ -= offset;
        return *this;
    }

    friend WorldPosition operator+(WorldPosition point, DVec3 offset) noexcept {
        return point += offset;
    }
    friend WorldPosition operator-(WorldPosition point, DVec3 offset) noexcept {
        return point -= offset;
    }
    friend DVec3 operator-(WorldPosition a, WorldPosition b) noexcept {
        return a.metres_ - b.metres_;
    }
    friend bool operator==(WorldPosition a, WorldPosition b) noexcept {
        return a.metres_ == b.metres_;
    }

private:
    DVec3 metres_{0.0};
};

// Offset of `point` from `origin` in single precision, for rendering and other float consumers.
[[nodiscard]] inline Vec3 relative_to(WorldPosition point, WorldPosition origin) noexcept {
    // Explicit GLM constructors rule out the braced return the check suggests.
    return Vec3(point - origin); // NOLINT(modernize-return-braced-init-list)
}

} // namespace hog
