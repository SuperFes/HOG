#pragma once

#include <hog/core/math.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/capsules.hpp>
#include <hog/replication/timeline.hpp>
#include <hog/terrain/bend.hpp>

#include <span>
#include <string_view>
#include <vector>

namespace hog::client {

// How far off others' names are still shown over them.
inline constexpr double tag_reach = 100.0; // metres

// Each player's own colour, told apart from those joining near them.
[[nodiscard]] Vec3 colour_of(protocol::NetId id) noexcept;

// Others as capsules the size of their characters, standing where they're shown.
[[nodiscard]] std::vector<render::Capsule> avatars(std::span<const replication::Shown> shown);

// A name shown over someone.
struct Tag {
    std::string_view name;
    DVec2 at;        // over the middle of the name, from the top left of the picture
    double distance; // metres from the eye
};

// Where the names of those shown go over a picture of `size` seen by `camera`, bent as drawn,
// furthest first so the nearest are drawn over them. None for those behind the eye, out of the
// picture or past tag_reach.
[[nodiscard]] std::vector<Tag> tags(std::span<const replication::Shown> shown,
                                    const render::Camera& camera, const terrain::Bend& bend,
                                    DVec2 size);

} // namespace hog::client
