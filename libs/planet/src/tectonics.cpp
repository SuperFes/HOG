#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/plates.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tectonics.hpp>
#include <hog/planet/tile.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>
#include <vector>

namespace hog::planet {

namespace {

constexpr std::uint64_t supercontinent_salt = 0x7375'7065'7200'0000ULL; // "super"
constexpr std::uint64_t coast_salt = 0x636F'6173'7400'0000ULL;          // "coast"
constexpr std::uint64_t floor_salt = 0x666C'6F6F'7200'0000ULL;          // "floor"
constexpr std::uint64_t pole_salt = 0x706F'6C65'0000'0000ULL;           // "pole"
constexpr std::uint64_t craton_salt = 0x6372'6174'6F6E'0000ULL;         // "craton"
// The supercontinent's coast wanders by so much of its reach either way, over features so many
// to the planet's width.
constexpr double coast_wander = 0.35;
constexpr double coast_frequency = 3.0;
// The ocean floor at the start is up to so many steps old: about as old on the whole as it stays
// once the plates have been moving a while, so the sea doesn't start out shallow.
constexpr float oldest_floor = 60.0F;
// Continents at the start are older than any step.
constexpr float craton_age = 1'000.0F;
// Continental crust thins toward the coast to so many kilometres, over so much of the
// supercontinent's reach, and varies inland by so many either way.
constexpr double margin_thickness = 24.0;
constexpr double margin_width = 0.3;
constexpr double craton_relief = 2.0;

// A uniform double in [0, 1) from the hash's high bits.
[[nodiscard]] double unit(std::uint64_t hash) {
    return static_cast<double>(hash >> 11U) * 0x1p-53;
}

// A unit vector uniform over the sphere, by rejection.
[[nodiscard]] DVec3 direction(std::uint64_t seed) {
    for (std::uint64_t attempt = 0;; ++attempt) {
        const DVec3 point{(2.0 * unit(hash_values(seed, attempt, 0U))) - 1.0,
                          (2.0 * unit(hash_values(seed, attempt, 1U))) - 1.0,
                          (2.0 * unit(hash_values(seed, attempt, 2U))) - 1.0};
        const double length = glm::length(point);
        if (length <= 1.0 && length > 0.01) {
            return point / length;
        }
    }
}

// `at` turned by `angle` radians about the unit `axis`.
[[nodiscard]] DVec3 turned(DVec3 at, DVec3 axis, double angle) {
    const double cosine = std::cos(angle);
    return (at * cosine) + (glm::cross(axis, at) * std::sin(angle)) +
           (axis * (glm::dot(axis, at) * (1.0 - cosine)));
}

// Of two columns meeting in a cell, the one left on top: continents float over ocean floor,
// young warm ocean floor over old cold floor, and the thicker of two continents over the thinner.
[[nodiscard]] bool stays_over(const Tectonics::Column& one, const Tectonics::Column& other) {
    if (one.crust != other.crust) {
        return one.crust == Tectonics::Crust::Continental;
    }
    if (one.crust == Tectonics::Crust::Oceanic) {
        return one.age < other.age;
    }
    return one.thickness > other.thickness;
}

} // namespace

Tectonics::Tectonics(std::uint64_t seed, double radius, Settings settings)
    : settings_{settings}, grid_{settings.level, radius} {
    HOG_ASSERT(settings.continental > 0.0 && settings.continental < 1.0,
               "continental share out of range");
    const Plates start{seed, grid_, radius, settings.plates};
    // Steps long enough for the average plate to move `travel` cells.
    double mean_speed = 0.0;
    for (const Plates::Plate& plate : start.plates()) {
        mean_speed += glm::length(plate.spin);
    }
    mean_speed /= static_cast<double>(start.plates().size());
    const double cell_angle = std::sqrt(4.0 * std::numbers::pi / static_cast<double>(grid_.size()));
    // Each turns about a pole a quarter turn from its middle, so it slides across the planet
    // rather than spinning on the spot.
    std::vector<DVec3> middles(start.plates().size(), DVec3{0.0});
    for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
        middles[start.plate()[cell]] += cell_center(grid_.cell(cell));
    }
    for (std::uint32_t plate = 0; plate < start.plates().size(); ++plate) {
        const double speed = glm::length(start.plates()[plate].spin);
        const DVec3 across =
            glm::cross(middles[plate], direction(hash_values(seed, pole_salt, plate)));
        plates_.push_back({.pole = glm::length(across) > 0.0
                                       ? glm::normalize(across)
                                       : glm::normalize(start.plates()[plate].spin),
                           .turn = speed / mean_speed * settings.travel * cell_angle});
    }

    // One supercontinent, its coast wandering, as the ocean floor's age varies around it.
    const DVec3 middle = direction(hash_values(seed, supercontinent_salt));
    const double reach = 1.0 - (2.0 * settings.continental);
    columns_.resize(grid_.size());
    for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
        const DVec3 at = cell_center(grid_.cell(cell));
        const double wander = fixed_to_double(
            fbm(hash_values(seed, coast_salt), noise_point(at, coast_frequency), 4));
        const double inland =
            glm::dot(at, middle) - reach - (wander * coast_wander * (1.0 - std::abs(reach)));
        const bool continental = inland >= 0.0;
        const double settled =
            static_cast<double>(continental_thickness) +
            (craton_relief * fixed_to_double(fbm(hash_values(seed, craton_salt),
                                                 noise_point(at, coast_frequency), 3)));
        const double rise = std::min(inland / (margin_width * (1.0 - reach)), 1.0);
        const double thickness =
            margin_thickness + ((settled - margin_thickness) * rise * rise * (3.0 - (2.0 * rise)));
        const double floor =
            0.5 + (0.5 * fixed_to_double(fbm(hash_values(seed, floor_salt),
                                             noise_point(at, coast_frequency), 3)));
        columns_[cell] = {
            .plate = start.plate()[cell],
            .crust = continental ? Crust::Continental : Crust::Oceanic,
            .thickness = continental ? static_cast<float>(thickness) : oceanic_thickness,
            .age = continental ? craton_age : static_cast<float>(floor) * oldest_floor,
            .raised = continental ? craton_age : 0.0F};
    }
}

Tectonics::Tectonics(LevelGrid grid, std::vector<Plate> plates, std::vector<Column> columns,
                     Settings settings)
    : settings_{settings}, grid_{std::move(grid)}, plates_{std::move(plates)},
      columns_{std::move(columns)} {
    HOG_ASSERT(columns_.size() == grid_.size(), "one column to each cell");
}

void Tectonics::step() {
    const std::vector<bool> diving = move();
    keep_whole();
    raise_arcs(diving);
    spread_and_wear();
    ++steps_;
}

std::vector<bool> Tectonics::move() {
    std::vector<Column> next(columns_.size());
    std::vector<bool> diving(columns_.size(), false);
    for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
        const DVec3 at = cell_center(grid_.cell(cell));
        // The crust each plate carries here: whatever of it lay a step back along its turn.
        bool found = false;
        bool piled = false;
        Column top{};
        for (std::uint32_t plate = 0; plate < plates_.size(); ++plate) {
            const DVec3 from = turned(at, plates_[plate].pole, -plates_[plate].turn);
            const Column& carried = columns_[grid_.index(cell_at(from, grid_.level()))];
            if (carried.plate != plate) {
                continue;
            }
            if (!found) {
                top = carried;
                found = true;
                continue;
            }
            // Continents meeting pile up into one thicker column; whatever else meets them dives
            // under.
            const bool collide =
                top.crust == Crust::Continental && carried.crust == Crust::Continental;
            Column over = stays_over(carried, top) ? carried : top;
            if (collide) {
                over.thickness = top.thickness + carried.thickness;
                piled = true;
            } else {
                diving[cell] = true;
            }
            top = over;
        }
        if (!found) {
            // Where plates part, new ocean floor wells up, joining the plate that was here.
            top = {.plate = columns_[cell].plate,
                   .crust = Crust::Oceanic,
                   .thickness = oceanic_thickness,
                   .age = 0.0F,
                   .raised = 0.0F};
        } else {
            top.age += 1.0F;
            top.raised = piled ? 0.0F : top.raised + 1.0F;
        }
        top.arc = 0.0F;
        next[cell] = top;
    }
    columns_ = std::move(next);
    return diving;
}

void Tectonics::keep_whole() {
    std::vector<std::uint32_t> plates(columns_.size());
    for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
        plates[cell] = columns_[cell].plate;
        const auto around = grid_.neighbors(cell);
        if (std::ranges::any_of(around, [&](std::uint32_t next) {
                return columns_[next].plate == columns_[cell].plate;
            })) {
            continue;
        }
        // Alone: it joins whichever plate holds most of its neighbours.
        std::uint32_t most = 0;
        for (const std::uint32_t next : around) {
            const auto count =
                static_cast<std::uint32_t>(std::ranges::count_if(around, [&](std::uint32_t other) {
                    return columns_[other].plate == columns_[next].plate;
                }));
            if (count > most) {
                most = count;
                plates[cell] = columns_[next].plate;
            }
        }
    }
    for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
        columns_[cell].plate = plates[cell];
    }
}

void Tectonics::raise_arcs(const std::vector<bool>& diving) {
    // Out from each trench across the plate over it, cell by cell, as far as the arc reaches.
    std::vector<int> reached(columns_.size(), -1);
    std::vector<std::uint32_t> frontier;
    for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
        if (diving[cell]) {
            reached[cell] = 0;
            frontier.push_back(cell);
        }
    }
    for (int out = 1; out <= settings_.arc_reach && !frontier.empty(); ++out) {
        std::vector<std::uint32_t> further;
        for (const std::uint32_t cell : frontier) {
            for (const std::uint32_t next : grid_.neighbors(cell)) {
                if (reached[next] < 0 && columns_[next].plate == columns_[cell].plate) {
                    reached[next] = out;
                    further.push_back(next);
                }
            }
        }
        frontier = std::move(further);
    }
    for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
        if (reached[cell] <= 0) {
            continue;
        }
        Column& column = columns_[cell];
        column.thickness += settings_.arc_growth;
        column.arc = settings_.arc_growth;
        column.raised = 0.0F;
        if (column.crust == Crust::Oceanic && column.thickness >= arc_continent) {
            column.crust = Crust::Continental;
        }
    }
}

void Tectonics::spread_and_wear() {
    // Crust piled up over what it settles to spreads into its continental neighbours, volume for
    // volume, so a range piled up along a single line of cells widens into a belt. Crust thinned
    // under that, along a margin, holds its shape.
    const auto piled = [&](std::uint32_t cell) {
        return std::max(columns_[cell].thickness - continental_thickness, 0.0F);
    };
    std::vector<float> thickness(columns_.size());
    for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
        thickness[cell] = columns_[cell].thickness;
    }
    for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
        if (columns_[cell].crust != Crust::Continental) {
            continue;
        }
        const auto around = grid_.neighbors(cell);
        const auto share = settings_.spreading / static_cast<float>(around.size());
        for (const std::uint32_t next : around) {
            if (next < cell || columns_[next].crust != Crust::Continental) {
                continue;
            }
            const auto here = static_cast<float>(grid_.area(cell));
            const auto there = static_cast<float>(grid_.area(next));
            const float moved = share * (piled(cell) - piled(next)) * std::min(here, there);
            thickness[cell] -= moved / here;
            thickness[next] += moved / there;
        }
    }
    for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
        Column& column = columns_[cell];
        column.thickness = thickness[cell];
        if (column.crust == Crust::Continental && column.thickness > continental_thickness) {
            column.thickness -= (column.thickness - continental_thickness) * settings_.wearing;
        }
    }
}

double Tectonics::continental_volume() const {
    double volume = 0.0;
    for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
        if (columns_[cell].crust == Crust::Continental) {
            volume += static_cast<double>(grid_.area(cell)) * 1e-6 *
                      static_cast<double>(columns_[cell].thickness);
        }
    }
    return volume;
}

} // namespace hog::planet
