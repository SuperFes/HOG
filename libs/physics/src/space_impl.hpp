#pragma once

// What the library's sources share of Jolt, kept out of its public headers.

#include <hog/core/math.hpp>
#include <hog/physics/space.hpp>

#include <Jolt/Jolt.h>

#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <cstddef>
#include <optional>

namespace hog::physics {

namespace layers {
inline constexpr JPH::ObjectLayer still = 0;
inline constexpr JPH::ObjectLayer moving = 1;
inline constexpr JPH::uint count = 2;
} // namespace layers

// Jolt's allocator, factory and shape types are process-wide and set up once.
void register_jolt();

[[nodiscard]] inline JPH::RVec3 to_jolt(WorldPosition position) noexcept {
    const DVec3 metres = position.metres();
    return {metres.x, metres.y, metres.z};
}

[[nodiscard]] inline JPH::Vec3 to_jolt(DVec3 offset) noexcept {
    const Vec3 single{offset};
    return {single.x, single.y, single.z};
}

[[nodiscard]] inline WorldPosition from_jolt(JPH::RVec3 position) noexcept {
    return {position.GetX(), position.GetY(), position.GetZ()};
}

[[nodiscard]] inline DVec3 from_jolt(JPH::Vec3 offset) noexcept {
    return DVec3{Vec3{offset.GetX(), offset.GetY(), offset.GetZ()}};
}

struct Space::Impl {
    // Comes first: Jolt's tables below allocate with its allocator.
    struct Registration {
        Registration() { register_jolt(); }
    };

    explicit Impl(const SpaceSettings& settings);

    Registration registration;
    JPH::ObjectLayerPairFilterTable pairs{layers::count};
    JPH::BroadPhaseLayerInterfaceTable broad{layers::count, layers::count};
    // Built from the two above once they're filled in.
    std::optional<JPH::ObjectVsBroadPhaseLayerFilterTable> object_vs_broad;
    JPH::PhysicsSystem system;
    // For moving characters, one at a time.
    JPH::TempAllocatorImpl scratch{std::size_t{1} << 20U};
    std::size_t size = 0;
};

} // namespace hog::physics
