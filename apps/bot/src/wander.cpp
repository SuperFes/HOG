#include <hog/bot/wander.hpp>
#include <hog/planet/tangent_frame.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace hog::bot {

Wander pick(DVec3 at, double planet_radius, std::mt19937_64& random, const WanderSettings& settings,
            std::optional<DVec3> held) {
    std::uniform_real_distribution<double> bearings{0.0, 2.0 * std::numbers::pi};
    std::uniform_real_distribution<double> reaches{settings.nearest, settings.furthest};
    const double bearing = bearings(random);
    const double angle = reaches(random) / planet_radius;
    const planet::TangentFrame frame = planet::tangent_frame(at);
    DVec3 way = (std::cos(bearing) * frame.east) + (std::sin(bearing) * frame.north);
    // Back the way it came or off to a side, rather than into whatever held it up.
    if (held && glm::dot(way, *held) > 0.0) {
        way = -way;
    }
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
    // Held up against a step or a wall, it gets no nearer however long it tries.
    if (along < wander.closest - settings.nearer) {
        wander.closest = along;
        wander.stalled = 0.0;
    } else {
        wander.stalled += seconds;
    }
    if (wander.stalled >= settings.stuck && length >= 1e-9) {
        return {.wander = wander, .heading = std::nullopt, .held = toward / length};
    }
    if (wander.patience <= 0.0 || along <= settings.near_enough || length < 1e-9) {
        return {.wander = wander, .heading = std::nullopt, .held = std::nullopt};
    }
    return {.wander = wander, .heading = toward / length, .held = std::nullopt};
}

} // namespace hog::bot
