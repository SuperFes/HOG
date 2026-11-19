#pragma once

#include <hog/baked/page_store.hpp>
#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/globe/generator.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/jobs/job_system.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <vector>

namespace hog::bake {

// How far a bake goes, down from the top page through the open leaves under each: where the
// surface runs, down to the level `coarse` metres rounds to; within `across` / 2 metres of
// `around` (metres from the centre), down to the finest, caves and all.
struct Reach {
    double coarse = std::numeric_limits<double>::infinity();
    std::optional<DVec3> around;
    double across = 0.0;
};

// The level `metres` rounds to: the first whose nodes are no wider, so a bake gives at least the
// detail asked for; the finest where none is that fine.
[[nodiscard]] std::uint8_t depth_of(const grid::Grid& grid, double metres) noexcept;

// How many levels under its root a bake reaching as `reach` fills the page rooted at `page`: as
// far as `coarse` asks where `surface` says the ground runs through it, whole near the place, and
// none where neither wants it.
[[nodiscard]] std::uint8_t levels_for(const grid::Grid& grid, const Reach& reach, grid::Key page,
                                      bool surface) noexcept;

// The pages a bake reaching as `reach` fills under an open leaf at `leaf` of the page rooted at
// `page`, at its foot. `surface` says whether it's open for the ground passing near rather than
// for caves that may lie in it.
[[nodiscard]] std::vector<grid::Key> pages_under(const grid::Grid& grid, const Reach& reach,
                                                 grid::Key page, grid::Key leaf, bool surface);

struct Baked {
    std::size_t filled = 0; // pages filled and written
    std::size_t kept = 0;   // already held as this generator makes them
    // Of each kind's trees written.
    std::size_t shape_bytes = 0;
    std::size_t material_bytes = 0;
    std::size_t water_bytes = 0;
    std::size_t woods_bytes = 0;
};

// Called after each batch with the pages done and those found but not done yet.
using Progress = std::function<void(std::size_t done, std::size_t waiting)>;

// Fills the pages `reach` asks for that the store doesn't hold as deep as asked as this generator
// makes them, `batch` at a time across the job system's threads, each batch written together, so
// a stopped bake carries on where it left off. It goes depth first, so what it holds in memory
// is bounded by the pages along one way down, however many it fills. Pages held are read for the
// open leaves to go down from.
[[nodiscard]] Result<Baked> bake(const globe::Generator& generator, baked::PageStore& store,
                                 jobs::JobSystem& jobs, const Reach& reach, std::size_t batch = 64,
                                 const Progress& progress = {});

} // namespace hog::bake
