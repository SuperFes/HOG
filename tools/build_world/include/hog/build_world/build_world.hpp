#pragma once

#include <hog/core/result.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/volume_archive.hpp>
#include <hog/globe/volume_build.hpp>
#include <hog/jobs/job_system.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace hog::build_world {

using globe::Area;

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
