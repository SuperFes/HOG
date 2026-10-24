#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/climate.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/grass_lattice.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/terrain.hpp>
#include <hog/render/vulkan.hpp>
#include <hog/terrain/bend.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace hog::render {

class Device;

// The furthest grass can reach, within reason.
inline constexpr double furthest_grass = 4095.0;
// Metres across the coarsest grid cells grass grows on, as far as a draw's packing carries.
inline constexpr double coarsest_grass_cells = 15.9;
// Levels into a patch its blades are sown at most: a blade at most for each of its cells that
// deep, 64 to a grid cell.
inline constexpr int most_grass_depth = 8;
// Blades grown in a frame at most, in view; those past it are left out.
inline constexpr std::uint32_t most_grown_blades = 1U << 19U;
// Patches grass grows on in a frame at most; those past it are left out.
inline constexpr std::size_t most_grass_draws = 1024;
// Bytes of a blade as grown, its root from the eye first, as four floats.
inline constexpr std::size_t grown_blade_bytes = 48;

// How grass thins away from the eye: `density` blades a square metre out to `hold` metres,
// falling with the square of distance beyond, the blades left drawn wider to cover the ground as
// all of them would. Each blade shrinks away as it comes under a couple of pixels tall, so short
// grass stays near and tall grass reaches further, and all of it by `reach`.
struct GrassTier {
    double density = 40.0;
    double hold = 25.0;
    double reach = 260.0;
};

// The most times its biome's density grass grows in its thickest patches, as lush.slang's.
inline constexpr double thickest_grass = 3.0;

// Blades a grid cell `spacing` metres across grows `distance` metres from the eye, where it
// grows `thickness` times its density. Thicker grass
// holds that density only until it's thinned to the density of grass of thickness 1.
[[nodiscard]] double grass_layers(const GrassTier& tier, double spacing, double distance,
                                  double thickness = 1.0) noexcept;

// Levels into a patch its blades are sown, where its grid cells grow `layers` blades: deep enough
// that its cells hold as many, at most most_grass_depth and `to_lattice` deep.
[[nodiscard]] int grass_depth(double layers, int to_lattice) noexcept;

// How much of a biome's ground is grass tall enough to wave in the wind, 0 to 1.
[[nodiscard]] float waving_grass(planet::Biome biome) noexcept;

// A biome's grass on the whole: its blades' mean height in metres, and the share of the ground
// they cover.
struct GrassStand {
    float height = 0.0F;
    float cover = 0.0F;
};

[[nodiscard]] GrassStand grass_stand(planet::Biome biome) noexcept;

// A patch's grass: the blades on the lattice within its cell, as many as grow that far from the
// eye.
struct GrassDraw {
    std::uint32_t slot = 0;
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    WorldPosition origin = {};
    double spacing = 1.0; // metres between the patch's grid vertices
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    planet::FaceIJ cell = {};
    double nearest = 0.0; // metres from the eye its nearest blade may be, at most
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

// Grass grown on the ground's own patches, straight from the pool's vertices: each blade on its
// own spot of the world's lattice, whichever patch draws it, placed by biome, and blown by the
// wind.
class Grass {
public:
    // Blades take each biome's colour from `palette`, what its ground's material averages to in
    // linear light, so grass blends with the ground it stands on. They stand on `lattice`, and
    // thin as `tier` has it.
    [[nodiscard]] static Result<Grass> create(Device& device, VkFormat colour_format,
                                              VkFormat depth_format, std::span<const Vec3> palette,
                                              const GrassLattice& lattice,
                                              const GrassTier& tier = {});

    // Records growing the frame's blades on the draws' patches, those that may be seen in a view
    // of `extent`, ready to draw. Recorded outside rendering, after the last frame's draw. The
    // draws' slots must hold patches already uploaded.
    void grow(VkCommandBuffer commands, VkExtent2D extent, const TerrainPool& pool,
              std::span<const GrassDraw> draws, const GrassView& view) const;
    // Records drawing the blades last grown into the colour and depth images being rendered to,
    // of `extent`, seen as they were grown.
    void draw(VkCommandBuffer commands, VkExtent2D extent, const GrassView& view) const;

    // The draw's indirect arguments: six vertices, and the blades grown, those left out too.
    [[nodiscard]] VkBuffer grown_draw() const noexcept { return grown_draw_.handle(); }
    // The blades grown, grown_blade_bytes each.
    [[nodiscard]] VkBuffer grown_blades() const noexcept { return blades_.handle(); }

private:
    Grass() = default;

    Pipeline pipeline_;
    Pipeline growing_;
    Buffer settings_;
    Buffer blades_;
    Buffer grown_draw_;
    Buffer patches_;
    GrassTier tier_;
    GrassLattice lattice_;
};

} // namespace hog::render
