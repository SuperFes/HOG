#include <hog/bot/wander.hpp>
#include <hog/planet/tangent_frame.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace hog::bot {

Wander pick(DVec3 at, double planet_radius, std::mt19937_64& random,
            const WanderSettings& settings) {
    std::uniform_real_distribution<double> bearings{0.0, 2.0 * std::numbers::pi};
    std::uniform_real_distribution<double> reaches{settings.nearest, settings.furthest};
    const double bearing = bearings(random);
    const double angle = reaches(random) / planet_radius;
    const planet::TangentFrame frame = planet::tangent_frame(at);
    const DVec3 way = (std::cos(bearing) * frame.east) + (std::sin(bearing) * frame.north);
    return {.target = glm::normalize((std::cos(angle) * at) + (std::sin(angle) * way)),
            .patience = settings.patience};
}

Walked walk(Wander wander, WorldPosition feet, double planet_radius, double seconds,
            const WanderSettings& settings) {
    wander.patience -= seconds;
    const DVec3 at = glm::normalize(feet.metres());
    const double along =
        planet_radius * std::acos(std::clamp(glm::dot(at, wander.target), -1.0, 1.0));
    const DVec3 toward = wander.target - (glm::dot(wander.target, at) * at);
    const double length = glm::length(toward);
    if (wander.patience <= 0.0 || along <= settings.near_enough || length < 1e-9) {
        return {.wander = wander, .heading = std::nullopt};
    }
    return {.wander = wander, .heading = toward / length};
}

} // namespace hog::bot
