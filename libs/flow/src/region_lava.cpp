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

std::vector<Site> Region::touching(Site site) const {
    std::vector<Site> sites;
    sites.push_back({.face = site.face, .i = site.i, .j = site.j, .layer = site.layer + 1});
    if (site.layer > 0) {
        sites.push_back({.face = site.face, .i = site.i, .j = site.j, .layer = site.layer - 1});
    }
    for (const auto& step :
         std::array<std::pair<int, int>, 4>{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}}) {
        if (const std::optional<Site> next = beside(site, step.first, step.second)) {
            sites.push_back(*next);
        }
    }
    return sites;
}

void Region::quench(Stepped& stepped) {
    const double thickness = world_->layers().thickness;
    const double volume = thickness * thickness * thickness;

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
        quench_at(site, volume, stepped);
    }
}

void Region::quench_at(Site site, double volume, Stepped& stepped) {
    Voxel* here = voxel(site);
    std::vector<Voxel*> wet;
    const std::vector<Site> sites = touching(site);
    std::int64_t available = 0;
    for (const Site next : sites) {
        Voxel* there = voxel(next);
        if (there != nullptr && there->liquid == Material::Water && there->liquid_amount > 0) {
            wet.push_back(there);
            available += there->liquid_amount;
        }
    }
    if (wet.empty()) {
        return;
    }
    const globe::Properties& water = globe::properties(Material::Water);
    const double per_kilogram = (water.heat_capacity * (boiling_kelvin - air_kelvin)) + latent_heat;
    // Units of water a kilogram of it is, in a voxel's own scale.
    const double units_per_kilogram = full / (water.density * volume);
    // What the lava gives up cooling to where it stiffens, per unit of water it boils.
    const globe::Properties& rock = globe::properties(here->liquid);
    const double kelvin = globe::kelvin(here->temperature);
    const double stiff = rock.melts - 200.0;
    const double mass = rock.density * volume * here->liquid_amount / full;
    const double heat = mass * rock.heat_capacity * std::max(kelvin - stiff, 0.0);
    const auto needed =
        static_cast<std::int64_t>(std::ceil(heat / per_kilogram * units_per_kilogram));
    const std::int64_t used = std::min(needed, available);
    // The water is taken in proportion from what touches the lava.
    std::int64_t left = used;
    for (Voxel* there : wet) {
        const auto share =
            std::min<std::int64_t>({there->liquid_amount, left,
                                    ((used * there->liquid_amount) + available - 1) / available});
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
        const double cooled_to =
            kelvin - ((kelvin - stiff) * static_cast<double>(used) / static_cast<double>(needed));
        here->temperature = globe::temperature_in_eighths(cooled_to);
    }
    for (const Site next : sites) {
        if (Held* brick = held(globe::brick_of(next))) {
            brick->dirty = true;
        }
    }
    if (Held* const brick = held(globe::brick_of(site))) {
        brick->dirty = true;
    }
}

int Region::open_faces(Site site) {
    int open = 0;
    const auto side = [&](const Voxel* there) {
        open += (there != nullptr && there->solid == Material::Air) ? 1 : 0;
    };
    side(voxel({.face = site.face, .i = site.i, .j = site.j, .layer = site.layer + 1}));
    if (site.layer > 0) {
        side(voxel({.face = site.face, .i = site.i, .j = site.j, .layer = site.layer - 1}));
    }
    for (const auto& step :
         std::array<std::pair<int, int>, 4>{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}}) {
        if (const std::optional<Site> next = beside(site, step.first, step.second)) {
            side(voxel(*next));
        }
    }
    return open;
}

void Region::cool_rock(double seconds) {
    constexpr double stretch = 10.0;
    cooling_ += seconds;
    if (cooling_ < stretch) {
        return;
    }
    const double elapsed = cooling_;
    cooling_ = 0.0;
    const double thickness = world_->layers().thickness;
    for (auto& [key, brick] : held_) {
        for (int k = 0; k < globe::brick_voxels; ++k) {
            Voxel& voxel = brick.voxels[static_cast<std::size_t>(k)];
            if (!red_hot(voxel) || (voxel.liquid_amount > 0 && molten(voxel.liquid))) {
                continue;
            }
            const Site site = globe::site_in(globe::brick_at_key(key), k);
            const double kelvin = globe::kelvin(voxel.temperature);
            const double next =
                cooled(voxel.solid, kelvin, air_kelvin, open_faces(site) / 6.0, thickness, elapsed);
            voxel.temperature = next < red_hot_kelvin
                                    ? globe::temperature_in_eighths(air_kelvin)
                                    : static_cast<std::uint16_t>(dithered(next * 8.0, site, 3));
            brick.dirty = true;
        }
    }
}

} // namespace hog::flow
