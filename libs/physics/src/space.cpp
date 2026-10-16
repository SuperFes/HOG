#include "space_impl.hpp"

#include <hog/core/assert.hpp>
#include <hog/physics/space.hpp>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/RegisterTypes.h>
#include <glm/geometric.hpp>

#include <format>
#include <utility>

namespace hog::physics {

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

Space::Impl::Impl(const SpaceSettings& settings) {
    pairs.EnableCollision(layers::moving, layers::still);
    pairs.EnableCollision(layers::moving, layers::moving);
    broad.MapObjectToBroadPhaseLayer(layers::still, JPH::BroadPhaseLayer{layers::still});
    broad.MapObjectToBroadPhaseLayer(layers::moving, JPH::BroadPhaseLayer{layers::moving});
    object_vs_broad.emplace(broad, layers::count, pairs, layers::count);
    system.Init(settings.bodies, 0, settings.body_pairs, settings.contacts, broad, *object_vs_broad,
                pairs);
}

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

std::optional<Hit> Space::cast_ray(WorldPosition from, DVec3 along, Faces faces) const {
    const JPH::RRayCast ray{to_jolt(from), to_jolt(along)};
    // Unlike the simpler overload, these settings can pass through back faces.
    JPH::RayCastSettings settings;
    settings.SetBackFaceMode(faces == Faces::Both ? JPH::EBackFaceMode::CollideWithBackFaces
                                                  : JPH::EBackFaceMode::IgnoreBackFaces);
    JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> closest;
    impl_->system.GetNarrowPhaseQuery().CastRay(ray, settings, closest);
    if (!closest.HadHit()) {
        return std::nullopt;
    }
    const JPH::RayCastResult& result = closest.mHit;
    const JPH::RVec3 point = ray.GetPointOnRay(result.mFraction);
    const JPH::BodyLockRead lock{impl_->system.GetBodyLockInterface(), result.mBodyID};
    HOG_ASSERT(lock.Succeeded(), "a body hit is in the space");
    const DVec3 normal =
        from_jolt(lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point));
    return Hit{.body = BodyId{result.mBodyID.GetIndexAndSequenceNumber()},
               .at = from_jolt(point),
               .normal = normal,
               .back = glm::dot(normal, along) > 0.0};
}

} // namespace hog::physics
