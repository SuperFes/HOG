#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/volume.hpp>
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

// The voxel world drawn in place of the ground: the volume, empty until it's given its tiles.
struct GlobeWorld {
    explicit GlobeWorld(const globe::Planet& planet,
                        const globe::Generator::Progress& progress = {})
        : generator{planet, progress}, world{generator, volume} {}
    GlobeWorld(const GlobeWorld&) = delete;
    GlobeWorld& operator=(const GlobeWorld&) = delete;
    GlobeWorld(GlobeWorld&&) = delete;
    GlobeWorld& operator=(GlobeWorld&&) = delete;
    ~GlobeWorld() = default;

    globe::Generator generator;
    globe::Volume volume;
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
