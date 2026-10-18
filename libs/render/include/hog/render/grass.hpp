#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/terrain.hpp>
#include <hog/render/vulkan.hpp>
#include <hog/terrain/bend.hpp>

#include <cstdint>
#include <span>

namespace hog::render {

class Device;

// Metres from the eye blades keep their height to, and where they've shrunk away; as the shader
// fades them.
inline constexpr double grass_near = 25.0;
inline constexpr double grass_far = 55.0;
// Blades grown in one grid cell of a patch at most.
inline constexpr std::uint32_t most_blades_per_cell = 63;

// A patch's grass: blades grown in each of its grid cells, hashed from its seed.
struct GrassDraw {
    std::uint32_t slot = 0;
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    WorldPosition origin = {};
    double spacing = 1.0; // metres between the patch's grid vertices
    std::uint32_t blades_per_cell = 1;
    std::uint32_t seed = 0; // of the patch's cell, the same on every machine
};

// Where the grass is seen from, how it's lit, and the wind that moves it.
struct GrassView {
    Camera camera;
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    terrain::Bend bend = {};
    // The frame's air, which lights the blades as it does the ground.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    AirBindings air = {};
    DVec3 wind{0.0};      // near the ground, m/s, in the planet's frame
    double seconds = 0.0; // of sway so far
};

// Grass grown on the ground's own patches, straight from the pool's vertices: deterministic from
// each patch's seed, placed by biome, and blown by the wind.
class Grass {
public:
    // Blades take each biome's colour from `palette`, what its ground's material averages to in
    // linear light, so grass blends with the ground it stands on.
    [[nodiscard]] static Result<Grass> create(Device& device, VkFormat colour_format,
                                              VkFormat depth_format, std::span<const Vec3> palette);

    // Records drawing into the colour and depth images being rendered to, of `extent`. The draws'
    // slots must hold patches already uploaded.
    void draw(VkCommandBuffer commands, VkExtent2D extent, const TerrainPool& pool,
              std::span<const GrassDraw> draws, const GrassView& view) const;

private:
    Grass() = default;

    Pipeline pipeline_;
    Buffer settings_;
};

} // namespace hog::render
