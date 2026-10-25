#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>
#include <hog/planet/continents.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tile.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace hog::planet {

namespace {

// Keeps this layer's noise independent of every other layer seeded from the same planet seed.
constexpr std::uint64_t layer_salt = 0x636F'6E74'696E'656EULL; // "continen"

constexpr double frequency = 3.0;
constexpr int octaves = 6;
// Shares of the field from the plates' crust and from the noise roughening it.
constexpr std::int64_t crust_share = fixed_one / 2;
constexpr std::int64_t noise_share = fixed_one - crust_share;
// Passes averaging each plate grid cell's crust with its neighbours', spreading the change
// between two plates' crust over about this many cells either side of their boundary squared.
constexpr int crust_passes = 128;

// The crust is read up to this far from where it's asked for, about a third of it typically, along
// smooth bends this long, so the plates' edges and the coasts along them wander instead of running
// straight.
constexpr std::uint64_t warp_salt = 0x7761'7270'0000'0000ULL; // "warp"
constexpr double warp_reach = 25'000.0;                       // metres
constexpr double warp_wavelength = 80'000.0;
constexpr int warp_octaves = 2;

std::vector<std::int64_t> crust(const Plates& plates) {
    const LevelGrid& grid = plates.grid();
    std::vector<double> crust(grid.size());
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        crust[index] = plates.plates()[plates.plate()[index]].continental;
    }
    std::vector<double> smoothed(grid.size());
    for (int pass = 0; pass < crust_passes; ++pass) {
        for (std::uint32_t index = 0; index < grid.size(); ++index) {
            double sum = crust[index];
            for (const std::uint32_t neighbor : grid.neighbors(index)) {
                sum += crust[neighbor];
            }
            smoothed[index] = sum / static_cast<double>(grid.neighbors(index).size() + 1);
        }
        std::swap(crust, smoothed);
    }
    std::vector<std::int64_t> fixed(grid.size());
    std::ranges::transform(crust, fixed.begin(), [](double value) {
        return std::llround(value * static_cast<double>(fixed_one));
    });
    return fixed;
}

} // namespace

Continents::Continents(std::uint64_t seed, double ocean_fraction,
                       std::shared_ptr<const Plates> plates)
    : seed_{hash_combine(seed, layer_salt)}, warp_seed_{hash_combine(seed, warp_salt)},
      plates_{std::move(plates)}, crust_{crust(*plates_)} {
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

template <int Bits>
std::int64_t Continents::field(CellId cell) const {
    constexpr int finer = Bits - fixed_fraction_bits;
    const DVec3 center = cell_center(cell);
    const std::int64_t noise = fbm(seed_, noise_point<Bits>(center, frequency), octaves);
    const double radius = plates_->radius();
    const NoisePoint at = noise_point(center, radius / warp_wavelength);
    const DVec3 shift{fixed_to_double(fbm(warp_seed_, at, warp_octaves)),
                      fixed_to_double(fbm(warp_seed_ + 1, at, warp_octaves)),
                      fixed_to_double(fbm(warp_seed_ + 2, at, warp_octaves))};
    const CellId warped = cell_at(glm::normalize(center + (shift * (warp_reach / radius))));
    const std::int64_t crust = plates_->grid().sample(crust_, warped) << finer;
    return ((noise * noise_share) + (crust * crust_share)) >> fixed_fraction_bits;
}

template std::int64_t Continents::field<fixed_fraction_bits>(CellId cell) const;
template std::int64_t Continents::field<fine_fraction_bits>(CellId cell) const;

} // namespace hog::planet
