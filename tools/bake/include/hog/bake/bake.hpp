#pragma once

#include <hog/baked/page_store.hpp>
#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/globe/generator.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/jobs/job_system.hpp>

#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <vector>

namespace hog::bake {

// What a page store of the generator's world is.
[[nodiscard]] baked::Baking baking_of(const globe::Generator& generator);

// Which pages a bake makes, down from the top page through the open leaves under each: every page
// whose finest nodes are `coarse` metres across or wider where the surface runs, and every page
// within `across` / 2 metres of `around` (metres from the centre) however fine, caves and all.
struct Reach {
    double coarse = std::numeric_limits<double>::infinity();
    std::optional<DVec3> around;
    double across = 0.0;
};

// The pages a bake reaching as `reach` makes under an open leaf at `leaf` of the page rooted at
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
};

// Called after each batch with the pages done and those known to do so far.
using Progress = std::function<void(std::size_t done, std::size_t known)>;

// Fills the pages `reach` asks for that the store doesn't hold as this generator makes them yet,
// `batch` at a time across the job system's threads, each batch written together, so a stopped
// bake carries on where it left off. Pages held are read for the open leaves to go down from.
[[nodiscard]] Result<Baked> bake(const globe::Generator& generator, baked::PageStore& store,
                                 jobs::JobSystem& jobs, const Reach& reach, std::size_t batch = 64,
                                 const Progress& progress = {});

} // namespace hog::bake
