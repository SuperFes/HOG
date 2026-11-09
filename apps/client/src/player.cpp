#include <hog/client/player.hpp>
#include <hog/client/scene.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/grip.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <utility>

namespace hog::client {

namespace {

// Feet dropped from above the ground on to it, so they start where the ground's mesh is.
constexpr double drop = 50.0; // metres
// Nearer than this, the camera would be inside the character's head.
constexpr double closest = 0.3; // metres
// The ground's grip is looked up again after this far, or this many steps, whichever is first.
constexpr double regrip_distance = 0.5; // metres
constexpr int regrip_steps = 30;

// How steep a slope holds where the character stands, from the world's ground and what lies on
// it; `steepest` where the ground isn't known, and never more than it.
class Footing {
public:
    Footing(globe::World world, double steepest) : world_(world), steepest_(steepest) {}

    double operator()(WorldPosition feet) {
        const DVec3 here = feet.metres();
        if (steps_++ >= regrip_steps || glm::distance(here, looked_) > regrip_distance) {
            steps_ = 0;
            looked_ = here;
            held_ = std::min(steepest_, globe::grip_at(world_, here).value_or(steepest_));
        }
        return held_;
    }

private:
    globe::World world_;
    double steepest_;
    DVec3 looked_{0.0};
    double held_ = 0.0;
    int steps_ = regrip_steps;
};

} // namespace

Result<Player> Player::create(const globe::World& world, DVec3 direction,
                              const PlaySettings& settings) {
    const DVec3 up = glm::normalize(direction);
    const double under = globe::top_of(world, up).value_or(world.planet().radius);
    const WorldPosition over{up * (under + drop)};
    auto space = std::make_unique<physics::Space>();
    physics::Ground ground{*space, world};
    if (auto kept = ground.keep(std::array{over}); !kept) {
        return std::unexpected(kept.error());
    }
    const std::optional<physics::Hit> hit = space->cast_ray(over, up * (-2.0 * drop));
    const WorldPosition feet = hit ? hit->at : over - (up * drop);
    physics::Character character{*space, feet, settings.character};
    character.set_grip(Footing{world, settings.character.steepest});
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

Result<Placing> placing(const globe::World& world, const protocol::Placed& placed,
                        const PlaySettings& settings) {
    Placing placing{.flight = faced({.eye = protocol::to_world(placed.at)}, placed.facing),
                    .player = std::nullopt};
    if (placed.stance == protocol::Stance::Walking) {
        auto player = Player::create(world, glm::normalize(placing.flight.eye.metres()), settings);
        if (!player) {
            return std::unexpected(player.error());
        }
        placing.player.emplace(std::move(player).value());
    }
    return placing;
}

bool ground_settled(const globe::World& world, const globe::HeldTiles& tiles, DVec3 at) {
    const globe::Layers& layers = world.layers();
    const DVec3 up = glm::normalize(at);
    const std::optional<double> top = globe::top_of(world, up);
    if (!top) {
        return false;
    }
    constexpr auto side = static_cast<std::uint32_t>(globe::Volume::tile_side);
    const globe::Cell cell = globe::cell_under(up, layers.level);
    const auto layer = static_cast<std::uint32_t>(std::max(layers.layer_at(*top) - 1, 0));
    const globe::TileKey under{
        .level = 0, .face = cell.face, .i = cell.i / side, .j = cell.j / side, .k = layer / side};
    const auto settled = [&](globe::TileKey key, bool air_whole) {
        const std::optional<globe::HeldTile> held = tiles.finest_over(key);
        if (!held) {
            return false;
        }
        const globe::VoxelTree& tree = *held->tree;
        return held->key.level == 0 ||
               (air_whole && tree.leaf_side(0, 0, 0) == globe::Volume::tile_side &&
                tree.at(0, 0, 0) == globe::Cube{});
    };
    globe::TileKey over = under;
    ++over.k;
    return settled(under, false) &&
           (std::cmp_greater_equal(over.k * side, layers.count) || settled(over, true));
}

physics::Stride stride(const Flight& aim, DVec3 move, bool running) {
    const DVec3 right = glm::cross(aim.heading, glm::normalize(aim.eye.metres()));
    return {.along = (right * move.x) + (aim.heading * move.z),
            .running = running,
            .jumping = move.y > 0.0};
}

} // namespace hog::client
