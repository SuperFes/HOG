#include <hog/client/others.hpp>
#include <hog/physics/character.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace hog::client {

namespace {

// Hues a golden turn apart, so those joining one after another differ most.
constexpr double golden_turn = 0.6180339887498949;
constexpr double saturation = 0.6;
constexpr double value = 0.85;
// Over the head, so the name doesn't cover the face.
constexpr double above_head = 0.3; // metres

} // namespace

Vec3 colour_of(protocol::NetId id) noexcept {
    const double turn = std::fmod(static_cast<double>(std::to_underlying(id)) * golden_turn, 1.0);
    // Each channel from the hue, saturation and value, made linear.
    const auto channel = [turn](double offset) {
        const double k = std::fmod(offset + (turn * 6.0), 6.0);
        const double dimmed = std::clamp(std::min(k, 4.0 - k), 0.0, 1.0);
        return static_cast<float>(std::pow(value * (1.0 - (saturation * dimmed)), 2.2));
    };
    return {channel(5.0), channel(3.0), channel(1.0)};
}

std::vector<render::Capsule> avatars(std::span<const replication::Shown> shown) {
    const physics::CharacterSettings character;
    std::vector<render::Capsule> capsules;
    capsules.reserve(shown.size());
    for (const replication::Shown& other : shown) {
        capsules.push_back({.feet = other.at,
                            .up = glm::normalize(other.at.metres()),
                            .height = character.height,
                            .radius = character.radius,
                            .colour = colour_of(other.id)});
    }
    return capsules;
}

std::vector<Tag> tags(std::span<const replication::Shown> shown, const render::Camera& camera,
                      const terrain::Bend& bend, DVec2 size) {
    const double height = physics::CharacterSettings{}.height + above_head;
    const glm::dmat4 clip_from_eye{render::view_projection(camera, size.x / size.y)};
    std::vector<Tag> found;
    for (const replication::Shown& other : shown) {
        const DVec3 over = (other.at - camera.eye) + (glm::normalize(other.at.metres()) * height);
        const double distance = glm::length(over);
        if (distance > tag_reach) {
            continue;
        }
        const glm::dvec4 clip = clip_from_eye * glm::dvec4{terrain::bent(bend, over), 1.0};
        if (clip.w <= 0.0) {
            continue;
        }
        const DVec2 drawn = ((DVec2{clip.x, clip.y} / clip.w) + 1.0) / 2.0;
        if (drawn.x < 0.0 || drawn.x > 1.0 || drawn.y < 0.0 || drawn.y > 1.0) {
            continue;
        }
        found.push_back({.name = other.name, .at = drawn * size, .distance = distance});
    }
    std::ranges::sort(found, std::ranges::greater{}, &Tag::distance);
    return found;
}

} // namespace hog::client
