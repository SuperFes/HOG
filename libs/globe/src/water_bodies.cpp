#include "formed.hpp"

#include <hog/globe/grid_fill.hpp>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace hog::globe {

grid::WaterBodies water_bodies(const Generator& generator) {
    grid::WaterBodies table;
    const double radius = generator.planet().body.radius;
    if (const std::optional<double> sea = generator.sea_level()) {
        table.bodies.emplace(sea_body, grid::Sea{.level = *sea});
    }
    const Formed& formed = *generator.shape().formed;
    const std::vector<Formed::LakeAt> lakes = formed.lakes();
    for (std::uint32_t lake = 0; lake < lakes.size(); ++lake) {
        table.bodies.emplace(body_value({.kind = WaterBody::Kind::Lake, .index = lake}),
                             grid::Lake{.level = radius + lakes[lake].level,
                                        .season = {.rise = lakes[lake].season.rise,
                                                   .peak = lakes[lake].season.peak}});
    }
    const double lod = voxel_lod(generator);
    const formation::World& world = formed.world();
    const auto points = static_cast<std::uint32_t>(world.sphere.size());
    for (std::uint32_t stem = 0; stem < points; ++stem) {
        const std::vector<Formed::Turn> turns = formed.river_course(stem, lod);
        if (turns.size() < 2) {
            continue;
        }
        grid::River river{.course = {}, .peak = world.regimes.peak[stem]};
        river.course.reserve(turns.size());
        for (const Formed::Turn& turn : turns) {
            const double level = radius + turn.level;
            const DVec3 place = turn.direction * level;
            // Turns closer than a millimetre are one.
            if (!river.course.empty() && glm::distance(river.course.back().place, place) < 1e-3) {
                continue;
            }
            river.course.push_back(
                {.place = place, .level = level, .flow = turn.flow, .rise = turn.rise});
        }
        if (river.course.size() >= 2) {
            table.bodies.emplace(body_value({.kind = WaterBody::Kind::River, .index = stem}),
                                 std::move(river));
        }
    }
    return table;
}

} // namespace hog::globe
