#include <hog/client/player.hpp>
#include <hog/client/scene.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace hog::client {

namespace {

// Feet dropped from above the ground on to it, so they start where the ground's mesh is.
constexpr double drop = 50.0; // metres
// Nearer than this, the camera would be inside the character's head.
constexpr double closest = 0.3; // metres

} // namespace

Result<Player> Player::create(const planet::World& world, DVec3 direction,
                              const PlaySettings& settings) {
    const DVec3 up = glm::normalize(direction);
    const double radius = world.geology().planet_radius();
    const WorldPosition over{up *
                             (radius + ground_under(world, WorldPosition{up * radius}) + drop)};
    auto space = std::make_unique<physics::Space>();
    physics::Ground ground{*space, world};
    if (auto kept = ground.keep(std::array{over}); !kept) {
        return std::unexpected(kept.error());
    }
    const std::optional<physics::Hit> hit = space->cast_ray(over, up * (-2.0 * drop));
    const WorldPosition feet = hit ? hit->at : over - (up * drop);
    physics::Character character{*space, feet, settings.character};
    return Player{std::move(space), std::move(ground), std::move(character), settings};
}

Player::Player(std::unique_ptr<physics::Space> space, physics::Ground ground,
               physics::Character character, const PlaySettings& settings)
    : space_(std::move(space)), ground_(std::move(ground)), character_(std::move(character)),
      settings_(settings) {}

Result<> Player::play(const physics::Stride& stride, double seconds) {
    if (auto kept = ground_.keep(std::array{feet()}); !kept) {
        return kept;
    }
    character_.move(stride, seconds);
    return {};
}

void Player::place(WorldPosition feet) {
    character_.place(feet);
}

WorldPosition Player::shoulder() const {
    const WorldPosition at = feet();
    return at + (glm::normalize(at.metres()) * settings_.shoulder);
}

double Player::room(WorldPosition from, DVec3 back, double wanted) const {
    const DVec3 along = back * (wanted + settings_.clearance);
    const std::optional<physics::Hit> hit = space_->cast_ray(from, along, physics::Faces::Both);
    if (!hit) {
        return wanted;
    }
    return std::max(glm::length(hit->at - from) - settings_.clearance, closest);
}

physics::Stride stride(const Flight& aim, DVec3 move, bool running) {
    const DVec3 right = glm::cross(aim.heading, glm::normalize(aim.eye.metres()));
    return {.along = (right * move.x) + (aim.heading * move.z),
            .running = running,
            .jumping = move.y > 0.0};
}

} // namespace hog::client
