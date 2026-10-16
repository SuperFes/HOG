#include "space_impl.hpp"

#include <hog/physics/character.hpp>

#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <glm/geometric.hpp>

#include <algorithm>

namespace hog::physics {

struct Character::Impl {
    Space::Impl* space = nullptr;
    CharacterSettings settings;
    JPH::Ref<JPH::CharacterVirtual> body;
    double banked = 0.0; // seconds not yet stepped through
    // How it actually moved over the last step; Jolt's own velocity still holds the step's pull
    // into the ground.
    DVec3 moving{0.0};
};

namespace {

// Turns the character's own up, Jolt's +y, to point away from the centre.
JPH::Quat upright(DVec3 up) noexcept {
    return JPH::Quat::sFromTo(JPH::Vec3::sAxisY(), to_jolt(up));
}

DVec3 up_at(JPH::RVec3 position) noexcept {
    return glm::normalize(from_jolt(position).metres());
}

} // namespace

Character::Character(Space& space, WorldPosition feet, const CharacterSettings& settings)
    : impl_(std::make_unique<Impl>()) {
    impl_->space = space.impl_.get();
    impl_->settings = settings;
    const auto half = static_cast<float>(std::max(0.0, (settings.height / 2.0) - settings.radius));
    const DVec3 up = glm::normalize(feet.metres());

    const JPH::Ref<JPH::CharacterVirtualSettings> shaped = new JPH::CharacterVirtualSettings;
    const JPH::Ref<JPH::Shape> capsule =
        new JPH::CapsuleShape(half, static_cast<float>(settings.radius));
    shaped->mShape = capsule;
    // From the feet to the capsule's middle.
    shaped->mShapeOffset = JPH::Vec3{0.0F, static_cast<float>(settings.height / 2.0), 0.0F};
    shaped->mMaxSlopeAngle = static_cast<float>(settings.steepest);
    shaped->mUp = to_jolt(up);
    // Only the bottom of the capsule's lower sphere stands on things.
    shaped->mSupportingVolume =
        JPH::Plane{JPH::Vec3::sAxisY(), -static_cast<float>(settings.radius)};
    const JPH::Ref<JPH::CharacterVirtual> body =
        new JPH::CharacterVirtual(shaped, to_jolt(feet), upright(up), &impl_->space->system);
    impl_->body = body;
}

Character::~Character() = default;
Character::Character(Character&&) noexcept = default;
Character& Character::operator=(Character&&) noexcept = default;

void Character::move(const Stride& stride, double seconds) {
    impl_->banked += seconds;
    JPH::CharacterVirtual& body = *impl_->body;
    const CharacterSettings& settings = impl_->settings;
    const JPH::PhysicsSystem& system = impl_->space->system;
    // Allows for the rounding in adding up frames, which would otherwise drop a step now and then.
    while (impl_->banked >= step_seconds - 1e-9) {
        impl_->banked -= step_seconds;
        const DVec3 up = up_at(body.GetPosition());
        body.SetUp(to_jolt(up));
        body.SetRotation(upright(up));

        DVec3 along = stride.along - (up * glm::dot(stride.along, up));
        if (const double length = glm::length(along); length > 1.0) {
            along /= length;
        }
        const double speed = stride.running ? settings.running : settings.walking;

        const DVec3 was = from_jolt(body.GetLinearVelocity());
        const DVec3 ground = from_jolt(body.GetGroundVelocity());
        const bool on_ground =
            body.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround &&
            glm::dot(was - ground, up) < 0.1;
        DVec3 velocity = on_ground ? ground : up * glm::dot(was, up);
        if (on_ground && stride.jumping) {
            velocity += up * settings.jumping;
        }
        velocity += (along * speed) - (up * settings.gravity * step_seconds);
        body.SetLinearVelocity(to_jolt(velocity));

        JPH::CharacterVirtual::ExtendedUpdateSettings update;
        update.mStickToFloorStepDown = to_jolt(up * -0.5);
        update.mWalkStairsStepUp = to_jolt(up * settings.step);
        const JPH::RVec3 from = body.GetPosition();
        body.ExtendedUpdate(static_cast<float>(step_seconds), to_jolt(up * -settings.gravity),
                            update, system.GetDefaultBroadPhaseLayerFilter(layers::moving),
                            system.GetDefaultLayerFilter(layers::moving), {}, {},
                            impl_->space->scratch);
        impl_->moving = from_jolt(body.GetPosition()) - from_jolt(from);
        impl_->moving /= step_seconds;
    }
}

void Character::place(WorldPosition feet) {
    const DVec3 up = glm::normalize(feet.metres());
    impl_->body->SetPosition(to_jolt(feet));
    impl_->body->SetRotation(upright(up));
    impl_->body->SetUp(to_jolt(up));
    impl_->body->SetLinearVelocity(JPH::Vec3::sZero());
    impl_->banked = 0.0;
    impl_->moving = DVec3{0.0};
}

WorldPosition Character::feet() const {
    return from_jolt(impl_->body->GetPosition());
}

DVec3 Character::velocity() const {
    return impl_->moving;
}

bool Character::standing() const {
    return impl_->body->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
}

} // namespace hog::physics
