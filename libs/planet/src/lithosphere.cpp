#include <hog/core/assert.hpp>
#include <hog/planet/lithosphere.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tectonics.hpp>
#include <hog/planet/volume.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace hog::planet {

namespace {

constexpr auto parted = std::numeric_limits<std::uint32_t>::max();

// Adds crust to the plate at cells of its own grid it has none at yet; of any cell given twice,
// the first.
void add_crust(Lithosphere::Plate& plate, std::vector<std::pair<std::uint32_t, Crust>> added) {
    std::ranges::stable_sort(added, {}, &std::pair<std::uint32_t, Crust>::first);
    const auto [last, end] =
        std::ranges::unique(added, {}, &std::pair<std::uint32_t, Crust>::first);
    added.erase(last, end);
    std::vector<std::uint32_t> merged;
    std::vector<Crust> carried;
    merged.reserve(plate.cells.size() + added.size());
    carried.reserve(merged.capacity());
    std::size_t old = 0;
    for (const auto& [cell, crust] : added) {
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

// Each cell of continent next to the plate's crust at `cells` on its own grid not yet marked
// `mark`, as indices into its cells, now marked.
std::vector<std::size_t> continent_around(const Lithosphere::Plate& plate, const LevelGrid& grid,
                                          std::span<const std::size_t> cells,
                                          std::vector<std::size_t>& marks, std::size_t mark) {
    std::vector<std::size_t> around;
    for (const std::size_t index : cells) {
        for (const std::uint32_t next : grid.neighbors(plate.cells[index])) {
            const auto found = std::ranges::lower_bound(plate.cells, next);
            if (found == plate.cells.end() || *found != next) {
                continue;
            }
            const auto at = static_cast<std::size_t>(std::distance(plate.cells.begin(), found));
            if (marks[at] != mark && plate.crust[at].type == CrustType::Continental) {
                marks[at] = mark;
                around.push_back(at);
            }
        }
    }
    return around;
}

// Raises the plate's continent at `ring` toward `strongest` kilometres, all by the same share of
// its room, with up to `excess` kilometres deep over square metres; says how much it took.
double fill(Lithosphere::Plate& plate, const LevelGrid& grid, std::span<const std::size_t> ring,
            double excess, float strongest) {
    const auto most = static_cast<double>(strongest);
    double space = 0.0;
    for (const std::size_t index : ring) {
        space += (most - static_cast<double>(plate.crust[index].thickness)) *
                 static_cast<double>(grid.area(plate.cells[index]));
    }
    const double filled = space > excess ? excess / space : 1.0;
    for (const std::size_t index : ring) {
        Crust& crust = plate.crust[index];
        crust.thickness +=
            static_cast<float>(filled * (most - static_cast<double>(crust.thickness)));
    }
    return std::min(space, excess);
}

// Continental crust over `strongest` kilometres sheds what it can't hold up into the plate's
// nearest continent with room for it, volume for volume, filling that up to `strongest` a ring of
// cells at a time out from it. What finds no room, as along an arc between ocean floor, founders
// into the mantle; says how much, in kilometres deep over square metres.
double collapse(Lithosphere::Plate& plate, const LevelGrid& grid, float strongest) {
    const auto over = [&](std::size_t index) {
        return plate.crust[index].type == CrustType::Continental &&
               plate.crust[index].thickness > strongest;
    };
    const auto area = [&](std::size_t index) {
        return static_cast<double>(grid.area(plate.cells[index]));
    };
    const auto most = static_cast<double>(strongest);
    // Each cell marked with the overloaded crust it was last reached from, plus one.
    std::vector<std::size_t> marks(plate.crust.size(), 0);
    std::vector<bool> held(plate.crust.size(), false);
    double foundered = 0.0;
    for (std::size_t first = 0; first < plate.crust.size(); ++first) {
        if (held[first] || !over(first)) {
            continue;
        }
        const std::size_t mark = first + 1;
        marks[first] = mark;
        // All the overloaded crust joined to it, held up to `strongest`, and what it held over.
        double excess = 0.0;
        std::vector<std::size_t> ring{first};
        std::vector<std::size_t> room;
        while (!ring.empty()) {
            for (const std::size_t index : ring) {
                Crust& crust = plate.crust[index];
                excess += (static_cast<double>(crust.thickness) - most) * area(index);
                crust.thickness = strongest;
                held[index] = true;
            }
            const std::vector<std::size_t> around =
                continent_around(plate, grid, ring, marks, mark);
            ring.clear();
            for (const std::size_t index : around) {
                (over(index) ? ring : room).push_back(index);
            }
        }
        // Overloaded crust further out sheds on its own.
        const auto with_room = [&](std::vector<std::size_t> cells) {
            std::erase_if(cells, over);
            return cells;
        };
        for (ring = std::move(room); excess > 0.0 && !ring.empty();
             ring = with_room(continent_around(plate, grid, ring, marks, mark))) {
            excess -= fill(plate, grid, ring, excess, strongest);
        }
        foundered += excess;
    }
    return foundered;
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

// What a boundary is, by the crust either side, whether this side wins ties, and how fast they
// close across it.
Boundary::Kind kind_of(const Crust& own, const Crust& facing, bool lower, double closing) {
    if (closing <= 0.0) {
        return Boundary::Kind::Ridge;
    }
    if (own.type == CrustType::Continental && facing.type == CrustType::Continental) {
        return Boundary::Kind::Collision;
    }
    const bool over = stays_over(own, facing) || (!stays_over(facing, own) && lower);
    return over ? Boundary::Kind::Arc : Boundary::Kind::Trench;
}

// Of a place on a plate's edge, the plates on top of the places around it other than its own,
// each once.
std::vector<std::uint32_t> others_around(const LevelGrid& grid,
                                         std::span<const std::optional<Lithosphere::Found>> tops,
                                         std::uint32_t place, std::uint32_t own) {
    std::vector<std::uint32_t> others;
    for (const std::uint32_t next : grid.neighbors(place)) {
        const std::optional<Lithosphere::Found>& there = tops[next];
        if (there && there->plate != own) {
            others.push_back(there->plate);
        }
    }
    std::ranges::sort(others);
    const auto [last, end] = std::ranges::unique(others);
    others.erase(last, end);
    return others;
}

// From a place toward the places around it with `other` on top, along the surface, and the
// crust on the first of them; none if there are none.
// How a field over the grid's places rises along the surface at a place, per radian: the
// least-squares fit to how it differs at each neighbour.
DVec3 gradient(const LevelGrid& grid, std::span<const float> field, std::uint32_t place) {
    const DVec3 at = cell_center(grid.cell(place));
    // Up holds the fit solvable, where no neighbour lies.
    glm::dmat3 fit = glm::outerProduct(at, at);
    DVec3 rise{0.0};
    for (const std::uint32_t next : grid.neighbors(place)) {
        const auto differs = static_cast<double>(field[next] - field[place]);
        DVec3 offset = cell_center(grid.cell(next)) - at;
        offset -= glm::dot(offset, at) * at;
        fit += glm::outerProduct(offset, offset);
        rise += differs * offset;
    }
    if (rise == DVec3{0.0}) {
        return rise;
    }
    return glm::inverse(fit) * rise;
}

// A plate's cell as a line across the plate meets it: where it lies, how the mantle drags on
// it, how long a line across it is and how strong it is along that.
struct Stretch {
    DVec3 at{0.0};
    glm::dmat3 drag{0.0};
    double across = 0.0;
    double strong = 0.0;
};

// How many times harder than it holds a plate turning at `turn` is pulled apart along the line
// whose plane faces `facing`, with `pushed` the torque on the side it faces: what the other side
// puts on that one to keep it turning with the plate, taken as a pull even along the line.
double pulled(std::span<const Stretch> stretches, DVec3 turn, DVec3 pushed, DVec3 facing) {
    glm::dmat3 held{0.0};
    DVec3 chord{0.0};
    double length = 0.0;
    double holds = 0.0;
    for (const Stretch& stretch : stretches) {
        const double side = glm::dot(stretch.at, facing);
        if (side > 0.0) {
            held += stretch.drag;
        }
        if (std::abs(side) < 0.5 * stretch.across) {
            chord += stretch.across * stretch.at;
            length += stretch.across;
            holds += stretch.strong * stretch.across;
        }
    }
    const DVec3 apart = glm::cross(chord, facing);
    if (holds == 0.0 || glm::dot(apart, apart) == 0.0) {
        return 0.0;
    }
    return -glm::dot((held * turn) - pushed, apart) / glm::dot(apart, apart) * length / holds;
}

struct Facing {
    DVec3 toward;
    Crust crust;
};
std::optional<Facing> facing(const LevelGrid& grid,
                             std::span<const std::optional<Lithosphere::Found>> tops,
                             std::uint32_t place, std::uint32_t other) {
    const DVec3 at = cell_center(grid.cell(place));
    std::optional<Facing> found;
    for (const std::uint32_t next : grid.neighbors(place)) {
        const std::optional<Lithosphere::Found>& there = tops[next];
        if (!there || there->plate != other) {
            continue;
        }
        const DVec3 offset = cell_center(grid.cell(next)) - at;
        const DVec3 along = offset - (glm::dot(offset, at) * at);
        if (found) {
            found->toward += along;
        } else {
            found = Facing{.toward = along, .crust = there->crust};
        }
    }
    if (!found || glm::length(found->toward) == 0.0) {
        return std::nullopt;
    }
    found->toward = glm::normalize(found->toward);
    return found;
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
    : grid_{std::move(grid)}, plates_{std::move(plates)}, heat_(grid_.size(), 0.0F) {
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
    std::vector<std::vector<std::pair<std::uint32_t, Crust>>> fresh(plates_.size());
    while (!frontier.empty()) {
        std::vector<std::uint32_t> further;
        for (const std::uint32_t cell : frontier) {
            for (const std::uint32_t next : grid_.neighbors(cell)) {
                if (owner[next] != parted) {
                    continue;
                }
                owner[next] = owner[cell];
                const DVec3 own = to_plate(owner[next], cell_center(grid_.cell(next)));
                fresh[owner[next]].emplace_back(grid_.index(cell_at(own, grid_.level())), Crust{});
                further.push_back(next);
            }
        }
        frontier = std::move(further);
    }
    for (std::uint32_t plate = 0; plate < plates_.size(); ++plate) {
        add_crust(plates_[plate], std::move(fresh[plate]));
    }
}

DVec3 Lithosphere::to_planet(std::uint32_t plate, DVec3 own) const {
    return plates_[plate].frame * own;
}

DVec3 Lithosphere::to_plate(std::uint32_t plate, DVec3 at) const {
    return glm::conjugate(plates_[plate].frame) * at;
}

DVec3 Lithosphere::velocity(std::uint32_t plate, DVec3 at) const {
    return plates_[plate].rate * glm::cross(plates_[plate].pole, at);
}

std::vector<Boundary> Lithosphere::boundaries() const {
    std::vector<std::optional<Found>> tops(grid_.size());
    double sphere = 0.0;
    for (std::uint32_t place = 0; place < grid_.size(); ++place) {
        tops[place] = top(cell_center(grid_.cell(place)));
        sphere += static_cast<double>(grid_.area(place));
    }
    std::vector<Boundary> found;
    for (std::uint32_t place = 0; place < grid_.size(); ++place) {
        const std::optional<Found>& mine = tops[place];
        if (!mine) {
            continue;
        }
        const Found& here = *mine;
        const DVec3 at = cell_center(grid_.cell(place));
        for (const std::uint32_t other : others_around(grid_, tops, place, here.plate)) {
            const std::optional<Facing> across = facing(grid_, tops, place, other);
            if (!across) {
                continue;
            }
            const double closing =
                glm::dot(velocity(here.plate, at) - velocity(other, at), across->toward);
            found.push_back(
                {.kind = kind_of(here.crust, across->crust, here.plate < other, closing),
                 .place = place,
                 .plate = here.plate,
                 .other = other,
                 .normal = across->toward,
                 .length = std::sqrt(4.0 * std::numbers::pi *
                                     static_cast<double>(grid_.area(place)) / sphere),
                 .closing = closing,
                 .own = here.crust,
                 .facing = across->crust});
        }
    }
    return found;
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
        foundered_ += collapse(plates_[plate], grid_, ranges.strongest) * 1e-6;
        flow(plates_[plate], grid_, settled_continent,
             std::min(static_cast<float>(years) * ranges.flow, 1.0F));
    }
}

std::vector<std::vector<Lithosphere::Push>> Lithosphere::pushes(const Forces& forces,
                                                                Strength strength) const {
    std::vector<std::vector<Push>> found(plates_.size());
    for (const Boundary& edge : boundaries()) {
        const DVec3 at = cell_center(grid_.cell(edge.place));
        const double relative = glm::length(velocity(edge.plate, at) - velocity(edge.other, at));
        if (relative == 0.0) {
            continue;
        }
        const double across = std::abs(edge.closing) / relative;
        double push = 0.0;
        if (edge.kind == Boundary::Kind::Trench) {
            const auto age = static_cast<double>(edge.own.age);
            push = std::min(std::sqrt(std::min(age, forces.mature) / forces.mature),
                            strength.ocean / softer_);
        } else if (edge.kind == Boundary::Kind::Ridge) {
            push = -forces.ridge;
        } else if (edge.kind == Boundary::Kind::Collision) {
            const double raised =
                static_cast<double>(std::max(edge.own.thickness, edge.facing.thickness)) /
                static_cast<double>(settled_continent);
            push = -forces.ridge * std::max((raised * raised) - 1.0, 0.0);
        }
        found[edge.plate].push_back({.at = at, .force = push * across * edge.length * edge.normal});
    }
    // Downhill off the dome, by how steep it is over the place's area, so a dome's whole rim
    // pushes as far as it rises over it.
    double sphere = 0.0;
    for (std::uint32_t place = 0; place < grid_.size(); ++place) {
        sphere += static_cast<double>(grid_.area(place));
    }
    for (std::uint32_t place = 0; place < grid_.size(); ++place) {
        const DVec3 steep = gradient(grid_, heat_, place);
        if (steep == DVec3{0.0}) {
            continue;
        }
        const DVec3 at = cell_center(grid_.cell(place));
        if (const std::optional<Found> over = top(at)) {
            const double area =
                4.0 * std::numbers::pi * static_cast<double>(grid_.area(place)) / sphere;
            found[over->plate].push_back({.at = at, .force = -forces.dome * area * steep});
        }
    }
    return found;
}

void Lithosphere::drive(Forces forces, Strength strength) {
    // Each force along the surface at a place turns its plate about the planet's middle.
    const std::vector<std::vector<Push>> found = pushes(forces, strength);
    std::vector<DVec3> torques(plates_.size(), DVec3{0.0});
    for (std::uint32_t plate = 0; plate < plates_.size(); ++plate) {
        for (const Push& push : found[plate]) {
            torques[plate] += glm::cross(push.at, push.force);
        }
    }
    // The drag on a plate turning at w is -D w, D summing (I - r r^T) over its crust by its share
    // of the sphere; the same sum without keels gives how fast its crust moves on the whole.
    double sphere = 0.0;
    for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
        sphere += static_cast<double>(grid_.area(cell));
    }
    std::vector<DVec3> turns(plates_.size(), DVec3{0.0});
    double before = 0.0;
    double after = 0.0;
    for (std::uint32_t plate = 0; plate < plates_.size(); ++plate) {
        const Plate& carrier = plates_[plate];
        glm::dmat3 drag{0.0};
        glm::dmat3 spread{0.0};
        for (std::size_t index = 0; index < carrier.cells.size(); ++index) {
            const DVec3 at = to_planet(plate, cell_center(grid_.cell(carrier.cells[index])));
            const glm::dmat3 across =
                (static_cast<double>(grid_.area(carrier.cells[index])) / sphere) *
                (glm::dmat3{1.0} - glm::outerProduct(at, at));
            spread += across;
            drag +=
                (carrier.crust[index].type == CrustType::Continental ? forces.keel : 1.0) * across;
        }
        if (glm::determinant(drag) != 0.0) {
            turns[plate] = glm::inverse(drag) * torques[plate];
        }
        const DVec3 now = carrier.rate * carrier.pole;
        before += glm::dot(now, spread * now);
        after += glm::dot(turns[plate], spread * turns[plate]);
    }
    if (pace_ == 0.0 && after > 0.0) {
        pace_ = std::sqrt(before / after);
    }
    for (std::uint32_t plate = 0; plate < plates_.size(); ++plate) {
        const DVec3 turn = pace_ * turns[plate];
        const double rate = glm::length(turn);
        if (rate > 0.0) {
            plates_[plate].pole = turn / rate;
        }
        plates_[plate].rate = rate;
    }
}

void Lithosphere::warm(double years, Mantle mantle) {
    const auto toward = static_cast<float>(1.0 - std::exp(-years / mantle.settling));
    for (std::uint32_t place = 0; place < grid_.size(); ++place) {
        const std::optional<Found> found = top(cell_center(grid_.cell(place)));
        const float warmest = found && found->crust.type == CrustType::Continental ? 1.0F : 0.0F;
        heat_[place] += (warmest - heat_[place]) * toward;
    }
    constexpr double gas = 8.314;
    const double now = mantle.potential + std::max(mantle.hotter - (mantle.cooling * time_), 0.0);
    softer_ = std::exp(mantle.activation / gas * ((1.0 / mantle.potential) - (1.0 / now)));
}

void Lithosphere::rift(const Forces& forces, Strength strength) {
    const std::vector<std::vector<Push>> found = pushes(forces, strength);
    const std::size_t count = plates_.size();
    for (std::uint32_t plate = 0; plate < count; ++plate) {
        const std::optional<DVec3> facing = weakest(plate, found[plate], forces, strength);
        if (!facing) {
            continue;
        }
        const Plate& carrier = plates_[plate];
        Plate kept{.frame = carrier.frame,
                   .pole = carrier.pole,
                   .rate = carrier.rate,
                   .cells = {},
                   .crust = {}};
        Plate parted_off = kept;
        for (std::size_t index = 0; index < carrier.cells.size(); ++index) {
            const DVec3 at = to_planet(plate, cell_center(grid_.cell(carrier.cells[index])));
            Plate& side = glm::dot(at, *facing) > 0.0 ? kept : parted_off;
            side.cells.push_back(carrier.cells[index]);
            side.crust.push_back(carrier.crust[index]);
        }
        plates_[plate] = std::move(kept);
        plates_.push_back(std::move(parted_off));
    }
}

std::optional<DVec3> Lithosphere::weakest(std::uint32_t plate, std::span<const Push> pushes,
                                          const Forces& forces, Strength strength) const {
    double sphere = 0.0;
    for (std::uint32_t place = 0; place < grid_.size(); ++place) {
        sphere += static_cast<double>(grid_.area(place));
    }
    const Plate& carrier = plates_[plate];
    std::vector<Stretch> stretches(carrier.cells.size());
    glm::dmat3 drag{0.0};
    std::optional<std::size_t> warmest;
    float most = 0.0F;
    for (std::size_t index = 0; index < stretches.size(); ++index) {
        const std::uint32_t cell = carrier.cells[index];
        const bool continental = carrier.crust[index].type == CrustType::Continental;
        Stretch& stretch = stretches[index];
        stretch.at = to_planet(plate, cell_center(grid_.cell(cell)));
        const double share = static_cast<double>(grid_.area(cell)) / sphere;
        stretch.drag = (continental ? forces.keel : 1.0) * share *
                       (glm::dmat3{1.0} - glm::outerProduct(stretch.at, stretch.at));
        drag += stretch.drag;
        stretch.across = std::sqrt(4.0 * std::numbers::pi * share);
        stretch.strong = strength.ocean / softer_;
        if (continental) {
            const float heat = heat_[grid_.index(cell_at(stretch.at, grid_.level()))];
            stretch.strong =
                strength.cool * std::pow(strength.warm / strength.cool, heat) / softer_;
            if (!warmest || heat > most) {
                warmest = index;
                most = heat;
            }
        }
    }
    if (!warmest || glm::determinant(drag) == 0.0) {
        return std::nullopt;
    }
    DVec3 torque{0.0};
    for (const Push& push : pushes) {
        torque += glm::cross(push.at, push.force);
    }
    const DVec3 turn = glm::inverse(drag) * torque;
    // Lines through the warmest mantle, so many ways round.
    constexpr int ways = 12;
    const DVec3 middle = stretches[*warmest].at;
    const DVec3 east = glm::normalize(
        glm::cross(middle, std::abs(middle.x) < 0.9 ? DVec3{1.0, 0.0, 0.0} : DVec3{0.0, 1.0, 0.0}));
    const DVec3 north = glm::cross(middle, east);
    // Only a line pulled harder than it holds splits.
    double hardest = 1.0;
    std::optional<DVec3> split;
    for (int way = 0; way < ways; ++way) {
        const double angle = std::numbers::pi * way / ways;
        const DVec3 facing = (std::cos(angle) * east) + (std::sin(angle) * north);
        DVec3 pushed{0.0};
        for (const Push& push : pushes) {
            if (glm::dot(push.at, facing) > 0.0) {
                pushed += glm::cross(push.at, push.force);
            }
        }
        const double pull = pulled(stretches, turn, pushed, facing);
        if (pull > hardest) {
            hardest = pull;
            split = facing;
        }
    }
    return split;
}

void Lithosphere::weld(double years) {
    // How fast each two plates move against each other at most anywhere they meet, and where
    // their continents meet, and whether a range lies anywhere along that.
    const std::size_t count = plates_.size();
    std::vector<double> anywhere(count * count, -1.0);
    std::vector<double> fastest(count * count, -1.0);
    std::vector<bool> sutured(count * count, false);
    for (const Boundary& edge : boundaries()) {
        const DVec3 at = cell_center(grid_.cell(edge.place));
        const double moving = glm::length(velocity(edge.plate, at) - velocity(edge.other, at));
        const std::size_t pair =
            (std::min(edge.plate, edge.other) * count) + std::max(edge.plate, edge.other);
        anywhere[pair] = std::max(anywhere[pair], moving);
        if (edge.own.type != CrustType::Continental || edge.facing.type != CrustType::Continental) {
            continue;
        }
        fastest[pair] = std::max(fastest[pair], moving);
        if (std::max(edge.own.thickness, edge.facing.thickness) > settled_continent) {
            sutured[pair] = true;
        }
    }
    // Plates welded together, each by the one its crust goes to: the one with most cells.
    std::vector<std::uint32_t> into(count);
    for (std::uint32_t plate = 0; plate < count; ++plate) {
        into[plate] = plate;
    }
    const auto root = [&](std::uint32_t plate) {
        while (into[plate] != plate) {
            plate = into[plate];
        }
        return plate;
    };
    const double half_place = 0.5 * std::sqrt(4.0 * std::numbers::pi / grid_.size());
    for (std::uint32_t lower = 0; lower < count; ++lower) {
        for (std::uint32_t upper = lower + 1; upper < count; ++upper) {
            const std::size_t pair = (lower * count) + upper;
            const bool captured = anywhere[pair] >= 0.0 && anywhere[pair] * years < half_place;
            const bool collided = sutured[pair] && fastest[pair] * years < half_place;
            const std::uint32_t one = root(lower);
            const std::uint32_t other = root(upper);
            if ((!captured && !collided) || one == other) {
                continue;
            }
            const bool bigger = plates_[one].cells.size() >= plates_[other].cells.size();
            into[bigger ? other : one] = bigger ? one : other;
        }
    }
    for (std::uint32_t plate = 0; plate < count; ++plate) {
        if (root(plate) != plate) {
            take_in(root(plate), plate);
        }
    }
}

void Lithosphere::take_in(std::uint32_t plate, std::uint32_t from) {
    // Every cell of the plate's own grid it has no crust at, whose middle lies over the other's
    // crust, takes that crust; where both have crust, its own stays.
    std::vector<std::pair<std::uint32_t, Crust>> added;
    for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
        const DVec3 at = to_planet(plate, cell_center(grid_.cell(cell)));
        if (crust_of(plate, at)) {
            continue;
        }
        if (const std::optional<Crust> crust = crust_of(from, at)) {
            added.emplace_back(cell, *crust);
        }
    }
    add_crust(plates_[plate], std::move(added));
    plates_[from].cells.clear();
    plates_[from].crust.clear();
    plates_[from].rate = 0.0;
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
