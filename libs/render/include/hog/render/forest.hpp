#pragma once

#include <hog/core/result.hpp>
#include <hog/render/anchors.hpp>
#include <hog/render/grass.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/vulkan.hpp>

#include <array>
#include <cstdint>
#include <span>

namespace hog::render {

class Device;

// Metres from the eye the grown forest reaches; the far stamps hold off inside it and carry on
// beyond.
inline constexpr double forest_far = 240.0;
// Baked levels of detail, finest first.
inline constexpr int forest_details = 3;

// Pines grown whole by libs/flora, a few variants baked at a few details, each planted on a tree
// anchor bound to its spot in the world: solid trunks and boughs, needle-spray cards. Every tree
// sheds detail by its own distance.
class Forest {
public:
    // The fullest detail reaches `full` metres and the middle one `mid`; the forest itself ends
    // at forest_far, where the stamps take over.
    [[nodiscard]] static Result<Forest> create(Device& device, VkFormat colour_format,
                                               VkFormat depth_format, double full, double mid);

    // Records drawing into the colour and depth images being rendered to, of `extent`. The draws'
    // slots must hold anchors already uploaded.
    void draw(VkCommandBuffer commands, VkExtent2D extent, const AnchorPool& pool,
              std::span<const StandDraw> draws, const GrassView& view) const;

private:
    Forest() = default;

    Pipeline wood_;
    Pipeline leaves_;
    Buffer settings_;
    Buffer carpentry_;
    // The most indices any variant carries at each detail: a draw's vertex counts.
    std::array<std::uint32_t, forest_details> most_wood_{};
    std::array<std::uint32_t, forest_details> most_leaves_{};
};

} // namespace hog::render
