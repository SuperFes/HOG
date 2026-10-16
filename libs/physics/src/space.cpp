#include <hog/core/assert.hpp>
#include <hog/physics/space.hpp>

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <format>
#include <utility>

namespace hog::physics {

namespace {

namespace layers {
constexpr JPH::ObjectLayer still = 0;
constexpr JPH::ObjectLayer moving = 1;
constexpr JPH::uint count = 2;
} // namespace layers

// Jolt's allocator, factory and shape types are process-wide and set up once.
void register_jolt() {
    static const bool registered = [] {
        JPH::RegisterDefaultAllocator();
        static JPH::Factory factory;
        JPH::Factory::sInstance = &factory;
        JPH::RegisterTypes();
        return true;
    }();
    (void)registered;
}

JPH::RVec3 to_jolt(WorldPosition position) noexcept {
    const DVec3 metres = position.metres();
    return {metres.x, metres.y, metres.z};
}

JPH::Vec3 to_jolt(DVec3 offset) noexcept {
    const Vec3 single{offset};
    return {single.x, single.y, single.z};
}

WorldPosition from_jolt(JPH::RVec3 position) noexcept {
    return {position.GetX(), position.GetY(), position.GetZ()};
}

DVec3 from_jolt(JPH::Vec3 offset) noexcept {
    return DVec3{Vec3{offset.GetX(), offset.GetY(), offset.GetZ()}};
}

} // namespace

struct Space::Impl {
    // Comes first: Jolt's tables below allocate with its allocator.
    struct Registration {
        Registration() { register_jolt(); }
    };

    explicit Impl(const SpaceSettings& settings) {
        pairs.EnableCollision(layers::moving, layers::still);
        pairs.EnableCollision(layers::moving, layers::moving);
        broad.MapObjectToBroadPhaseLayer(layers::still, JPH::BroadPhaseLayer{layers::still});
        broad.MapObjectToBroadPhaseLayer(layers::moving, JPH::BroadPhaseLayer{layers::moving});
        object_vs_broad.emplace(broad, layers::count, pairs, layers::count);
        system.Init(settings.bodies, 0, settings.body_pairs, settings.contacts, broad,
                    *object_vs_broad, pairs);
    }

    Registration registration;
    JPH::ObjectLayerPairFilterTable pairs{layers::count};
    JPH::BroadPhaseLayerInterfaceTable broad{layers::count, layers::count};
    // Built from the two above once they're filled in.
    std::optional<JPH::ObjectVsBroadPhaseLayerFilterTable> object_vs_broad;
    JPH::PhysicsSystem system;
    std::size_t size = 0;
};

Space::Space(const SpaceSettings& settings) : impl_(std::make_unique<Impl>(settings)) {}

Space::~Space() = default;
Space::Space(Space&&) noexcept = default;
Space& Space::operator=(Space&&) noexcept = default;

Result<BodyId> Space::add(const Mesh& mesh) {
    if (mesh.triangles.empty()) {
        return make_error(ErrorCode::InvalidArgument, "a mesh needs triangles");
    }
    JPH::VertexList vertices;
    vertices.reserve(mesh.vertices.size());
    for (const Vec3 vertex : mesh.vertices) {
        vertices.push_back({vertex.x, vertex.y, vertex.z});
    }
    JPH::IndexedTriangleList triangles;
    triangles.reserve(mesh.triangles.size());
    for (const auto& triangle : mesh.triangles) {
        for (const std::uint32_t corner : triangle) {
            if (corner >= mesh.vertices.size()) {
                return make_error(
                    ErrorCode::InvalidArgument,
                    std::format("a triangle has vertex {} of {}", corner, mesh.vertices.size()));
            }
        }
        triangles.push_back({triangle[0], triangle[1], triangle[2]});
    }
    const JPH::MeshShapeSettings settings{std::move(vertices), std::move(triangles)};
    const JPH::ShapeSettings::ShapeResult shape = settings.Create();
    if (shape.HasError()) {
        return make_error(ErrorCode::InvalidArgument,
                          std::format("can't make a mesh: {}", shape.GetError()));
    }
    const JPH::BodyCreationSettings body{shape.Get(), to_jolt(mesh.origin), JPH::Quat::sIdentity(),
                                         JPH::EMotionType::Static, layers::still};
    const JPH::BodyID id =
        impl_->system.GetBodyInterface().CreateAndAddBody(body, JPH::EActivation::DontActivate);
    if (id.IsInvalid()) {
        return make_error(ErrorCode::OutOfRange, "no room for another body");
    }
    ++impl_->size;
    return BodyId{id.GetIndexAndSequenceNumber()};
}

void Space::remove(BodyId body) {
    const JPH::BodyID id{std::to_underlying(body)};
    JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
    HOG_ASSERT(bodies.IsAdded(id), "only a body in the space is removed");
    bodies.RemoveBody(id);
    bodies.DestroyBody(id);
    --impl_->size;
}

std::size_t Space::size() const noexcept {
    return impl_->size;
}

std::optional<Hit> Space::cast_ray(WorldPosition from, DVec3 along) const {
    const JPH::RRayCast ray{to_jolt(from), to_jolt(along)};
    // Unlike the simpler overload, these settings pass through back faces.
    JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> closest;
    impl_->system.GetNarrowPhaseQuery().CastRay(ray, JPH::RayCastSettings{}, closest);
    if (!closest.HadHit()) {
        return std::nullopt;
    }
    const JPH::RayCastResult& result = closest.mHit;
    const JPH::RVec3 point = ray.GetPointOnRay(result.mFraction);
    const JPH::BodyLockRead lock{impl_->system.GetBodyLockInterface(), result.mBodyID};
    HOG_ASSERT(lock.Succeeded(), "a body hit is in the space");
    return Hit{
        .body = BodyId{result.mBodyID.GetIndexAndSequenceNumber()},
        .at = from_jolt(point),
        .normal = from_jolt(lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point))};
}

} // namespace hog::physics
