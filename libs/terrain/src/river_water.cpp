#include <hog/planet/projection.hpp>
#include <hog/terrain/river_water.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <vector>

namespace hog::terrain {

namespace {

// Across a reach from bank to bank, a share of half its width, with the edges out over the banks
// filled in for each piece.
constexpr std::array<float, 5> across_water{-1.0F, -1.0F, 0.0F, 1.0F, 1.0F};
constexpr auto corners = static_cast<std::uint32_t>(across_water.size());

// Pieces to a meander, enough to follow its bends.
constexpr double pieces_per_turn = 8.0 / (2.0 * std::numbers::pi);

// A piece's end across a reach's course: where its middle truly lies, which way across it the
// channel is found from there, how far its meanders swing it, how wide it runs and the level of its
// water, if it runs in its channel or out over the water at its mouth, and how far mixed into that.
struct Section {
    DVec3 course;
    DVec3 side;
    planet::Swing swing;
    double half;
    DVec3 middle;
    std::optional<double> level;
    bool standing = false;
    double mixed = 0.0;
};

// Where a point `out` metres across a section from its middle truly lies.
DVec3 across_at(const planet::Meander& meander, const Section& section, double out, double radius) {
    // Across the swung river, so it keeps its width where it swings steeply.
    const double swung =
        section.swing.offset +
        (out / radius * std::sqrt(1.0 + (section.swing.slope * section.swing.slope)));
    return meander.unshifted(glm::normalize(section.course + (section.side * swung)));
}

// Whether the ground either side of a section, as far out as its water's drawn, lies under the
// water its reach runs into: coarse cells may hold land the sea or a lake covers.
bool out_in(const planet::Elevation& terrain, const planet::Reach& reach, const Section& section,
            double radius, double over) {
    const planet::Meander& meander = terrain.rivers().meander();
    return std::ranges::all_of(std::array{-1.0, 1.0}, [&](double way) {
        const DVec3 bank = across_at(meander, section, way * (section.half + over), radius);
        return terrain.elevation(planet::cell_at(bank)) < reach.to_surface;
    });
}

Section section_at(const planet::Elevation& terrain, const planet::Reach& reach, double t,
                   double radius, double over) {
    const DVec3 course = planet::course_at(reach, t);
    const DVec3 ahead = planet::course_at(reach, std::min(t + 0.01, 1.0)) -
                        planet::course_at(reach, std::max(t - 0.01, 0.0));
    const DVec3 side = glm::normalize(glm::cross(course, ahead));
    const planet::Swing swing = planet::swing_at(reach, t);
    Section section{.course = course,
                    .side = side,
                    .swing = swing,
                    .half = planet::width_at(reach, t) / 2.0,
                    .middle = terrain.rivers().meander().unshifted(
                        glm::normalize(course + (side * swing.offset))),
                    .level = std::nullopt};
    const planet::CellId cell = planet::cell_at(section.middle);
    const bool land = terrain.is_land(cell) && !terrain.lake(cell);
    if (reach.mouth && (!land || out_in(terrain, reach, section, radius, over))) {
        section.level = planet::fixed_to_double(reach.to_surface);
        section.standing = true;
    } else if (land) {
        if (const auto channel = terrain.river(cell)) {
            section.level = planet::fixed_to_double(channel->surface);
        }
    }
    return section;
}

// Past the shore a mouth's water mixes in, fanning out as it goes.
void mix_in(std::vector<Section>& sections, double width, double radius) {
    double past = 0.0;
    for (std::size_t at = 0; at < sections.size(); ++at) {
        Section& section = sections[at];
        if (!section.standing) {
            past = 0.0;
            continue;
        }
        const std::size_t before = at == 0 ? 1 : at - 1;
        past +=
            glm::length(section.middle - sections[std::min(before, sections.size() - 1)].middle) *
            radius;
        section.mixed = std::min(past / (river_mixing * width), 1.0);
        section.half *= 1.0 + (river_fan * section.mixed);
    }
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
    const double over = std::max(river_overlap, reach.width / 2.0);
    const double speed = river_speed(reach);
    const auto rough = static_cast<float>(river_rough(reach, radius));
    const int pieces = river_pieces(reach, radius, away);
    std::vector<Section> sections;
    sections.reserve(static_cast<std::size_t>(pieces) + 1);
    for (int piece = 0; piece <= pieces; ++piece) {
        sections.push_back(
            section_at(terrain, reach, static_cast<double>(piece) / pieces, radius, over));
    }
    mix_in(sections, reach.width, radius);
    const auto first = static_cast<std::uint32_t>(water.vertices.size());
    for (int piece = 0; piece <= pieces; ++piece) {
        const auto at = static_cast<std::size_t>(piece);
        const Section& section = sections[at];
        // Which way it truly runs, from the pieces' ends either side.
        const DVec3 downstream =
            glm::normalize(sections[std::min(at + 1, sections.size() - 1)].middle -
                           sections[at == 0 ? 0 : at - 1].middle);
        const double level = section.level.value_or(0.0);
        const double half = section.half;
        for (std::size_t corner = 0; corner < across_water.size(); ++corner) {
            const bool edge = corner == 0 || corner + 1 == across_water.size();
            const double out =
                static_cast<double>(across_water.at(corner)) * (half + (edge ? over : 0.0));
            const DVec3 point =
                corner == 2 ? section.middle : across_at(meander, section, out, radius);
            water.vertices.push_back(
                {.position = Vec3{(point * (radius + level)) - origin.metres()},
                 .beyond = static_cast<float>(std::abs(out) - half),
                 .flow = Vec3{downstream * speed},
                 .rough = rough,
                 .outward = corner == 2 ? Vec3{0.0F} : Vec3{glm::normalize(point - section.middle)},
                 .half = static_cast<float>(half),
                 .mixed = static_cast<float>(section.mixed)});
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

int river_pieces(const planet::Reach& reach, double radius, double away) {
    const double length = glm::length(reach.to - reach.from) * radius;
    const double turned = std::abs(reach.turn_from + reach.turn_to) / 2.0;
    const double finest = std::max(river_piece, (away - length) * river_piece_angle);
    return std::clamp(
        static_cast<int>(std::ceil(std::max(length / std::max(reach.width, finest),
                                            std::min(turned * pieces_per_turn, length / finest)))),
        2, 128);
}

double river_shore(const planet::Elevation& terrain, const planet::Reach& reach, double radius) {
    constexpr int steps = 16;
    const double over = std::max(river_overlap, reach.width / 2.0);
    for (int step = 0; step < steps; ++step) {
        const double t = static_cast<double>(step) / steps;
        if (section_at(terrain, reach, t, radius, over).standing) {
            return t;
        }
    }
    return 1.0;
}

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
