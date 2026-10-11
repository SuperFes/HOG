#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>
#include <hog/planet/continents.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tile.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace hog::planet {

namespace {

// Keeps this layer's noise independent of every other layer seeded from the same planet seed.
constexpr std::uint64_t layer_salt = 0x636F'6E74'696E'656EULL; // "continen"

constexpr double frequency = 1.5;
constexpr int octaves = 6;

} // namespace

Continents::Continents(std::uint64_t seed, double ocean_fraction)
    : seed_{hash_combine(seed, layer_salt)} {
    HOG_ASSERT(ocean_fraction >= 0.0 && ocean_fraction < 1.0, "ocean fraction must be in [0, 1)");

    std::vector<std::int64_t> survey;
    for (int face = 0; face < face_count; ++face) {
        const auto samples = sample_tile(CellId::from_face(face), survey_level,
                                         [this](CellId cell) { return field(cell); });
        survey.insert(survey.end(), samples.begin(), samples.end());
    }

    // The ocean is the `ocean_cells` lowest samples; land must rise above the highest of them.
    const auto ocean_cells =
        static_cast<std::size_t>(std::llround(ocean_fraction * static_cast<double>(survey.size())));
    if (ocean_cells == 0) {
        sea_level_ = *std::ranges::min_element(survey) - 1;
        return;
    }
    const auto highest_ocean = survey.begin() + static_cast<std::ptrdiff_t>(ocean_cells - 1);
    std::ranges::nth_element(survey, highest_ocean);
    sea_level_ = *highest_ocean;
}

std::int64_t Continents::field(CellId cell) const {
    return fbm(seed_, noise_point(cell_center(cell), frequency), octaves);
}

} // namespace hog::planet
