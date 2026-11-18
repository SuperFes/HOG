#include <hog/core/portable_maths.hpp>
#include <hog/flora/woods.hpp>

#include <algorithm>
#include <array>
#include <numbers>

namespace hog::flora {
namespace {

// How far through the year each month's warmth lies from its mean, as the cosine of its turn.
constexpr double half_root_three = 0.86602540378443865;
constexpr std::array<double, 12> months{1.0,  half_root_three,  0.5,  0.0, -0.5, -half_root_three,
                                        -1.0, -half_root_three, -0.5, 0.0, 0.5,  half_root_three};

// Rising smoothly from 0 at `from` to 1 at `to`.
double rise(double value, double from, double to) noexcept {
    const double t = std::clamp((value - from) / (to - from), 0.0, 1.0);
    return t * t * (3.0 - (2.0 * t));
}

// Rising from `from` to `full`, falling again from `fading` to `gone`.
double band(double value, double from, double full, double fading, double gone) noexcept {
    return rise(value, from, full) * (1.0 - rise(value, fading, gone));
}

// °C a day's mean must stand over for it to count towards the growing season (Körner).
constexpr double growing_from = 0.9;

// Metres a year of water a degree of biotemperature would take (Holdridge).
constexpr double evaporation_per_degree = 0.05893;

// How well water under the ground is kept up: by the rain falling on it, or where that's too
// little, by a river big enough to run the year round (the Nile's gallery forests).
double fed(const Habitat& habitat) noexcept {
    return std::max(rise(habitat.rain, 0.3, 0.8), rise(habitat.flow, 0.5, 5.0));
}

// How far a wander of 1 makes a kind more at home: a little over twice, so groves stay mixed but
// at their strongest.
constexpr double wandering = 0.8;
// What share of the cover the deepest glade takes away.
constexpr double glades = 0.6;

// Leans each kind's suitability, by its wander, and where it's an outlier, up to as much as the
// best's: only for a kind the habitat suits at least a little against the best, so outliers stand
// where they could live, never palms in the boreal woods.
void leaned(std::array<double, kinds>& suits, const Lean& lean, double best) noexcept {
    if (best <= 0.0) {
        return;
    }
    for (std::size_t kind = 0; kind < kinds; ++kind) {
        double& suit = suits.at(kind);
        const double possible = rise(suit / best, 0.02, 0.15);
        suit *= maths::exp(wandering * std::clamp(lean.wander.at(kind), -1.5, 1.5));
        suit = std::max(suit, 1.2 * best * possible * std::clamp(lean.outlier.at(kind), 0.0, 1.0));
    }
}

} // namespace

Warmth warmth_of(const Habitat& habitat) noexcept {
    Warmth warmth{.coldest = habitat.temperature - habitat.swing};
    for (const double turn : months) {
        const double month = habitat.temperature + (habitat.swing * turn);
        warmth.biotemperature += std::clamp(month, 0.0, 30.0) / static_cast<double>(months.size());
    }
    // Körner's growing season, the days whose mean stands over 0.9 °C, through the year's cosine:
    // the share of the turn it lies over that, and its mean there.
    const double swing = std::max(habitat.swing, 1e-3);
    const double over = std::clamp((growing_from - habitat.temperature) / swing, -1.0, 1.0);
    warmth.growing = maths::acos(over) / std::numbers::pi;
    if (warmth.growing > 0.0) {
        const double turn = std::numbers::pi * warmth.growing;
        warmth.growing_mean = habitat.temperature + (swing * maths::sin(turn) / turn);
    }
    return warmth;
}

double groundwater_of(const Habitat& habitat) noexcept {
    return (1.0 - rise(habitat.over_drainage, 2.0, 8.0)) * fed(habitat);
}

double moisture_of(const Habitat& habitat, const Warmth& warmth) noexcept {
    const double taken = evaporation_per_degree * warmth.biotemperature;
    // Cold enough to take none, any water is plenty.
    const double rained = taken > 0.0 ? habitat.rain / taken : 2.0 * habitat.rain;
    return std::max(rained, 2.0 * groundwater_of(habitat));
}

Woods woods_of(const Habitat& habitat, const Lean& lean) noexcept {
    const Warmth warmth = warmth_of(habitat);
    const double moisture = moisture_of(habitat, warmth);
    const double reached = groundwater_of(habitat);
    const bool flooded = habitat.water > 0.0;
    // Wet feet: open water shallow enough to stand in, or the water a step under the ground.
    const double wet = flooded ? 1.0 : (1.0 - rise(habitat.over_drainage, 0.3, 2.5)) * fed(habitat);

    std::array<double, kinds> suits{};
    const auto suit = [&](Kind kind) -> double& {
        return suits.at(static_cast<std::size_t>(kind));
    };
    const double bio = warmth.biotemperature;
    // Conifers hold on in the cold and on poor, thin ground.
    suit(Kind::Needleleaf) = band(bio, 1.0, 3.0, 13.0, 19.0) * rise(moisture, 0.3, 0.6) *
                             (0.7 + (0.3 * (1.0 - rise(habitat.soil, 0.3, 1.0))));
    // Dropping leaves wins only where there's a cold season to drop them for.
    suit(Kind::Deciduous) = band(bio, 5.0, 8.0, 15.0, 20.0) * rise(moisture, 0.5, 1.0) *
                            (1.0 - rise(warmth.coldest, 2.0, 8.0));
    suit(Kind::Evergreen) =
        rise(bio, 14.0, 19.0) * rise(moisture, 0.8, 1.6) * rise(warmth.coldest, 0.0, 8.0);
    suit(Kind::Dry) = rise(bio, 11.0, 16.0) * band(moisture, 0.15, 0.3, 0.7, 1.3);
    suit(Kind::Palm) = 0.7 * rise(bio, 17.0, 22.0) * rise(warmth.coldest, 4.0, 10.0) *
                       std::max(rise(moisture, 0.7, 1.4), reached);
    // The rest drown where the ground is waterlogged, and stand in none.
    for (double& kind : suits) {
        kind *= flooded ? 0.0 : 1.0 - (0.7 * wet);
    }
    suit(Kind::Wetland) = 1.2 * wet * wet * rise(warmth.growing_mean, 5.4, 7.9);

    const double best = *std::ranges::max_element(suits);
    double cover = rise(best, 0.05, 0.3);
    leaned(suits, lean, best);
    // Under the treeline: warm enough for long enough to grow wood.
    cover = std::min(cover, rise(warmth.growing_mean, 5.4, 7.9) * rise(warmth.growing, 0.2, 0.3));
    cover = std::min(cover, rise(moisture, 0.15, 0.9));
    // A few take hold in the cracks of bare rock and in the thinnest soil.
    cover = std::min(cover, (0.15 + (0.85 * rise(habitat.soil, 0.0, 0.3))) *
                                (1.0 - (0.85 * habitat.bare)));
    cover = std::min(cover, 1.0 - rise(habitat.slope, 1.0, 2.0));
    cover = std::min(cover, 1.0 - rise(habitat.ice, 0.0, 0.3));
    cover *= 1.0 - (glades * std::clamp(lean.glade, 0.0, 1.0));
    if (flooded) {
        cover = std::min(cover, 1.0 - rise(habitat.water, 0.5, 2.0));
        // In the sea, only mangroves, where it never comes near frost.
        if (habitat.sea) {
            cover = std::min(cover, rise(warmth.coldest, 10.0, 16.0));
        }
    }

    Woods woods{.cover = std::max(cover, 0.0)};
    double total = 0.0;
    for (const double kind : suits) {
        total += kind * kind;
    }
    if (woods.cover <= 0.0 || total <= 0.0) {
        woods.cover = 0.0;
        return woods;
    }
    for (std::size_t kind = 0; kind < kinds; ++kind) {
        woods.shares.at(kind) = suits.at(kind) * suits.at(kind) / total;
    }
    return woods;
}

} // namespace hog::flora
