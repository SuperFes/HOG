#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/planet/climate.hpp>
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

// The furthest grass can reach, within reason.
inline constexpr double furthest_grass = 4095.0;
// Metres across the coarsest grid cells grass grows on, as far as a draw's packing carries.
inline constexpr double coarsest_grass_cells = 15.9;
// Blades grown in one grid cell of a patch at most.
inline constexpr std::uint32_t most_blades_per_cell = 63;

// How grass thins away from the eye: `density` blades a square metre out to `hold` metres,
// falling with the square of distance beyond, the blades left drawn wider to cover the ground as
// all of them would. Each blade shrinks away as it comes under a couple of pixels tall, so short
// grass stays near and tall grass reaches further, and all of it by `reach`.
struct GrassTier {
    double density = 40.0;
    double hold = 25.0;
    double reach = 260.0;
};

// Blades a grid cell `spacing` metres across grows `distance` metres from the eye, before
// most_blades_per_cell holds it back.
[[nodiscard]] double grass_layers(const GrassTier& tier, double spacing, double distance) noexcept;

// How much of a biome's ground is grass tall enough to wave in the wind, 0 to 1.
[[nodiscard]] float waving_grass(planet::Biome biome) noexcept;

// A patch's grass: blades grown in each of its grid cells, hashed from its seed, as many as grow
// that far from the eye.
struct GrassDraw {
    std::uint32_t slot = 0;
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    WorldPosition origin = {};
    double spacing = 1.0;   // metres between the patch's grid vertices
    std::uint32_t seed = 0; // of the patch's cell, the same on every machine
    double nearest = 0.0;   // metres from the eye its nearest blade may be, at most
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
    // linear light, so grass blends with the ground it stands on. It thins as `tier` has it.
    [[nodiscard]] static Result<Grass> create(Device& device, VkFormat colour_format,
                                              VkFormat depth_format, std::span<const Vec3> palette,
                                              const GrassTier& tier = {});

    // Records drawing into the colour and depth images being rendered to, of `extent`. The draws'
    // slots must hold patches already uploaded.
    void draw(VkCommandBuffer commands, VkExtent2D extent, const TerrainPool& pool,
              std::span<const GrassDraw> draws, const GrassView& view) const;

private:
    Grass() = default;

    Pipeline pipeline_;
    Buffer settings_;
    GrassTier tier_;
};

} // namespace hog::render
