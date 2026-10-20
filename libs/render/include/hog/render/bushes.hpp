#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/render/anchors.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/grass.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/vulkan.hpp>

#include <span>

namespace hog::render {

class Device;

// Metres from the eye bushes have shrunk away by.
inline constexpr double bushes_far = 260.0;

// Leaves a touch darker and greener than the ground's mean, so bushes sit in their biome.
[[nodiscard]] constexpr Vec3 leaf_colour(Vec3 ground) noexcept {
    return (ground * 0.55F) + Vec3{0.02F, 0.065F, 0.02F};
}

// Bushes drawn from their anchors: blobs, rosettes, twiggy skeletons and low mats, swaying
// slightly with the wind, shrinking away toward the edge of their reach.
class Bushes {
public:
    [[nodiscard]] static Result<Bushes> create(Device& device, VkFormat colour_format,
                                               VkFormat depth_format, double fade_far = bushes_far);

    // Records drawing into the colour and depth images being rendered to, of `extent`. The draws'
    // slots must hold bush anchors already uploaded.
    void draw(VkCommandBuffer commands, VkExtent2D extent, const AnchorPool& pool,
              std::span<const StandDraw> draws, const GrassView& view) const;

private:
    Bushes() = default;

    Pipeline pipeline_;
    Buffer settings_;
};

} // namespace hog::render
