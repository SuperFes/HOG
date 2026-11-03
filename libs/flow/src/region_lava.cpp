#include <hog/flow/lava.hpp>
#include <hog/flow/region.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace hog::flow {
namespace {

using globe::Material;
using globe::Site;
using globe::Voxel;

constexpr double full = Voxel::full;
// Joules to turn a kilogram of water at the air's temperature to steam.
constexpr double boiling_kelvin = 373.15;
constexpr double air_kelvin = 288.0;
constexpr double latent_heat = 2.26e6;

bool molten(Material liquid) noexcept {
    return liquid != Material::Air && liquid != Material::Water;
}

} // namespace

void Region::quench(Stepped& stepped) {
    const double thickness = world_->store().layers().thickness;
    const double volume = thickness * thickness * thickness;
    const globe::Properties& water = globe::properties(Material::Water);
    const double per_kilogram = (water.heat_capacity * (boiling_kelvin - air_kelvin)) + latent_heat;
    // Units of water a kilogram of it is, in a voxel's own scale.
    const double units_per_kilogram = full / (water.density * volume);

    std::vector<Site> lava;
    for (const auto& [key, brick] : held_) {
        for (int k = 0; k < globe::brick_voxels; ++k) {
            const Voxel& voxel = brick.voxels[static_cast<std::size_t>(k)];
            if (voxel.liquid_amount > 0 && molten(voxel.liquid)) {
                lava.push_back(globe::site_in(globe::brick_at_key(key), k));
            }
        }
    }
    for (const Site site : lava) {
        Voxel* here = voxel(site);
        std::vector<Voxel*> wet;
        std::vector<Site> around;
        around.push_back({.face = site.face, .i = site.i, .j = site.j, .layer = site.layer + 1});
        if (site.layer > 0) {
            around.push_back(
                {.face = site.face, .i = site.i, .j = site.j, .layer = site.layer - 1});
        }
        for (const auto& step : std::array<std::pair<int, int>, 4>{
                 {{-1, 0}, {1, 0}, {0, -1}, {0, 1}}}) {
            if (const std::optional<Site> next = beside(site, step.first, step.second)) {
                around.push_back(*next);
            }
        }
        std::int64_t available = 0;
        for (const Site next : around) {
            Voxel* there = voxel(next);
            if (there != nullptr && there->liquid == Material::Water && there->liquid_amount > 0) {
                wet.push_back(there);
                available += there->liquid_amount;
            }
        }
        if (wet.empty()) {
            continue;
        }
        // What the lava gives up cooling to where it stiffens, per unit of water it boils.
        const globe::Properties& rock = globe::properties(here->liquid);
        const double kelvin = globe::kelvin(here->temperature);
        const double stiff = rock.melts - 200.0;
        const double mass = rock.density * volume * here->liquid_amount / full;
        const double heat = mass * rock.heat_capacity * std::max(kelvin - stiff, 0.0);
        const auto needed = static_cast<std::int64_t>(std::ceil(heat / per_kilogram * units_per_kilogram));
        const std::int64_t used = std::min(needed, available);
        // The water is taken in proportion from what touches the lava.
        std::int64_t left = used;
        for (Voxel* there : wet) {
            const std::int64_t share =
                std::min<std::int64_t>({there->liquid_amount, left,
                                        (used * there->liquid_amount + available - 1) / available});
            there->liquid_amount = static_cast<std::uint16_t>(there->liquid_amount - share);
            if (there->liquid_amount == 0) {
                there->liquid = Material::Air;
            }
            left -= share;
        }
        stepped.steamed += used - left;
        if (used >= needed) {
            freeze(site, *held(globe::brick_of(site)));
            ++stepped.frozen;
        } else {
            const double cooled_to = kelvin - ((kelvin - stiff) * static_cast<double>(used) /
                                               static_cast<double>(needed));
            here->temperature = globe::temperature_in_eighths(cooled_to);
        }
        for (const Site next : around) {
            if (Held* brick = held(globe::brick_of(next))) {
                brick->dirty = true;
            }
        }
        held(globe::brick_of(site))->dirty = true;
    }
}

} // namespace hog::flow
