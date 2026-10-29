#include <hog/core/assert.hpp>
#include <hog/planet/lithosphere.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tectonics.hpp>
#include <hog/planet/volume.hpp>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace hog::planet {

namespace {

constexpr auto parted = std::numeric_limits<std::uint32_t>::max();

// Adds `crust` to the plate at each of `cells` of its own grid, none of which it has crust at yet.
void add_crust(Lithosphere::Plate& plate, std::vector<std::uint32_t> cells, Crust crust) {
    std::ranges::sort(cells);
    const auto [last, end] = std::ranges::unique(cells);
    cells.erase(last, end);
    std::vector<std::uint32_t> merged;
    std::vector<Crust> carried;
    merged.reserve(plate.cells.size() + cells.size());
    carried.reserve(merged.capacity());
    std::size_t old = 0;
    for (const std::uint32_t cell : cells) {
        for (; old < plate.cells.size() && plate.cells[old] < cell; ++old) {
            merged.push_back(plate.cells[old]);
            carried.push_back(plate.crust[old]);
        }
        merged.push_back(cell);
        carried.push_back(crust);
    }
    merged.insert(merged.end(), plate.cells.begin() + static_cast<std::ptrdiff_t>(old),
                  plate.cells.end());
    carried.insert(carried.end(), plate.crust.begin() + static_cast<std::ptrdiff_t>(old),
                   plate.crust.end());
    plate.cells = std::move(merged);
    plate.crust = std::move(carried);
}

} // namespace

bool stays_over(const Crust& one, const Crust& other) noexcept {
    if (one.type != other.type) {
        return one.type == CrustType::Continental;
    }
    if (one.type == CrustType::Oceanic) {
        return one.age < other.age;
    }
    return one.thickness > other.thickness;
}

Lithosphere::Lithosphere(LevelGrid grid, std::vector<Plate> plates)
    : grid_{std::move(grid)}, plates_{std::move(plates)} {
    for (const Plate& plate : plates_) {
        HOG_ASSERT(plate.cells.size() == plate.crust.size(), "one crust to each cell");
        HOG_ASSERT(std::ranges::is_sorted(plate.cells), "cells ascending");
        HOG_ASSERT(plate.cells.empty() || plate.cells.back() < grid_.size(), "cells of the grid");
    }
}

void Lithosphere::advance(double years) {
    const auto aged = static_cast<float>(years);
    for (Plate& plate : plates_) {
        plate.frame = glm::normalize(glm::angleAxis(plate.rate * years, plate.pole) * plate.frame);
        for (Crust& crust : plate.crust) {
            crust.age += aged;
            crust.raised += aged;
        }
    }
    time_ += years;
}

void Lithosphere::spread() {
    // Each parted place of the grid takes the plate of the nearest crust, out from the gap's
    // edges a ring of cells at a time.
    std::vector<std::uint32_t> owner(grid_.size(), parted);
    for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
        if (const std::optional<Found> found = top(cell_center(grid_.cell(cell)))) {
            owner[cell] = found->plate;
        }
    }
    std::vector<std::uint32_t> frontier;
    for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
        const auto around = grid_.neighbors(cell);
        if (owner[cell] != parted && std::ranges::any_of(around, [&](std::uint32_t next) {
                return owner[next] == parted;
            })) {
            frontier.push_back(cell);
        }
    }
    std::vector<std::vector<std::uint32_t>> fresh(plates_.size());
    while (!frontier.empty()) {
        std::vector<std::uint32_t> further;
        for (const std::uint32_t cell : frontier) {
            for (const std::uint32_t next : grid_.neighbors(cell)) {
                if (owner[next] != parted) {
                    continue;
                }
                owner[next] = owner[cell];
                const DVec3 own = to_plate(owner[next], cell_center(grid_.cell(next)));
                fresh[owner[next]].push_back(grid_.index(cell_at(own, grid_.level())));
                further.push_back(next);
            }
        }
        frontier = std::move(further);
    }
    for (std::uint32_t plate = 0; plate < plates_.size(); ++plate) {
        add_crust(plates_[plate], std::move(fresh[plate]), Crust{});
    }
}

DVec3 Lithosphere::to_planet(std::uint32_t plate, DVec3 own) const {
    return plates_[plate].frame * own;
}

DVec3 Lithosphere::to_plate(std::uint32_t plate, DVec3 at) const {
    return glm::conjugate(plates_[plate].frame) * at;
}

std::optional<Crust> Lithosphere::crust_of(std::uint32_t plate, DVec3 at) const {
    const Plate& carrier = plates_[plate];
    const std::uint32_t cell = grid_.index(cell_at(to_plate(plate, at), grid_.level()));
    const auto found = std::ranges::lower_bound(carrier.cells, cell);
    if (found == carrier.cells.end() || *found != cell) {
        return std::nullopt;
    }
    return carrier.crust[static_cast<std::size_t>(std::distance(carrier.cells.begin(), found))];
}

std::optional<Lithosphere::Found> Lithosphere::top(DVec3 at) const {
    std::optional<Found> top;
    for (std::uint32_t plate = 0; plate < plates_.size(); ++plate) {
        const std::optional<Crust> crust = crust_of(plate, at);
        if (crust && (!top || stays_over(*crust, top->crust))) {
            top = Found{.plate = plate, .crust = *crust};
        }
    }
    return top;
}

Lithosphere lithosphere_from(const Tectonics& tectonics, double step_years) {
    const auto step = static_cast<float>(step_years);
    std::vector<Lithosphere::Plate> plates;
    plates.reserve(tectonics.plates().size());
    for (const Tectonics::Plate& plate : tectonics.plates()) {
        plates.push_back({.frame = DQuat{1.0, 0.0, 0.0, 0.0},
                          .pole = plate.pole,
                          .rate = plate.turn / step_years,
                          .cells = {},
                          .crust = {}});
    }
    const std::span<const Tectonics::Column> columns = tectonics.columns();
    for (std::uint32_t cell = 0; cell < columns.size(); ++cell) {
        const Tectonics::Column& column = columns[cell];
        Lithosphere::Plate& plate = plates[column.plate];
        plate.cells.push_back(cell);
        plate.crust.push_back({.type = column.crust == Tectonics::Crust::Continental
                                           ? CrustType::Continental
                                           : CrustType::Oceanic,
                               .thickness = column.thickness,
                               .age = column.age * step,
                               .raised = column.raised * step});
    }
    return Lithosphere{tectonics.grid(), std::move(plates)};
}

void fill_lithosphere(Volume& volume, const Lithosphere& lithosphere, double radius, int level,
                      int shell_level) {
    const auto filled = [](Material material) {
        Contents contents;
        contents.material = material;
        return contents;
    };
    const double bottom = radius - (volume_span / 2.0);
    const std::uint32_t across = 1U << static_cast<unsigned>(level);
    for (int face = 0; face < face_count; ++face) {
        for (std::uint32_t i = 0; i < across; ++i) {
            for (std::uint32_t j = 0; j < across; ++j) {
                const CellId column = CellId::from_face_ij(face, i, j, level);
                const std::optional<Lithosphere::Found> found =
                    lithosphere.top(cell_center(column));
                if (!found) {
                    fill_between(volume, radius, column, bottom, radius, shell_level,
                                 filled(Material::Mantle));
                    continue;
                }
                const Crust& crust = found->crust;
                const double base = radius - (static_cast<double>(crust.thickness) * 1'000.0);
                fill_between(volume, radius, column, bottom, base, shell_level,
                             filled(Material::Mantle));
                fill_between(volume, radius, column, base, radius, shell_level,
                             filled(crust.type == CrustType::Continental ? Material::Granite
                                                                         : Material::Basalt));
            }
        }
    }
}

} // namespace hog::planet
