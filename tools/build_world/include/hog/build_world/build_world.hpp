#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/volume_archive.hpp>
#include <hog/jobs/job_system.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace hog::build_world {

// A square of tile columns on one of the planet's six sides, counted in tiles.
struct Area {
    std::uint8_t face = 0;
    std::uint32_t i = 0;
    std::uint32_t j = 0;
    std::uint32_t across = 1;

    friend bool operator==(const Area&, const Area&) = default;
};

// Tile columns along a side of one of the planet's six sides.
[[nodiscard]] std::uint32_t tiles_across(const globe::Generator& generator);
// `across` tile columns square about the one under `direction`, kept on its side.
[[nodiscard]] Area around(const globe::Generator& generator, DVec3 direction, std::uint32_t across);
// Each of the planet's six sides whole.
[[nodiscard]] std::vector<Area> whole(const globe::Generator& generator);

struct Built {
    std::size_t made = 0;    // tile columns made and written
    std::size_t skipped = 0; // already in the archive
    std::size_t bytes = 0;   // of those written
};

// Called after each batch with the tile columns done and to do in all.
using Progress = std::function<void(std::size_t done, std::size_t total)>;

// Makes every tile column of `areas` the archive doesn't hold yet, across the job system's
// threads, and writes them `batch` at a time, so a stopped build carries on where it left off.
// Fails, writing nothing, on an archive made from another planet.
[[nodiscard]] Result<Built> build(const globe::Generator& generator, globe::VolumeArchive& archive,
                                  jobs::JobSystem& jobs, const std::vector<Area>& areas,
                                  std::size_t batch = 512, const Progress& progress = {});

} // namespace hog::build_world
