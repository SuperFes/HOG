#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/globe/drainage.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/world.hpp>

#include <cstdint>
#include <filesystem>
#include <vector>

// Pictures of a generated planet to judge it by, drawn only from samples of its 3D field.
namespace hog::stills {

struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels; // RGB, row by row from the top

    [[nodiscard]] std::array<std::uint8_t, 3> at(int x, int y) const;
};

// A place on the planet and the way to face from it, in degrees.
struct Place {
    double latitude = 0.0;
    double longitude = 0.0;
    double heading = 90.0; // clockwise from north

    [[nodiscard]] DVec3 direction() const noexcept;
};

// The whole planet, twice as wide as tall, longitude across and latitude up: the ground's height,
// shaded by its slope, the sea by its depth.
[[nodiscard]] Image map(const globe::Generator& generator, int width, int threads);

// The whole planet as `map` is, with where rain would run: hollows that fill to a lake before they
// spill, and the ground's rivers by how much drains through them, from cells of `level`; the
// ground first worn by running water for `erosion`'s steps.
[[nodiscard]] Image drainage(const globe::Generator& generator, int width, int level, int threads,
                             const globe::Erosion& erosion = {.steps = 0});

// A square of ground `span` metres across, centred on `place`, seen from straight above.
[[nodiscard]] Image relief(const globe::Generator& generator, const Place& place, double span,
                           int size, int threads);

// A cut straight down through the shell along `place`'s heading, `length` metres of arc wide,
// from `bottom` to `top` metres over the radius: what each point is made of.
[[nodiscard]] Image section(const globe::Generator& generator, const Place& place, double length,
                            double bottom, double top, int width, int threads);

// The same cut through the world as it now is, voxel by voxel: each pixel shows the voxel it
// falls in, the store's where it holds one.
[[nodiscard]] Image voxel_section(const globe::World& world, const Place& place, double length,
                                  double bottom, double top, int width);

// A slice `depth` metres under the ground everywhere, `span` metres across, centred on `place`:
// what each point is made of, so caves show as their passages run.
[[nodiscard]] Image plan(const globe::Generator& generator, const Place& place, double span,
                         double depth, int size, int threads);

[[nodiscard]] Result<void> write_png(const std::filesystem::path& path, const Image& image);

} // namespace hog::stills
