#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/detail.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/surface_mesh.hpp>
#include <hog/globe/voxel_store.hpp>
#include <hog/globe/world.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/terrain/block_mesh.hpp>
#include <hog/terrain/material.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace hog::client {

// The planet drawn as the globe: a preset by name or a planet file, Earth's knobs if neither.
// A preset, or Earth, takes the seed and radius of the world it's drawn in place of, so the sky
// fits it.
[[nodiscard]] Result<globe::Planet> globe_planet(std::optional<std::string_view> named,
                                                 std::uint64_t seed, double radius);

// The voxel world a planet file describes: its [globe] table, with the world's seed, or Earth's
// knobs at the seed and radius given if it has none.
[[nodiscard]] Result<globe::Planet> globe_planet_in(const std::filesystem::path& file,
                                                    std::uint64_t seed, double radius);

// The voxel world drawn in place of the ground, nothing yet changed in it.
struct GlobeWorld {
    explicit GlobeWorld(const globe::Planet& planet,
                        const globe::Generator::Progress& progress = {})
        : generator{planet, progress}, store{generator.layers()}, world{generator, store} {}
    GlobeWorld(const GlobeWorld&) = delete;
    GlobeWorld& operator=(const GlobeWorld&) = delete;
    GlobeWorld(GlobeWorld&&) = delete;
    GlobeWorld& operator=(GlobeWorld&&) = delete;
    ~GlobeWorld() = default;

    globe::Generator generator;
    globe::VoxelStore store;
    globe::World world;
};

// What a material averages to, seen from afar.
[[nodiscard]] Vec3 surface_colour(globe::Material material);

// How each material looks up close, in Material's order, averaging to its surface_colour.
[[nodiscard]] std::array<terrain::Look, globe::material_count> globe_looks();

// Texels across a material's tile: about 1.5 cm each over the four metres a tile spans.
inline constexpr int material_size = 256;

// The tiles block meshes name: the globe's materials by number.
[[nodiscard]] std::vector<terrain::MaterialImage> block_tiles();

} // namespace hog::client
