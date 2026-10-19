#pragma once

#include <hog/core/result.hpp>
#include <hog/render/anchors.hpp>
#include <hog/render/grass.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/vulkan.hpp>

#include <span>

namespace hog::render {

class Device;

// Metres from the eye trees have shrunk away by.
inline constexpr double trees_far = 900.0;

// Pine stamps drawn from anchored trees: crossed quads cut to a tiered conical crown over a bare
// trunk, their tops swaying stiffly in the wind. They hold off where the grown forest covers and
// shrink away at the edge of their own reach.
class Trees {
public:
    // Stamps keep off trees nearer than `hold_off`, the grown forest's ground, and shrink away by
    // `fade_far` metres.
    [[nodiscard]] static Result<Trees> create(Device& device, VkFormat colour_format,
                                              VkFormat depth_format, double hold_off,
                                              double fade_far = trees_far);

    // Records drawing into the colour and depth images being rendered to, of `extent`. The draws'
    // slots must hold anchors already uploaded.
    void draw(VkCommandBuffer commands, VkExtent2D extent, const AnchorPool& pool,
              std::span<const StandDraw> draws, const GrassView& view) const;

private:
    Trees() = default;

    Pipeline pipeline_;
    Buffer settings_;
};

} // namespace hog::render
