#include <hog/planet/projection.hpp>
#include <hog/terrain/river_water.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace hog::terrain {

namespace {

// Across a reach from bank to bank, as RiverVertex::across has it, with the edges out over the
// banks filled in for each piece.
constexpr std::array<float, 5> across_water{-1.0F, -1.0F, 0.0F, 1.0F, 1.0F};
constexpr auto corners = static_cast<std::uint32_t>(across_water.size());

// A piece's end across a reach's course: where its middle truly lies, which way across it the
// channel is found from there, and the level of its water, if it runs over land, not into a lake
// or the sea.
struct Section {
    DVec3 course;
    DVec3 side;
    DVec3 middle;
    std::optional<double> level;
};

Section section_at(const planet::Elevation& terrain, const planet::Reach& reach, double t) {
    const DVec3 course = planet::course_at(reach, t);
    const DVec3 ahead = planet::course_at(reach, std::min(t + 0.01, 1.0)) -
                        planet::course_at(reach, std::max(t - 0.01, 0.0));
    Section section{.course = course,
                    .side = glm::normalize(glm::cross(course, ahead)),
                    .middle = terrain.rivers().meander().unshifted(course),
                    .level = std::nullopt};
    const planet::CellId cell = planet::cell_at(section.middle);
    if (terrain.is_land(cell) && !terrain.lake(cell)) {
        if (const auto channel = terrain.river(cell)) {
            section.level = planet::fixed_to_double(channel->surface);
        }
    }
    return section;
}

// Lays out one reach's surface, if it runs within reach of `around`.
void lay_reach(const planet::Elevation& terrain, const planet::Reach& reach, DVec3 around,
               double radius, const WorldPosition& origin, RiverWater& water) {
    const double length = glm::length(reach.to - reach.from) * radius;
    // Its course keeps within a length of its ends.
    const double away = glm::length(((reach.from + reach.to) * 0.5) - around) * radius;
    if (away > river_reach + length) {
        return;
    }
    const planet::Meander& meander = terrain.rivers().meander();
    const double half = reach.width / 2.0;
    const double over = std::max(river_overlap, half);
    const double speed = river_speed(reach);
    const auto rough = static_cast<float>(river_rough(reach, radius));
    const int pieces =
        std::clamp(static_cast<int>(std::ceil(length / std::max(reach.width, river_piece))), 2, 64);
    std::vector<Section> sections;
    sections.reserve(static_cast<std::size_t>(pieces) + 1);
    for (int piece = 0; piece <= pieces; ++piece) {
        sections.push_back(section_at(terrain, reach, static_cast<double>(piece) / pieces));
    }
    const auto first = static_cast<std::uint32_t>(water.vertices.size());
    for (int piece = 0; piece <= pieces; ++piece) {
        const auto at = static_cast<std::size_t>(piece);
        const Section& section = sections[at];
        // Which way it truly runs, from the pieces' ends either side.
        const DVec3 downstream =
            glm::normalize(sections[std::min(at + 1, sections.size() - 1)].middle -
                           sections[at == 0 ? 0 : at - 1].middle);
        const double level = section.level.value_or(0.0);
        for (std::size_t corner = 0; corner < across_water.size(); ++corner) {
            const bool edge = corner == 0 || corner + 1 == across_water.size();
            const double out =
                static_cast<double>(across_water.at(corner)) * (half + (edge ? over : 0.0));
            const DVec3 point =
                corner == 2 ? section.middle
                            : meander.unshifted(
                                  glm::normalize(section.course + (section.side * (out / radius))));
            water.vertices.push_back(
                {.position = Vec3{(point * (radius + level)) - origin.metres()},
                 .across = static_cast<float>(out / half),
                 .flow = Vec3{downstream * speed},
                 .rough = rough});
        }
    }
    for (int piece = 0; piece < pieces; ++piece) {
        const auto at = static_cast<std::size_t>(piece);
        if (!sections[at].level || !sections[at + 1].level) {
            continue;
        }
        const std::uint32_t row = first + (static_cast<std::uint32_t>(piece) * corners);
        for (std::uint32_t corner = 0; corner + 1 < corners; ++corner) {
            const std::uint32_t here = row + corner;
            const std::uint32_t next = here + corners;
            water.indices.insert(water.indices.end(),
                                 {here, next, here + 1, here + 1, next, next + 1});
        }
    }
}

} // namespace

double river_speed(const planet::Reach& reach) {
    const double depth = planet::fixed_to_double(reach.depth);
    return reach.width > 0.0 && depth > 0.0 ? reach.flow / (reach.width * depth) : 0.0;
}

double river_rough(const planet::Reach& reach, double radius) {
    const double run = glm::length(reach.to - reach.from);
    if (run <= 0.0) {
        return 0.0;
    }
    // As planet::Rivers tells falls.
    const double drop = planet::fixed_to_double(reach.from_surface - reach.to_surface);
    return std::clamp(drop / (run * radius * planet::Rivers::falls_gradient), 0.0, 1.0);
}

RiverWater river_water(const planet::World& world, DVec3 around, double radius) {
    const planet::Elevation& terrain = world.geology().terrain();
    RiverWater water{
        .origin = WorldPosition{glm::round(around * (radius / river_pattern)) * river_pattern},
        .vertices = {},
        .indices = {}};
    for (const planet::Reach& reach : terrain.rivers().reaches()) {
        lay_reach(terrain, reach, around, radius, water.origin, water);
    }
    return water;
}

void RiverStream::follow(DVec3 eye) {
    if (!wanted_ || glm::length(eye - *wanted_) * radius_ > river_relay) {
        wanted_ = eye;
    }
    if (!working_ && wanted_ != started_) {
        start(*wanted_);
    }
}

void RiverStream::start(DVec3 around) {
    started_ = around;
    working_ = std::make_unique<jobs::Async<RiverWater>>(
        *jobs_,
        [world = world_, around, radius = radius_] { return river_water(*world, around, radius); });
}

std::optional<RiverWater> RiverStream::collect() {
    if (!working_ || !working_->done()) {
        return std::nullopt;
    }
    RiverWater found = working_->take();
    working_.reset();
    if (wanted_ && wanted_ != started_) {
        start(*wanted_);
    }
    return found;
}

std::optional<RiverWater> RiverStream::finish() {
    std::optional<RiverWater> last;
    while (working_) {
        last = working_->take();
        working_.reset();
        if (wanted_ && wanted_ != started_) {
            start(*wanted_);
        }
    }
    return last;
}

} // namespace hog::terrain
