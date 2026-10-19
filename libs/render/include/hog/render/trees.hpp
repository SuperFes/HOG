#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/render/grass.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/terrain.hpp>
#include <hog/render/vulkan.hpp>

#include <cstdint>
#include <span>

namespace hog::render {

class Device;

// The fixed lattice trees are keyed to, coarser than the bushes': every patch maps its grid onto
// cells of this level, so a tree stands in the same spot at every terrain level and on every
// machine.
inline constexpr int tree_lattice_level = 14;
// Metres from the eye trees have shrunk away by.
inline constexpr double trees_far = 900.0;

// A patch's trees: at most one in each lattice cell over it.
struct TreeDraw {
    std::uint32_t slot = 0;
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    WorldPosition origin = {};
    std::uint32_t face = 0;      // the patch's cube face
    std::uint32_t lattice_i = 0; // the patch's first lattice cell
    std::uint32_t lattice_j = 0;
    int shift = 0;               // grid cells across a lattice cell, as a power of two; -1 for half
    double fade_far = trees_far; // metres from the eye its trees have shrunk away by
    double hold_off = 0.0;       // metres the stamps keep off, where the grown forest covers
};

// Pines grown on the ground's own patches: crossed quads cut to a tiered conical crown over a
// bare trunk, sized and spread by biome, their tops swaying stiffly in the wind.
class Trees {
public:
    // Needles take each biome's colour blended from `palette`, as grass blades do.
    [[nodiscard]] static Result<Trees> create(Device& device, VkFormat colour_format,
                                              VkFormat depth_format, std::span<const Vec3> palette);

    // Records drawing into the colour and depth images being rendered to, of `extent`. The draws'
    // slots must hold patches already uploaded.
    void draw(VkCommandBuffer commands, VkExtent2D extent, const TerrainPool& pool,
              std::span<const TreeDraw> draws, const GrassView& view) const;

private:
    Trees() = default;

    Pipeline pipeline_;
    Buffer settings_;
};

} // namespace hog::render
