#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/plates.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tectonics.hpp>
#include <hog/planet/tile.hpp>

#include <cmath>
#include <numbers>
#include <vector>

namespace hog::planet {

namespace {

constexpr std::uint64_t supercontinent_salt = 0x7375'7065'7200'0000ULL; // "super"
constexpr std::uint64_t coast_salt = 0x636F'6173'7400'0000ULL;          // "coast"
constexpr std::uint64_t floor_salt = 0x666C'6F6F'7200'0000ULL;          // "floor"
// The supercontinent's coast wanders by so much of its reach either way, over features so many
// to the planet's width.
constexpr double coast_wander = 0.35;
constexpr double coast_frequency = 3.0;
// The ocean floor at the start is up to so many steps old.
constexpr float oldest_floor = 30.0F;
// Continents at the start are older than any step.
constexpr float craton_age = 1'000.0F;

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
    : grid_{settings.level, radius} {
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
    for (const Plates::Plate& plate : start.plates()) {
        const double speed = glm::length(plate.spin);
        plates_.push_back({.pole = glm::normalize(plate.spin),
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
        const bool continental =
            glm::dot(at, middle) >= reach + (wander * coast_wander * (1.0 - std::abs(reach)));
        const double floor =
            0.5 + (0.5 * fixed_to_double(fbm(hash_values(seed, floor_salt),
                                             noise_point(at, coast_frequency), 3)));
        columns_[cell] = {.plate = start.plate()[cell],
                          .crust = continental ? Crust::Continental : Crust::Oceanic,
                          .thickness = continental ? continental_thickness : oceanic_thickness,
                          .age =
                              continental ? craton_age : static_cast<float>(floor) * oldest_floor,
                          .raised = continental ? craton_age : 0.0F};
    }
}

void Tectonics::step() {
    std::vector<Column> next(columns_.size());
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
        next[cell] = top;
    }
    columns_ = std::move(next);
    ++steps_;
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
