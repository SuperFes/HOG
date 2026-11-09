#include "formed.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace hog::globe {

namespace {

formation::Body body_for(const Planet& planet) {
    formation::Body body;
    body.seed = planet.seed;
    body.tilt = planet.tilt;
    return body;
}

// The cube face a direction falls on, and where on it, -1 to 1 both ways.
struct OnCube {
    std::size_t axis = 0;
    bool negative = false;
    double u = 0.0;
    double v = 0.0;
};

OnCube on_cube(DVec3 direction) noexcept {
    const DVec3 size = glm::abs(direction);
    std::size_t axis = 0;
    if (size.y > size.x && size.y >= size.z) {
        axis = 1;
    } else if (size.z > size.x && size.z > size.y) {
        axis = 2;
    }
    const double major = std::max(size[axis], 1e-300);
    return {.axis = axis,
            .negative = direction[axis] < 0.0,
            .u = direction[(axis + 1) % 3] / major,
            .v = direction[(axis + 2) % 3] / major};
}

} // namespace

Formed::Formed(const Planet& planet, const Derived& derived)
    : world_{body_for(planet), 5, static_cast<int>(planet.formed_detail)},
      smooth_{world_.sphere, 2.0}, radius_{planet.radius}, vertical_{derived.vertical} {
    const formation::World& world = world_;
    const std::uint32_t size = world.sphere.size();
    for (auto* field :
         {&top_, &organic_, &topsoil_, &subsoil_, &loess_, &sand_, &alluvium_, &lake_beds_, &till_,
          &ice_, &lava_, &rock_cover_, &light_, &lake_depth_, &lake_level_, &lake_share_}) {
        field->assign(size, 0.0);
    }
    for (std::uint32_t point = 0; point < size; ++point) {
        const bool land = world.rivers.receiver[point] != point;
        has_sea_ = has_sea_ || !land;
        // The ice, rock and deposits stand as tall against the planet as its relief does; the
        // soil and what lies on it keep their true depths.
        // The wind took off what rivers and ice laid, and laid sand and loess over it all.
        const double taken = std::min(world.drifts.deflated[point],
                                      world.sediment.alluvium[point] + world.ice.till[point]);
        const double blown = formation::layered(world.drifts.loess[point], world.body) +
                             formation::layered(world.drifts.sand[point], world.body);
        const double ground =
            land ? world.rivers.ground[point] + world.sediment.alluvium[point] - taken + blown
                 : world.rivers.height[point] + world.sediment.delta[point];
        top_[point] = (vertical_ * ground) + (land ? world.soil.organic[point] : 0.0);
        organic_[point] = world.soil.organic[point];
        topsoil_[point] = world.soil.topsoil[point];
        subsoil_[point] = std::max(0.0, world.soil.depth[point] - world.soil.topsoil[point]);
        loess_[point] = vertical_ * world.soil.loess[point];
        sand_[point] = vertical_ * world.soil.sand[point];
        alluvium_[point] = vertical_ * world.soil.alluvium[point];
        lake_beds_[point] = vertical_ * world.soil.lake_beds[point];
        till_[point] = vertical_ * world.soil.till[point];
        ice_[point] = land ? vertical_ * world.ice.thickness[point] : 0.0;
        lava_[point] = vertical_ * world.outcrop.lava[point];
        rock_cover_[point] = vertical_ * world.outcrop.cover[point];
        light_[point] = world.crust.light[point];
        const std::uint32_t lake = world.lakes.lake[point];
        if (land && lake != formation::Lakes::none && world.sediment.lake_depth[point] > 0.0) {
            lake_level_[point] = vertical_ * world.lakes.lakes[lake].level;
            lake_depth_[point] = std::max(0.0, lake_level_[point] - top_[point]);
            lake_share_[point] = 1.0;
        }
    }
    const auto [lowest, highest] = std::ranges::minmax(top_);
    lowest_ = lowest;
    highest_ = highest;

    const auto across = static_cast<double>(starts_across);
    starts_.resize(6 * starts_across * starts_across);
    std::uint32_t near = 0;
    for (std::size_t face = 0; face < 6; ++face) {
        const std::size_t axis = face / 2;
        for (std::size_t i = 0; i < starts_across; ++i) {
            for (std::size_t j = 0; j < starts_across; ++j) {
                DVec3 direction{0.0};
                direction[axis] = face % 2 == 0 ? 1.0 : -1.0;
                direction[(axis + 1) % 3] = ((2.0 * (static_cast<double>(i) + 0.5)) / across) - 1.0;
                direction[(axis + 2) % 3] = ((2.0 * (static_cast<double>(j) + 0.5)) / across) - 1.0;
                near = world.sphere.nearest(glm::normalize(direction), near);
                starts_[(((face * starts_across) + i) * starts_across) + j] = near;
            }
        }
    }
}

std::uint32_t Formed::start_near(DVec3 direction) const noexcept {
    const OnCube on = on_cube(direction);
    const auto across = static_cast<double>(starts_across);
    const auto cell = [&](double along) {
        return static_cast<std::size_t>(
            std::clamp((along + 1.0) * 0.5 * across, 0.0, across - 1.0));
    };
    const std::size_t face = (on.axis * 2) + (on.negative ? 1 : 0);
    return starts_[(((face * starts_across) + cell(on.u)) * starts_across) + cell(on.v)];
}

Formed::Here Formed::at(DVec3 direction) const {
    formation::Smooth::Weights weights;
    smooth_.weigh(direction, weights, start_near(direction));
    const auto value = [&](const std::vector<double>& field) {
        return formation::Smooth::read(field, weights).value;
    };
    const formation::Smooth::Read top = formation::Smooth::read(top_, weights);

    Here here;
    here.top = {.value = top.value, .gradient = top.gradient / radius_};
    const formation::World& world = world_;
    // A lake's water stands at its level, ending where the ground rises over it; where the ground
    // falls away instead, out of its outlet, it thins as its depth read like the ground's does,
    // rather than standing up as a wall where the lake's points end.
    if (const double share = value(lake_share_); share > 0.0) {
        const double level = value(lake_level_) / share;
        const double surface = std::min(level, top.value + value(lake_depth_));
        if (surface > top.value + 0.01) {
            here.lake = surface;
        }
    }
    here.light = value(light_);
    here.folding = value(world.bedrock.folding);
    Strata& layers = here.layers;
    layers.ice = value(ice_);
    layers.organic = value(organic_);
    layers.topsoil = value(topsoil_);
    layers.subsoil = value(subsoil_);
    layers.loess = value(loess_);
    layers.sand = value(sand_);
    layers.alluvium = value(alluvium_);
    layers.lake_beds = value(lake_beds_);
    layers.till = value(till_);
    layers.lava = value(lava_);
    layers.cover = layers.ice + layers.organic + layers.topsoil + layers.subsoil + layers.loess +
                   layers.sand + layers.alluvium + layers.lake_beds + layers.till + layers.lava +
                   value(rock_cover_);
    layers.basement = here.light >= 0.5 ? Material::Granite : Material::Basalt;
    return here;
}

} // namespace hog::globe
