#include <hog/core/assert.hpp>
#include <hog/planet/lithosphere.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tectonics.hpp>
#include <hog/planet/volume.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
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

// Drops the crust at each of `held`, indices into the plate's cells.
void remove_crust(Lithosphere::Plate& plate, std::vector<std::size_t> held) {
    std::ranges::sort(held);
    std::size_t kept = 0;
    std::size_t next = 0;
    for (std::size_t index = 0; index < plate.cells.size(); ++index) {
        if (next < held.size() && held[next] == index) {
            ++next;
            continue;
        }
        plate.cells[kept] = plate.cells[index];
        plate.crust[kept] = plate.crust[index];
        ++kept;
    }
    plate.cells.resize(kept);
    plate.crust.resize(kept);
}

// The cells next to the plate's crust at `index` on its own grid that carry continent thinner
// than it, as indices into its cells; says how many.
std::size_t thinner_continent(const Lithosphere::Plate& plate, const LevelGrid& grid,
                              std::size_t index, std::array<std::size_t, 8>& lower) {
    std::size_t count = 0;
    for (const std::uint32_t next : grid.neighbors(plate.cells[index])) {
        const auto found = std::ranges::lower_bound(plate.cells, next);
        if (found == plate.cells.end() || *found != next || count == lower.size()) {
            continue;
        }
        const auto at = static_cast<std::size_t>(std::distance(plate.cells.begin(), found));
        if (plate.crust[at].type == CrustType::Continental &&
            plate.crust[at].thickness < plate.crust[index].thickness) {
            lower.at(count++) = at;
        }
    }
    return count;
}

// Continental crust over `strongest` kilometres sheds what it can't hold up into the plate's
// thinner continent around it, volume for volume, and that passes on what then overloads it.
void collapse(Lithosphere::Plate& plate, const LevelGrid& grid, float strongest) {
    constexpr int most_passes = 256;
    const auto over = [&](std::size_t index) {
        return plate.crust[index].type == CrustType::Continental &&
               plate.crust[index].thickness > strongest;
    };
    std::vector<std::size_t> loaded;
    for (std::size_t index = 0; index < plate.crust.size(); ++index) {
        if (over(index)) {
            loaded.push_back(index);
        }
    }
    std::vector<float> flowed(plate.crust.size(), 0.0F);
    for (int pass = 0; pass < most_passes && !loaded.empty(); ++pass) {
        std::vector<std::size_t> reached;
        for (const std::size_t index : loaded) {
            std::array<std::size_t, 8> lower{};
            const std::size_t count = thinner_continent(plate, grid, index, lower);
            if (count == 0) {
                continue;
            }
            const auto here = static_cast<float>(grid.area(plate.cells[index]));
            const float excess = plate.crust[index].thickness - strongest;
            flowed[index] -= excess;
            reached.push_back(index);
            const float each = excess * here / static_cast<float>(count);
            for (std::size_t next = 0; next < count; ++next) {
                const std::size_t to = lower.at(next);
                flowed[to] += each / static_cast<float>(grid.area(plate.cells[to]));
                reached.push_back(to);
            }
        }
        std::ranges::sort(reached);
        const auto [last, end] = std::ranges::unique(reached);
        reached.erase(last, end);
        loaded.clear();
        for (const std::size_t index : reached) {
            plate.crust[index].thickness += flowed[index];
            flowed[index] = 0.0F;
            if (over(index)) {
                loaded.push_back(index);
            }
        }
    }
}

// Moves what continental crust has piled over `settled` kilometres into the plate's continent
// around it, by `share` of the difference with each, volume for volume.
void flow(Lithosphere::Plate& plate, const LevelGrid& grid, float settled, float share) {
    const auto piled = [&](std::size_t index) {
        return std::max(plate.crust[index].thickness - settled, 0.0F);
    };
    std::vector<float> thickness(plate.crust.size());
    for (std::size_t index = 0; index < plate.crust.size(); ++index) {
        thickness[index] = plate.crust[index].thickness;
    }
    for (std::size_t index = 0; index < plate.crust.size(); ++index) {
        if (plate.crust[index].type != CrustType::Continental) {
            continue;
        }
        const auto around = grid.neighbors(plate.cells[index]);
        const float each = share / static_cast<float>(around.size());
        const auto here = static_cast<float>(grid.area(plate.cells[index]));
        // Each pair once, from the lower cell.
        for (const std::uint32_t next : around) {
            const auto found = std::ranges::lower_bound(plate.cells, next);
            if (next < plate.cells[index] || found == plate.cells.end() || *found != next) {
                continue;
            }
            const auto at = static_cast<std::size_t>(std::distance(plate.cells.begin(), found));
            if (plate.crust[at].type != CrustType::Continental) {
                continue;
            }
            const auto there = static_cast<float>(grid.area(next));
            const float moved = each * (piled(index) - piled(at)) * std::min(here, there);
            thickness[index] -= moved / here;
            thickness[at] += moved / there;
        }
    }
    for (std::size_t index = 0; index < plate.crust.size(); ++index) {
        plate.crust[index].thickness = thickness[index];
    }
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

std::optional<std::size_t> Lithosphere::held(std::uint32_t plate, DVec3 at) const {
    const Plate& carrier = plates_[plate];
    const std::uint32_t cell = grid_.index(cell_at(to_plate(plate, at), grid_.level()));
    const auto found = std::ranges::lower_bound(carrier.cells, cell);
    if (found == carrier.cells.end() || *found != cell) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(carrier.cells.begin(), found));
}

std::optional<Crust> Lithosphere::crust_of(std::uint32_t plate, DVec3 at) const {
    const std::optional<std::size_t> index = held(plate, at);
    if (!index) {
        return std::nullopt;
    }
    return plates_[plate].crust[*index];
}

std::vector<bool> Lithosphere::overriding() const {
    // Where two plates' floor meets, the older and colder of it dives all along the edge, and
    // where their continents meet the thinner goes under, not place by place, so ages and
    // thicknesses varying across a plate don't hole both. Each place they overlap with crust of
    // one type votes; ties go to the lower plate.
    const std::size_t count = plates_.size();
    std::vector<int> votes(count * count, 0);
    std::vector<std::pair<std::uint32_t, Crust>> here;
    for (std::uint32_t place = 0; place < grid_.size(); ++place) {
        const DVec3 at = cell_center(grid_.cell(place));
        here.clear();
        for (std::uint32_t plate = 0; plate < count; ++plate) {
            const std::optional<Crust> crust = crust_of(plate, at);
            if (crust) {
                here.emplace_back(plate, *crust);
            }
        }
        for (std::size_t one = 0; one < here.size(); ++one) {
            for (std::size_t other = one + 1; other < here.size(); ++other) {
                const auto& [lower, lower_crust] = here[one];
                const auto& [upper, upper_crust] = here[other];
                if (lower_crust.type == upper_crust.type) {
                    votes[(lower * count) + upper] += stays_over(upper_crust, lower_crust) ? -1 : 1;
                }
            }
        }
    }
    std::vector<bool> overrides(count * count, false);
    for (std::size_t lower = 0; lower < count; ++lower) {
        for (std::size_t upper = lower + 1; upper < count; ++upper) {
            const bool lower_stays = votes[(lower * count) + upper] >= 0;
            overrides[(lower * count) + upper] = lower_stays;
            overrides[(upper * count) + lower] = !lower_stays;
        }
    }
    return overrides;
}

std::optional<std::uint32_t> Lithosphere::overlying(std::uint32_t plate, CrustType type, DVec3 at,
                                                    const std::vector<bool>& overrides) const {
    for (std::uint32_t other = 0; other < plates_.size(); ++other) {
        if (other == plate) {
            continue;
        }
        const std::optional<Crust> over = crust_of(other, at);
        if (!over) {
            continue;
        }
        const bool overridden =
            overrides[(static_cast<std::size_t>(other) * plates_.size()) + plate];
        if (over->type == type ? overridden : over->type == CrustType::Continental) {
            return other;
        }
    }
    return std::nullopt;
}

void Lithosphere::subduct(double years, Arcs arcs) {
    // A cell of ocean floor dives once its middle lies under another plate; the rest of it, if
    // any, leaves a sliver for new floor to fill.
    const std::vector<bool> overrides = overriding();
    std::vector<std::vector<std::size_t>> diving(plates_.size());
    std::vector<std::uint32_t> trenches;
    for (std::uint32_t plate = 0; plate < plates_.size(); ++plate) {
        const Plate& carrier = plates_[plate];
        for (std::size_t index = 0; index < carrier.cells.size(); ++index) {
            const Crust& crust = carrier.crust[index];
            const DVec3 at = to_planet(plate, cell_center(grid_.cell(carrier.cells[index])));
            if (crust.type == CrustType::Oceanic && overlying(plate, crust.type, at, overrides)) {
                diving[plate].push_back(index);
                trenches.push_back(grid_.index(cell_at(at, grid_.level())));
            }
        }
    }
    for (std::uint32_t plate = 0; plate < plates_.size(); ++plate) {
        remove_crust(plates_[plate], std::move(diving[plate]));
    }
    raise_arcs(trenches, years, arcs);
}

void Lithosphere::collide(double years, Ranges ranges) {
    // A cell of continent goes under once its middle lies under another plate's, and all of it
    // is piled onto the cell over that.
    const std::vector<bool> overrides = overriding();
    std::vector<std::vector<std::size_t>> scraped(plates_.size());
    struct Pile {
        std::uint32_t plate;
        std::size_t onto;
        float thickness;
    };
    std::vector<Pile> piles;
    for (std::uint32_t plate = 0; plate < plates_.size(); ++plate) {
        const Plate& carrier = plates_[plate];
        for (std::size_t index = 0; index < carrier.cells.size(); ++index) {
            const Crust& crust = carrier.crust[index];
            const DVec3 at = to_planet(plate, cell_center(grid_.cell(carrier.cells[index])));
            const std::optional<std::uint32_t> over =
                crust.type == CrustType::Continental ? overlying(plate, crust.type, at, overrides)
                                                     : std::nullopt;
            const std::optional<std::size_t> onto = over ? held(*over, at) : std::nullopt;
            if (!over || !onto) {
                continue;
            }
            // Volume for volume, between cells of the same grid in different places.
            const auto share =
                static_cast<float>(static_cast<double>(grid_.area(carrier.cells[index])) /
                                   static_cast<double>(grid_.area(plates_[*over].cells[*onto])));
            piles.push_back({.plate = *over, .onto = *onto, .thickness = crust.thickness * share});
            scraped[plate].push_back(index);
        }
    }
    for (const Pile& pile : piles) {
        Crust& crust = plates_[pile.plate].crust[pile.onto];
        crust.thickness += pile.thickness;
        crust.raised = 0.0F;
    }
    for (std::uint32_t plate = 0; plate < plates_.size(); ++plate) {
        remove_crust(plates_[plate], std::move(scraped[plate]));
        collapse(plates_[plate], grid_, ranges.strongest);
        flow(plates_[plate], grid_, settled_continent,
             std::min(static_cast<float>(years) * ranges.flow, 1.0F));
    }
}

void Lithosphere::raise_arcs(std::span<const std::uint32_t> trenches, double years, Arcs arcs) {
    // Out from each trench, a ring of places at a time, while the same plate stays on top.
    std::vector<std::uint32_t> over(grid_.size(), parted);
    std::vector<std::uint32_t> frontier;
    for (const std::uint32_t place : trenches) {
        const std::optional<Found> found = top(cell_center(grid_.cell(place)));
        if (found && over[place] == parted) {
            over[place] = found->plate;
            frontier.push_back(place);
        }
    }
    std::vector<std::pair<std::uint32_t, std::size_t>> raised;
    for (int out = 1; out <= arcs.reach && !frontier.empty(); ++out) {
        std::vector<std::uint32_t> further;
        for (const std::uint32_t place : frontier) {
            for (const std::uint32_t next : grid_.neighbors(place)) {
                const DVec3 at = cell_center(grid_.cell(next));
                const std::optional<Found> found = top(at);
                if (over[next] != parted || !found || found->plate != over[place]) {
                    continue;
                }
                over[next] = over[place];
                raised.emplace_back(over[place], held(over[place], at).value_or(0));
                further.push_back(next);
            }
        }
        frontier = std::move(further);
    }
    std::ranges::sort(raised);
    const auto [last, end] = std::ranges::unique(raised);
    raised.erase(last, end);
    const auto thicker = static_cast<float>(years) * arcs.growth;
    for (const auto& [plate, index] : raised) {
        Crust& crust = plates_[plate].crust[index];
        crust.thickness += thicker;
        crust.raised = 0.0F;
        if (crust.type == CrustType::Oceanic && crust.thickness >= arc_continent) {
            crust.type = CrustType::Continental;
        }
    }
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
