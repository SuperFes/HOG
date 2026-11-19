#pragma once

#include <hog/baked/chunk_filling.hpp>
#include <hog/baked/chunk_store.hpp>
#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/globe/generator.hpp>
#include <hog/grid/grid.hpp>
#include <hog/jobs/job_system.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>

namespace hog::bake {

// How far a bake goes, down from the root through what each band leaves open: where the surface
// runs, down to the band holding the level `coarse` metres rounds to; within `across` / 2 metres
// of `around` (metres from the centre), down to the finest, caves and all.
struct Reach {
    double coarse = std::numeric_limits<double>::infinity();
    std::optional<DVec3> around;
    double across = 0.0;
};

// The level `metres` rounds to: the first whose nodes are no wider, so a bake gives at least the
// detail asked for; the finest where none is that fine.
[[nodiscard]] std::uint8_t depth_of(const grid::Grid& grid, double metres) noexcept;

// Whether a bake reaching as `reach` fills `region`: near the place, or where `surface` says the
// ground runs through it and its band is no deeper than the one holding the level asked for.
[[nodiscard]] bool wanted(const grid::Grid& grid, const baked::Bands& bands, const Reach& reach,
                          baked::Region region, bool surface) noexcept;

struct Baked {
    std::size_t filled = 0; // regions filled and written
    std::size_t kept = 0;   // already held as this generator makes them
    // Of each kind's trees written.
    std::size_t shape_bytes = 0;
    std::size_t material_bytes = 0;
    std::size_t water_bytes = 0;
    std::size_t woods_bytes = 0;
};

// Called after each batch with the regions done and those found but not done yet.
using Progress = std::function<void(std::size_t done, std::size_t waiting)>;

struct Pace {
    // Regions filled at once across the job system's threads and written together.
    std::size_t batch = 64;
    // How many levels one fill takes at most.
    std::uint8_t fill_levels = 6;
};

// Fills the regions `reach` asks for that the store doesn't hold yet as this generator makes them,
// a batch at a time, each batch written together, so a stopped bake carries on where it left off.
// It goes depth first, so what it holds in memory is bounded by the regions along one way down,
// however many it fills. Regions held are read for what's open under them.
[[nodiscard]] Result<Baked> bake(const globe::Generator& generator, baked::ChunkStore& store,
                                 jobs::JobSystem& jobs, const Reach& reach, const Pace& pace = {},
                                 const Progress& progress = {});

} // namespace hog::bake
