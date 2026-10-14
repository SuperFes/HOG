#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/planet/climate.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/vulkan.hpp>
#include <hog/terrain/patch.hpp>
#include <hog/terrain/stitching.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace hog::render {

class Device;

inline constexpr std::uint32_t patch_vertex_count = terrain::patch_side * terrain::patch_side;

struct TerrainPoolDesc {
    std::uint32_t patches = 1; // held on the GPU at once
    std::uint32_t frames = 1;  // in flight, each staging through its own buffer
    std::uint32_t uploads = 1; // staged per frame at most
};

// Patches' vertices on the GPU, a slot each.
class TerrainPool {
public:
    [[nodiscard]] static Result<TerrainPool> create(const Device& device,
                                                    const TerrainPoolDesc& desc);

    [[nodiscard]] VkBuffer vertices() const noexcept { return vertices_.handle(); }
    [[nodiscard]] std::uint32_t capacity() const noexcept {
        return static_cast<std::uint32_t>(held_.size());
    }
    [[nodiscard]] std::uint32_t free_slots() const noexcept {
        return static_cast<std::uint32_t>(free_.size());
    }

    // A slot nothing is held in, if any is left.
    [[nodiscard]] std::optional<std::uint32_t> acquire();
    // Frames in flight may still draw from it: the next upload into it waits for them.
    void release(std::uint32_t slot);

    // Copies a patch's vertices in for `frame`'s next record_uploads, within its budget. The
    // frame's last submit must have finished.
    Result<> stage(std::uint32_t frame, std::uint32_t slot,
                   std::span<const terrain::PatchVertex> vertices);
    // Records copying what `frame` has staged into its slots, after drawing already recorded
    // from them and before any recorded next.
    Result<> record_uploads(VkCommandBuffer commands, std::uint32_t frame);

private:
    struct Staged {
        std::uint32_t slot;
        std::uint32_t at; // in the frame's staging buffer
    };

    TerrainPool() = default;

    Buffer vertices_;
    std::vector<Buffer> staging_;
    std::vector<std::vector<Staged>> staged_;
    std::uint32_t uploads_ = 0;
    std::vector<bool> held_;
    std::vector<std::uint32_t> free_;
};

// Where the terrain is seen from and how it's lit.
struct TerrainView {
    Camera camera;
    DVec3 sun{1.0, 0.0, 0.0}; // unit, towards the sun
    // How bright ground facing away from the sun stays, from 0 to 1.
    double night_light = 0.0;
};

// A patch held in a pool's slot.
struct TerrainDraw {
    std::uint32_t slot = 0;
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    WorldPosition origin = {};
    // Edge bits, where the neighbour is a level coarser.
    std::uint8_t coarser_edges = 0;
};

// Flat colours by biome, until there are materials.
using BiomePalette = std::array<Vec3, planet::biome_count>;

// Ground drawn from patches in a pool, each placed from the eye so floats keep their precision
// near it.
class Terrain {
public:
    [[nodiscard]] static Result<Terrain> create(const Device& device, VkFormat colour_format,
                                                VkFormat depth_format, const BiomePalette& palette);

    // Records drawing into the colour and depth images being rendered to, of `extent`. Their
    // slots must hold patches already uploaded.
    void draw(VkCommandBuffer commands, VkExtent2D extent, const TerrainPool& pool,
              std::span<const TerrainDraw> draws, const TerrainView& view) const;

private:
    struct Range {
        std::uint32_t first;
        std::uint32_t count;
    };

    Terrain() = default;

    Pipeline pipeline_;
    Buffer indices_;
    Buffer palette_;
    std::array<Range, terrain::stitchings> stitchings_{};
};

} // namespace hog::render
