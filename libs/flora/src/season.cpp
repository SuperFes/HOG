#include <hog/flora/season.hpp>
#include <hog/flora/stand.hpp>

#include <algorithm>

namespace hog::flora {

namespace {

// Leaves colour through the first part of their fall, then drop through the rest.
constexpr float colouring = 0.6F;
constexpr float dropping = 0.4F;
// Of the way from the shortest day to the longest, how long new leaves take to come out.
constexpr float flushing = 0.1F;

float saturated(float value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}

Leaves autumn(const Leafing& leafing, float own, float hours) noexcept {
    const float dark = leafing.turn + (own * leafing.spread) - hours;
    return {.turned = saturated(dark / (colouring * leafing.fall)),
            .held =
                1.0F - saturated((dark - (dropping * leafing.fall)) / (colouring * leafing.fall))};
}

} // namespace

float own_season(std::uint32_t hash, float age) noexcept {
    const float dice = (2.0F * unit(hash + 12U)) - 1.0F;
    return std::clamp((0.7F * dice) + (0.6F * (age - 0.5F)), -1.0F, 1.0F);
}

Leaves leaves_at(const Leafing& leafing, float own, const Daylight& daylight) noexcept {
    if (leafing.turn <= 0.0F) {
        return {};
    }
    if (!daylight.lengthening) {
        return autumn(leafing, own, daylight.hours);
    }
    // Lengthening days start from however bare the shortest day left the tree.
    const float shortest = 24.0F - daylight.longest;
    const Leaves winter = autumn(leafing, own, shortest);
    const float range = daylight.longest - shortest;
    if (range <= 0.0F) {
        return winter;
    }
    const float into = (daylight.hours - shortest) / range;
    // Early turners come out late.
    const float out = saturated((into - leafing.flush - (0.05F * own)) / flushing);
    return {.turned = winter.turned * (1.0F - out), .held = std::max(winter.held, out)};
}

} // namespace hog::flora
