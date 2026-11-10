#include <hog/client/globe_view.hpp>
#include <hog/client/grains.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>

namespace hog::client {

namespace {

// The smallest and largest a lump is drawn, in metres across.
constexpr double least_radius = 0.04;
constexpr double most_radius = 0.8;

} // namespace

void GrainField::receive(const protocol::Grains& grains, MonotonicClock::time_point now) {
    // The server's ticks only go up, and a message from before the newest is stale.
    if (any_ && grains.tick <= tick_) {
        return;
    }
    any_ = true;
    tick_ = grains.tick;
    told_ = now;
    flying_ = grains.flying;
}

std::vector<render::Capsule> GrainField::capsules(MonotonicClock::time_point now, double thickness,
                                                  double gravity) const {
    const double carried = std::min(
        std::chrono::duration<double>(std::max(now - told_, MonotonicClock::duration::zero()))
            .count(),
        longest_carry);
    std::vector<render::Capsule> found;
    found.reserve(flying_.size());
    for (const protocol::Flying& one : flying_) {
        const DVec3 up = glm::normalize(protocol::to_world(one.at).metres());
        const DVec3 velocity{one.velocity[0] / 100.0, one.velocity[1] / 100.0,
                             one.velocity[2] / 100.0};
        const DVec3 at = protocol::to_world(one.at).metres() + (velocity * carried) -
                         (up * (0.5 * gravity * carried * carried));
        const double volume = static_cast<double>(one.amount) / globe::Voxel::full * thickness *
                              thickness * thickness;
        const double radius =
            std::clamp(std::cbrt(0.75 * volume / std::numbers::pi), least_radius, most_radius);
        const auto material = one.material < globe::material_count
                                  ? static_cast<globe::Material>(one.material)
                                  : globe::Material::Sand;
        found.push_back({.feet = WorldPosition{at - (up * radius)},
                         .up = glm::normalize(at),
                         .height = 2.0 * radius,
                         .radius = radius,
                         .colour = surface_colour(material)});
    }
    return found;
}

} // namespace hog::client
