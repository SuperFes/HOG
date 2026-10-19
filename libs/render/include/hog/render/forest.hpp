#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/render/grass.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/terrain.hpp>
#include <hog/render/trees.hpp>
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

// A patch's grown trees: the same lattice cells the stamps key to, at one baked detail.
struct ForestDraw {
    std::uint32_t slot = 0;
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    WorldPosition origin = {};
    std::uint32_t face = 0;      // the patch's cube face
    std::uint32_t lattice_i = 0; // the patch's first lattice cell
    std::uint32_t lattice_j = 0;
    int shift = 0; // grid cells across a lattice cell, as a power of two; -1 for half
    double fade_far = forest_far; // metres from the eye its trees have shrunk away by
    int detail = 0;               // 0 the fullest, forest_details - 1 the coarsest
};

// Pines grown whole by libs/flora, a few variants baked at a few details and planted by the same
// lattice, stand field and dice as the far stamps: solid trunks and boughs, needle-spray cards.
class Forest {
public:
    // Needles take each biome's colour blended from `palette`, as the stamps' crowns do.
    [[nodiscard]] static Result<Forest> create(Device& device, VkFormat colour_format,
                                               VkFormat depth_format,
                                               std::span<const Vec3> palette);

    // Records drawing into the colour and depth images being rendered to, of `extent`. The draws'
    // slots must hold patches already uploaded.
    void draw(VkCommandBuffer commands, VkExtent2D extent, const TerrainPool& pool,
              std::span<const ForestDraw> draws, const GrassView& view) const;

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
