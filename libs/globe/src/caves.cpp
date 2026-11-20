#include "ground_cells.hpp"
#include "terms.hpp"

#include <hog/core/hash.hpp>
#include <hog/core/portable_maths.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>

namespace hog::globe::terms {
namespace {

// Passages run a few metres wide, wandering every few tens of metres, twice as far along as up
// or down: water dissolves its way along beds and toward the water table.
constexpr double tunnel_radius = 3.5;
constexpr double passage_step = 20.0;
constexpr double most_turn = 0.8; // radians a step, either way
constexpr double climb = 0.08;    // metres up or down a metre along, at most
constexpr int most_steps = 48;
// Narrower than this, or where the rock opens less than this, a passage ends.
constexpr double least_width = 0.3;
constexpr double least_open = 0.1;
// Passages begun in a cell where caves gather most, most of them in rock that opens fully, a few
// branching off another, a few at most deep.
constexpr double passages_per_cell = 12.0;
constexpr double branch_chance = 0.08;
constexpr int most_branching = 2;
// Chambers open where dissolution pooled along limestone's passages, rarely.
constexpr double chamber_chance = 0.06;
constexpr double least_chamber = 4.0;
constexpr double chamber_spread = 14.0;
static_assert(least_chamber + chamber_spread <= widest_cave);
static_assert(1.2 * tunnel_radius <= widest_cave);

// Cave cells about this wide, and passages keep within this many cell widths of their cell's
// middle, so the cells beside a point's hold every piece near it.
constexpr double cave_cell_width = 1'000.0;
constexpr double kept_within = 1.2;
// No cave cell is narrower than this: cave_level keeps them over three quarters of the width at
// the faces' middles, and the cube's tangent mapping narrows them by at most a fifth.
constexpr double least_cell = 0.6 * cave_cell_width;
// How near a point the pieces of the cells beside its own are all those that can come.
constexpr double spare = ((1.5 - kept_within) * least_cell) - widest_cave;
static_assert(spare > 0.0);
// The ground passages are laid along is read in cells about this wide.
constexpr double ground_detail = 100.0;

// Caves gather in districts a couple of kilometres across, where water found its way in.
constexpr double district_wavelength = 2'000.0;

// Lava tubes lie within tens of metres of the flows' tops.
constexpr double tube_depth = 30.0;
// What a tunnel's wall bears against the rock's strength: about two and a half times the weight
// of the rock over it, and the rock mass holds a fifth of what intact rock does, being jointed.
constexpr double wall_stress = 2.5;
constexpr double mass_strength = 0.2;

double unit(std::uint64_t value) {
    return static_cast<double>(value >> 11U) * 0x1.0p-53;
}

// The depth passages in `rock` are held open to, and none are by twice it: where the stress
// round them overcomes the rock.
double closure_depth(const Planet& planet, Material rock) {
    const Properties& properties = hog::globe::properties(rock);
    return mass_strength * properties.strength /
           (wall_stress * properties.density * planet.body.gravity);
}

// How readily each rock holds open passages.
double openness(Material rock, double depth, bool watered) {
    switch (rock) {
    case Material::Limestone:
        return watered ? 1.0 : 0.0;
    case Material::Shale:
    case Material::Sandstone:
        return watered ? 0.25 : 0.0;
    case Material::Basalt:
        // Lava tubes, on any world with flows.
        return 1.0 - smoothstep((depth - tube_depth) / tube_depth);
    case Material::Granite:
    case Material::Anorthosite:
        return 0.1;
    default:
        return 0.0;
    }
}

// How readily the rock at `radius` holds passages open, 0 to 1, and whether water pools in it
// into chambers.
struct Opening {
    double open = 0.0;
    bool chambers = false;
};

Opening opening_at(const Context& context, const CaveGround& under, double radius) {
    const double depth = under.ground - radius;
    if (depth < 0.0) {
        return {};
    }
    const bool watered = context.derived->liquid;
    const auto opens = [&](Material rock) {
        const double readily = openness(rock, depth, watered);
        if (readily <= 0.0) {
            return 0.0;
        }
        const double closure = closure_depth(*context.planet, rock);
        return readily * (1.0 - smoothstep((depth - closure) / closure));
    };
    const Layering& layers = under.layers;
    if (depth < layers.cover) {
        Beds beds{context, layers};
        const Material rock = bed_rock(context, beds.at(radius));
        return {.open = opens(rock), .chambers = rock == Material::Limestone && watered};
    }
    double open = opens(layers.basement);
    bool chambers = layers.basement == Material::Limestone;
    // Where two basements meet, either's caves open in both, the other's fading as less of it
    // shows.
    if (layers.basement_share > 0.0 && layers.basement_with != layers.basement) {
        open = std::max(open,
                        std::min(1.0, 2.0 * layers.basement_share) * opens(layers.basement_with));
        chambers = chambers || layers.basement_with == Material::Limestone;
    }
    return {.open = open, .chambers = chambers && watered};
}

// `heading` turned `angle` radians about `up`, anticlockwise seen from over it.
DVec3 turned(DVec3 heading, DVec3 up, double angle) {
    return glm::normalize((heading * maths::cos(angle)) +
                          (glm::cross(up, heading) * maths::sin(angle)));
}

// One cell's caves, laid out passage by passage.
class Layout {
public:
    Layout(const Context& context, Cell cell, const GroundUnder& under)
        : context_{&context}, under_{&under}, middle_{center(cell)},
          seed_{hash_values(context.seed(Term::CaveA), index_of(cell), cell.level)},
          width_{width_of(context, cell)}, district_{cave_district(context, middle_)},
          floor_{cave_floor(context)} {}

    std::vector<CavePiece> lay_out() {
        if (district_ <= 0.0) {
            return {};
        }
        const DVec3 pole = std::abs(middle_.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0};
        const DVec3 east = glm::normalize(glm::cross(pole, middle_));
        const DVec3 north = glm::cross(middle_, east);
        const double across = width_ / context_->planet->body.radius;
        const auto attempts =
            static_cast<std::uint64_t>((passages_per_cell * district_) + draw(0, 0));
        for (std::uint64_t attempt = 1; attempt <= attempts; ++attempt) {
            const std::uint64_t stream = hash_values(attempt);
            const DVec3 start =
                glm::normalize(middle_ + (east * ((draw(stream, 0) - 0.5) * across)) +
                               (north * ((draw(stream, 1) - 0.5) * across)));
            const CaveGround under = (*under_)(start);
            const double deep = draw(stream, 2);
            const double radius = under.ground - (3.0 + (floor_ * deep * deep));
            const Opening opening = opening_at(*context_, under, radius);
            // Most rock holds few passages open: fewer are begun in it.
            if (draw(stream, 3) >= opening.open) {
                continue;
            }
            const DVec3 heading = turned(east, start, 2.0 * std::numbers::pi * draw(stream, 4));
            constexpr int half_steps = most_steps / 2;
            const int steps =
                half_steps + static_cast<int>(draw(stream, 5) * static_cast<double>(half_steps));
            walk({.point = start * radius,
                  .heading = heading,
                  .opening = opening,
                  .steps = steps,
                  .branching = 0,
                  .stream = stream});
        }
        return std::move(pieces_);
    }

private:
    // A passage to follow: from where, which way, as open as the rock there holds it, for how many
    // stretches, how many times it's branched to get there, and the draws it makes.
    struct Passage {
        DVec3 point{0.0};
        DVec3 heading{0.0};
        Opening opening;
        int steps = 0;
        int branching = 0;
        std::uint64_t stream = 0;
    };

    [[nodiscard]] static double width_of(const Context& context, Cell cell) {
        const std::array<DVec3, 4> around = corners(cell);
        return context.planet->body.radius * glm::length(around[1] - around[0]);
    }

    [[nodiscard]] double draw(std::uint64_t stream, std::uint64_t k) const {
        return unit(hash_values(seed_, stream, k));
    }

    // The passage and every branch off it.
    void walk(const Passage& first) {
        std::vector<Passage> pending{first};
        while (!pending.empty()) {
            const Passage passage = pending.back();
            pending.pop_back();
            follow(passage, pending);
        }
    }

    // A passage on stretch by stretch, until the rock no longer holds it open, it breaks out of
    // the ground, or it strays from its cell; its branches into `branches`.
    void follow(Passage passage, std::vector<Passage>& branches) {
        auto& [point, heading, opening, steps, branching, stream] = passage;
        const double planet_radius = context_->planet->body.radius;
        for (int step = 0; step < steps; ++step) {
            const auto k = static_cast<std::uint64_t>(step) * 8U;
            const double width =
                tunnel_radius * opening.open * district_ * (0.8 + (0.4 * draw(stream, k)));
            if (width < least_width) {
                return;
            }
            const double radius = glm::length(point);
            const DVec3 up = point / radius;
            heading = turned(heading, up, (draw(stream, k + 1) - 0.5) * 2.0 * most_turn);
            const double rise = (draw(stream, k + 2) - 0.5) * 2.0 * climb * passage_step;
            const DVec3 ahead = glm::normalize(up + (heading * (passage_step / radius)));
            if (planet_radius * glm::length(ahead - middle_) > kept_within * width_) {
                return;
            }
            const DVec3 next = ahead * (radius + rise);
            pieces_.push_back({.from = point, .to = next, .radius = width});
            heading = glm::normalize(heading - (glm::dot(heading, ahead) * ahead));

            const CaveGround under = (*under_)(ahead);
            const double depth = under.ground - (radius + rise);
            // Breaking out of the ground, it opens a way in.
            if (depth < -width || depth > floor_) {
                return;
            }
            opening = opening_at(*context_, under, radius + rise);
            if (opening.open < least_open) {
                return;
            }
            if (opening.chambers && draw(stream, k + 3) < chamber_chance * district_) {
                const double spread = draw(stream, k + 4);
                pieces_.push_back({.from = next,
                                   .to = next,
                                   .radius = (least_chamber + (chamber_spread * spread * spread)) *
                                             opening.open});
            }
            if (branching < most_branching && draw(stream, k + 5) < branch_chance) {
                const double side = draw(stream, k + 6) < 0.5 ? -1.0 : 1.0;
                branches.push_back(
                    {.point = next,
                     .heading = turned(heading, ahead, side * (0.6 + (0.8 * draw(stream, k + 7)))),
                     .opening = opening,
                     .steps = steps / 2,
                     .branching = branching + 1,
                     .stream = hash_values(stream, step)});
            }
            point = next;
        }
    }

    const Context* context_;
    const GroundUnder* under_;
    DVec3 middle_;
    std::uint64_t seed_;
    double width_ = 0.0;
    double district_ = 0.0;
    double floor_ = 0.0;
    std::vector<CavePiece> pieces_;
};

} // namespace

double cave_floor(const Context& context) noexcept {
    const Planet& planet = *context.planet;
    // Lava tubes, and those the rocks that can hold passages hold, as far as they do.
    double floor = 2.0 * tube_depth;
    for (const Material rock : {Material::Granite, Material::Anorthosite}) {
        floor = std::max(floor, 2.0 * closure_depth(planet, rock));
    }
    // Water opens the soft rocks only where it runs as water.
    if (context.derived->liquid) {
        for (const Material rock : {Material::Limestone, Material::Shale, Material::Sandstone}) {
            floor = std::max(floor, 2.0 * closure_depth(planet, rock));
        }
    }
    return floor;
}

double cave_district(const Context& context, DVec3 direction) noexcept {
    const Noise field =
        fbm(direction * context.planet->body.radius,
            {.wavelength = district_wavelength, .gain = 0.5, .finest = district_wavelength / 2.0},
            0.0, context.seed(Term::CaveDistricts));
    return smoothstep((field.value + 0.1) / 0.4);
}

int cave_level(const Planet& planet) noexcept {
    const double face = std::numbers::pi / 2.0 * planet.body.radius;
    int level = 0;
    while (level < Cell::max_level && std::ldexp(face, -(level + 1)) >= 0.75 * cave_cell_width) {
        ++level;
    }
    return level;
}

std::vector<CavePiece> lay_out_caves(const Context& context, Cell cell, const GroundUnder& under) {
    return Layout{context, cell, under}.lay_out();
}

std::shared_ptr<const std::vector<CavePiece>> caves_in(const Context& context, Cell cell) {
    if (context.caves != nullptr) {
        if (auto kept = context.caves->find(cell)) {
            return std::move(*kept);
        }
    }
    const int level = cell_level(*context.planet, ground_detail);
    const double lod = cell_lod(*context.planet, level);
    const GroundUnder under = [&](DVec3 direction) {
        const Ground found = ground(context, cell_under(direction, level), lod);
        return CaveGround{.ground = context.planet->body.radius + found.height,
                          .layers = found.layers};
    };
    auto made = std::make_shared<const std::vector<CavePiece>>(lay_out_caves(context, cell, under));
    if (context.caves != nullptr) {
        context.caves->keep(cell, made);
    }
    return made;
}

std::vector<CavePiece> caves_near(const Context& context, DVec3 direction, double reach) {
    const Cell cell = cell_under(direction, cave_level(*context.planet));
    // The cells beside hold all that come within `spare`; each ring further out, a cell further.
    const int rings = 1 + static_cast<int>(std::ceil(std::max(0.0, reach - spare) / least_cell));
    std::vector<Cell> cells;
    for (int di = -rings; di <= rings; ++di) {
        for (int dj = -rings; dj <= rings; ++dj) {
            if (const auto beside = next_to(cell, di, dj);
                beside && std::ranges::find(cells, *beside) == cells.end()) {
                cells.push_back(*beside);
            }
        }
    }
    std::vector<CavePiece> found;
    for (const Cell from : cells) {
        for (const CavePiece& piece : *caves_in(context, from)) {
            if (piece.along(direction, piece.radius + reach)) {
                found.push_back(piece);
            }
        }
    }
    return found;
}

} // namespace hog::globe::terms

namespace hog::globe {

double CavePiece::to_line(DVec3 point) const noexcept {
    const DVec3 line = to - from;
    const DVec3 off = point - from;
    const double length = glm::dot(line, line);
    const double t = length > 0.0 ? std::clamp(glm::dot(off, line) / length, 0.0, 1.0) : 0.0;
    return glm::length(off - (t * line));
}

std::optional<std::pair<double, double>> CavePiece::along(DVec3 direction,
                                                          double reach) const noexcept {
    const DVec3 middle = (from + to) * 0.5;
    const double half = (glm::length(to - from) * 0.5) + reach;
    const double t = glm::dot(middle, direction);
    const DVec3 off = middle - (t * direction);
    const double apart = glm::dot(off, off);
    if (apart > half * half) {
        return std::nullopt;
    }
    const double across = std::sqrt((half * half) - apart);
    return std::pair{t - across, t + across};
}

double shown_radius(double radius, double lod) noexcept {
    return radius * terms::smoothstep((radius / lod) - 1.0);
}

} // namespace hog::globe
