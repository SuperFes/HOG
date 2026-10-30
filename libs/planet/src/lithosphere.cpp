#include <hog/core/assert.hpp>
#include <hog/jobs/job_system.hpp>
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

// Calls body(index, thread) for each index below `count`, in chunks of `chunk` over the threads
// of `jobs` if there are any, else on this one as thread nought.
template <typename F>
void each(jobs::JobSystem* jobs, std::uint32_t count, std::uint32_t chunk, F body) {
    if (jobs == nullptr) {
        for (std::uint32_t index = 0; index < count; ++index) {
            body(index, 0U);
        }
        return;
    }
    jobs->parallel_for(count, chunk, [&](jobs::Range range, std::uint32_t thread) {
        for (std::uint32_t index = range.begin; index < range.end; ++index) {
            body(index, thread);
        }
    });
}

// Of each two plates with crust of one type `here`, at a place, by ascending plate, a vote in
// `votes`, a row per plate, for the lower staying on top, or against.
void tally(std::vector<int>& votes, std::size_t count,
           std::span<const std::pair<std::uint32_t, Crust>> here) {
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

// Places of the grid, or cells of a plate, to each share of the work.
constexpr std::uint32_t places_per_job = 256;

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

// What crust collapsing founders, in kilometres deep over square metres, and whether any of it
// was piled up at cells of the plate's grid given, which then had nowhere to go.
struct Collapse {
    double foundered = 0.0;
    bool jammed = false;
};

// Continental crust over `strongest` kilometres sheds what it can't hold up into the plate's
// nearest continent with room for it, volume for volume, filling that up to `strongest` a ring of
// cells at a time out from it. What finds no room, as along an arc between ocean floor, founders
// into the mantle. `piled` are the cells of the plate's grid crust was piled onto, ascending.
Collapse collapse(Lithosphere::Plate& plate, const LevelGrid& grid, float strongest,
                  std::span<const std::uint32_t> piled) {
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
    Collapse collapsed;
    for (std::size_t first = 0; first < plate.crust.size(); ++first) {
        if (held[first] || !over(first)) {
            continue;
        }
        const std::size_t mark = first + 1;
        marks[first] = mark;
        // All the overloaded crust joined to it, held up to `strongest`, and what it held over.
        double excess = 0.0;
        bool piled_up = false;
        std::vector<std::size_t> ring{first};
        std::vector<std::size_t> room;
        while (!ring.empty()) {
            for (const std::size_t index : ring) {
                Crust& crust = plate.crust[index];
                excess += (static_cast<double>(crust.thickness) - most) * area(index);
                crust.thickness = strongest;
                held[index] = true;
                piled_up = piled_up || std::ranges::binary_search(piled, plate.cells[index]);
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
        collapsed.foundered += excess;
        collapsed.jammed = collapsed.jammed || (piled_up && excess > 0.0);
    }
    return collapsed;
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
// puts on that one to keep it turning with the plate, against how strong the line is where that
// pulls it apart. Tension t at a place p on the line puts -t (p x facing) on the side. Across a
// plate from edge to edge it's taken as even along the line. A line on the plate all the way
// round can't hold so, as its levers cancel, but by pulling on one side and pushing on the
// other: the least tension that does varies along it as p x facing does.
double pulled(std::span<const Stretch> stretches, DVec3 turn, DVec3 pushed, DVec3 facing) {
    const DVec3 first = glm::normalize(
        glm::cross(facing, std::abs(facing.x) < 0.9 ? DVec3{1.0, 0.0, 0.0} : DVec3{0.0, 1.0, 0.0}));
    const DVec3 second = glm::cross(facing, first);
    const auto on_line = [&](const Stretch& stretch) {
        return std::abs(glm::dot(stretch.at, facing)) < 0.5 * stretch.across;
    };
    glm::dmat3 held{0.0};
    // Of the levers along the line in the plane's two directions, weighted by length.
    double xx = 0.0;
    double xy = 0.0;
    double yy = 0.0;
    DVec3 levers{0.0};
    double widest = 0.0;
    std::vector<double> round;
    for (const Stretch& stretch : stretches) {
        if (glm::dot(stretch.at, facing) > 0.0) {
            held += stretch.drag;
        }
        if (on_line(stretch)) {
            const DVec3 lever = glm::cross(stretch.at, facing);
            const double x = glm::dot(lever, first);
            const double y = glm::dot(lever, second);
            xx += stretch.across * x * x;
            xy += stretch.across * x * y;
            yy += stretch.across * y * y;
            levers += stretch.across * lever;
            widest = std::max(widest, stretch.across);
            round.push_back(std::atan2(glm::dot(stretch.at, second), glm::dot(stretch.at, first)));
        }
    }
    if (round.empty()) {
        return 0.0;
    }
    std::ranges::sort(round);
    double gap = round.front() + (2.0 * std::numbers::pi) - round.back();
    for (std::size_t index = 1; index < round.size(); ++index) {
        gap = std::max(gap, round[index] - round[index - 1]);
    }
    const DVec3 needed = pushed - (held * turn);
    // Tension at a place as c . (p x facing), plus `even` all along.
    DVec3 c{0.0};
    double even = 0.0;
    const double det = (xx * yy) - (xy * xy);
    if (gap > 2.0 * widest) {
        if (glm::dot(levers, levers) == 0.0) {
            return 0.0;
        }
        even = glm::dot(needed, levers) / glm::dot(levers, levers);
    } else if (det > 0.0) {
        const double bx = glm::dot(needed, first);
        const double by = glm::dot(needed, second);
        c = ((((yy * bx) - (xy * by)) / det) * first) + ((((xx * by) - (xy * bx)) / det) * second);
    } else {
        return 0.0;
    }
    double pulling = 0.0;
    double holds = 0.0;
    for (const Stretch& stretch : stretches) {
        if (!on_line(stretch)) {
            continue;
        }
        const double tension = even + glm::dot(c, glm::cross(stretch.at, facing));
        if (tension > 0.0) {
            pulling += tension * stretch.across;
            holds += stretch.strong * stretch.across;
        }
    }
    return holds > 0.0 ? pulling / holds : 0.0;
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

// Of each two plates, a row per plate, whether they move as one: slower than `slowest` against
// each other everywhere they meet, as `speeds` says at each of the `edges`, or everywhere their
// continents meet where a range lies along that.
std::vector<bool> as_one(std::span<const Boundary> edges, std::span<const double> speeds,
                         std::size_t count, double slowest) {
    std::vector<double> anywhere(count * count, -1.0);
    std::vector<double> fastest(count * count, -1.0);
    std::vector<bool> sutured(count * count, false);
    for (std::size_t index = 0; index < edges.size(); ++index) {
        const Boundary& edge = edges[index];
        const std::size_t pair =
            (std::min(edge.plate, edge.other) * count) + std::max(edge.plate, edge.other);
        anywhere[pair] = std::max(anywhere[pair], speeds[index]);
        if (edge.own.type != CrustType::Continental || edge.facing.type != CrustType::Continental) {
            continue;
        }
        fastest[pair] = std::max(fastest[pair], speeds[index]);
        if (std::max(edge.own.thickness, edge.facing.thickness) > Lithosphere::settled_continent) {
            sutured[pair] = true;
        }
    }
    std::vector<bool> joined(count * count, false);
    for (std::size_t pair = 0; pair < joined.size(); ++pair) {
        joined[pair] = (anywhere[pair] >= 0.0 && anywhere[pair] < slowest) ||
                       (sutured[pair] && fastest[pair] < slowest);
    }
    return joined;
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
    : grid_{std::move(grid)}, plates_{std::move(plates)}, bounds_(plates_.size()),
      slabs_(plates_.size()), heat_(grid_.size(), 0.0F) {
    for (std::uint32_t plate = 0; plate < plates_.size(); ++plate) {
        const Plate& carrier = plates_[plate];
        HOG_ASSERT(carrier.cells.size() == carrier.crust.size(), "one crust to each cell");
        HOG_ASSERT(std::ranges::is_sorted(carrier.cells), "cells ascending");
        HOG_ASSERT(carrier.cells.empty() || carrier.cells.back() < grid_.size(),
                   "cells of the grid");
        bound(plate);
    }
}

void Lithosphere::bound(std::uint32_t plate) {
    const Plate& carrier = plates_[plate];
    if (carrier.cells.empty()) {
        bounds_[plate] = Bound{};
        return;
    }
    DVec3 sum{0.0};
    for (const std::uint32_t cell : carrier.cells) {
        sum += cell_center(grid_.cell(cell));
    }
    const double length = glm::length(sum);
    const DVec3 middle = length > 0.0 ? sum / length : DVec3{0.0, 0.0, 1.0};
    double nearest = 1.0;
    for (const std::uint32_t cell : carrier.cells) {
        nearest = std::min(nearest, glm::dot(cell_center(grid_.cell(cell)), middle));
    }
    // A place in a cell lies no further from its middle than twice the cells' mean spacing.
    const double spacing = std::sqrt(4.0 * std::numbers::pi / grid_.size());
    const double angle = std::acos(std::clamp(nearest, -1.0, 1.0)) + (2.0 * spacing);
    bounds_[plate] = {.middle = middle, .reach = angle < std::numbers::pi ? std::cos(angle) : -2.0};
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
    for (std::vector<Slab>& slabs : slabs_) {
        for (Slab& slab : slabs) {
            slab.idle += years;
        }
        std::erase_if(slabs, [](const Slab& slab) { return slab.idle > breakoff; });
    }
    time_ += years;
}

void Lithosphere::spread() {
    // Each parted place of the grid takes the plate of the nearest crust, out from the gap's
    // edges a ring of cells at a time.
    std::vector<std::uint32_t> owner(grid_.size(), parted);
    const std::vector<std::optional<Found>> on_top = tops();
    for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
        if (const std::optional<Found>& found = on_top[cell]) {
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
        bound(plate);
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

std::vector<std::optional<Lithosphere::Found>> Lithosphere::tops() const {
    std::vector<std::optional<Found>> found(grid_.size());
    each(jobs_, grid_.size(), places_per_job, [&](std::uint32_t place, std::uint32_t /*thread*/) {
        found[place] = top(cell_center(grid_.cell(place)));
    });
    return found;
}

std::vector<Boundary> Lithosphere::boundaries() const {
    const std::vector<std::optional<Found>> on_top = tops();
    double sphere = 0.0;
    for (std::uint32_t place = 0; place < grid_.size(); ++place) {
        sphere += static_cast<double>(grid_.area(place));
    }
    std::vector<Boundary> found;
    for (std::uint32_t place = 0; place < grid_.size(); ++place) {
        const std::optional<Found>& mine = on_top[place];
        if (!mine) {
            continue;
        }
        const Found& here = *mine;
        const DVec3 at = cell_center(grid_.cell(place));
        for (const std::uint32_t other : others_around(grid_, on_top, place, here.plate)) {
            const std::optional<Facing> across = facing(grid_, on_top, place, other);
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
    const DVec3 own = to_plate(plate, at);
    if (glm::dot(own, bounds_[plate].middle) < bounds_[plate].reach) {
        return std::nullopt;
    }
    const std::uint32_t cell = grid_.index(cell_at(own, grid_.level()));
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
    const std::uint32_t threads = jobs_ != nullptr ? jobs_->thread_count() : 1U;
    std::vector<std::vector<int>> tallies(threads, std::vector<int>(count * count, 0));
    std::vector<std::vector<std::pair<std::uint32_t, Crust>>> scratch(threads);
    each(jobs_, grid_.size(), places_per_job, [&](std::uint32_t place, std::uint32_t thread) {
        std::vector<int>& votes = tallies[thread];
        std::vector<std::pair<std::uint32_t, Crust>>& here = scratch[thread];
        const DVec3 at = cell_center(grid_.cell(place));
        here.clear();
        for (std::uint32_t plate = 0; plate < count; ++plate) {
            const std::optional<Crust> crust = crust_of(plate, at);
            if (crust) {
                here.emplace_back(plate, *crust);
            }
        }
        tally(votes, count, here);
    });
    std::vector<int> votes(count * count, 0);
    for (const std::vector<int>& tally : tallies) {
        for (std::size_t pair = 0; pair < tally.size(); ++pair) {
            votes[pair] += tally[pair];
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
    std::vector<std::uint8_t> dives;
    for (std::uint32_t plate = 0; plate < plates_.size(); ++plate) {
        const Plate& carrier = plates_[plate];
        const auto cells = static_cast<std::uint32_t>(carrier.cells.size());
        dives.assign(cells, 0);
        each(jobs_, cells, places_per_job, [&](std::uint32_t index, std::uint32_t /*thread*/) {
            const Crust& crust = carrier.crust[index];
            const DVec3 at = to_planet(plate, cell_center(grid_.cell(carrier.cells[index])));
            const bool under = crust.type == CrustType::Oceanic &&
                               overlying(plate, crust.type, at, overrides).has_value();
            dives[index] = under ? 1 : 0;
        });
        for (std::uint32_t index = 0; index < cells; ++index) {
            if (dives[index] != 0) {
                diving[plate].push_back(index);
                const DVec3 at = to_planet(plate, cell_center(grid_.cell(carrier.cells[index])));
                trenches.push_back(grid_.index(cell_at(at, grid_.level())));
            }
        }
    }
    for (std::uint32_t plate = 0; plate < plates_.size(); ++plate) {
        // A slab hangs on from the floor left at the trench, not the floor gone down it.
        std::vector<std::uint32_t> gone;
        for (const std::size_t index : diving[plate]) {
            gone.push_back(plates_[plate].cells[index]);
        }
        std::erase_if(slabs_[plate], [&](const Slab& slab) {
            return std::ranges::binary_search(gone, slab.cell);
        });
        remove_crust(plates_[plate], std::move(diving[plate]));
        bound(plate);
    }
    raise_arcs(trenches, years, arcs);
}

void Lithosphere::collide(double years, Ranges ranges) {
    // A cell of continent goes under once its middle lies under another plate's, and all of it
    // is piled onto the cell over that.
    const std::vector<bool> overrides = overriding();
    std::vector<std::vector<std::size_t>> scraped(plates_.size());
    struct Pile {
        std::uint32_t from;
        std::uint32_t plate;
        std::size_t onto;
        float thickness;
    };
    std::vector<Pile> piles;
    std::vector<std::optional<std::pair<std::uint32_t, std::size_t>>> unders;
    for (std::uint32_t plate = 0; plate < plates_.size(); ++plate) {
        const Plate& carrier = plates_[plate];
        const auto cells = static_cast<std::uint32_t>(carrier.cells.size());
        unders.assign(cells, std::nullopt);
        each(jobs_, cells, places_per_job, [&](std::uint32_t index, std::uint32_t /*thread*/) {
            const Crust& crust = carrier.crust[index];
            if (crust.type != CrustType::Continental) {
                return;
            }
            const DVec3 at = to_planet(plate, cell_center(grid_.cell(carrier.cells[index])));
            const std::optional<std::uint32_t> over = overlying(plate, crust.type, at, overrides);
            const std::optional<std::size_t> onto = over ? held(*over, at) : std::nullopt;
            if (over && onto) {
                unders[index].emplace(*over, *onto);
            }
        });
        for (std::uint32_t index = 0; index < cells; ++index) {
            const std::optional<std::pair<std::uint32_t, std::size_t>>& under = unders[index];
            if (!under) {
                continue;
            }
            const Crust& crust = carrier.crust[index];
            const auto [over, onto] = *under;
            // Volume for volume, between cells of the same grid in different places.
            const auto share =
                static_cast<float>(static_cast<double>(grid_.area(carrier.cells[index])) /
                                   static_cast<double>(grid_.area(plates_[over].cells[onto])));
            piles.push_back(
                {.from = plate, .plate = over, .onto = onto, .thickness = crust.thickness * share});
            scraped[plate].push_back(index);
        }
    }
    // Each plate's cells piled onto, and the plates that piled them.
    std::vector<std::vector<std::uint32_t>> piled(plates_.size());
    std::vector<std::vector<std::uint32_t>> piled_by(plates_.size());
    for (const Pile& pile : piles) {
        Crust& crust = plates_[pile.plate].crust[pile.onto];
        crust.thickness += pile.thickness;
        crust.raised = 0.0F;
        piled[pile.plate].push_back(plates_[pile.plate].cells[pile.onto]);
        piled_by[pile.plate].push_back(pile.from);
    }
    for (std::uint32_t plate = 0; plate < plates_.size(); ++plate) {
        remove_crust(plates_[plate], std::move(scraped[plate]));
        std::ranges::sort(piled[plate]);
        const Collapse collapsed = collapse(plates_[plate], grid_, ranges.strongest, piled[plate]);
        foundered_ += collapsed.foundered * 1e-6;
        if (collapsed.jammed) {
            for (const std::uint32_t from : piled_by[plate]) {
                jammed_.emplace_back(std::min(from, plate), std::max(from, plate));
            }
        }
        flow(plates_[plate], grid_, settled_continent,
             std::min(static_cast<float>(years) * ranges.flow, 1.0F));
    }
}

void Lithosphere::hang(std::span<const Boundary> edges) {
    std::vector<std::vector<Slab>> fed(plates_.size());
    for (const Boundary& edge : edges) {
        if (edge.kind != Boundary::Kind::Trench) {
            continue;
        }
        const DVec3 at = cell_center(grid_.cell(edge.place));
        const double relative = glm::length(velocity(edge.plate, at) - velocity(edge.other, at));
        const std::optional<std::size_t> index = held(edge.plate, at);
        if (relative == 0.0 || !index) {
            continue;
        }
        // As far as the plates close across the edge, not slide past along it.
        const double across = edge.closing / relative;
        fed[edge.plate].push_back({.cell = plates_[edge.plate].cells[*index],
                                   .pull = to_plate(edge.plate, across * edge.length * edge.normal),
                                   .age = edge.own.age,
                                   .idle = 0.0});
    }
    for (std::uint32_t plate = 0; plate < plates_.size(); ++plate) {
        std::vector<std::uint32_t> cells;
        for (const Slab& slab : fed[plate]) {
            cells.push_back(slab.cell);
        }
        std::ranges::sort(cells);
        std::erase_if(slabs_[plate], [&](const Slab& slab) {
            return std::ranges::binary_search(cells, slab.cell);
        });
        slabs_[plate].insert(slabs_[plate].end(), fed[plate].begin(), fed[plate].end());
    }
}

std::vector<std::vector<Lithosphere::Push>> Lithosphere::pushes(std::span<const Boundary> edges,
                                                                const Forces& forces,
                                                                Strength strength) const {
    std::vector<std::vector<Push>> found(plates_.size());
    for (std::uint32_t plate = 0; plate < plates_.size(); ++plate) {
        for (const Slab& slab : slabs_[plate]) {
            const auto age = static_cast<double>(slab.age);
            const double push = std::min(std::sqrt(std::min(age, forces.mature) / forces.mature),
                                         strength.ocean / softer_);
            found[plate].push_back({.at = to_planet(plate, cell_center(grid_.cell(slab.cell))),
                                    .force = push * to_planet(plate, slab.pull)});
        }
    }
    for (const Boundary& edge : edges) {
        const DVec3 at = cell_center(grid_.cell(edge.place));
        const double relative = glm::length(velocity(edge.plate, at) - velocity(edge.other, at));
        if (relative == 0.0) {
            continue;
        }
        const double across = std::abs(edge.closing) / relative;
        double push = 0.0;
        if (edge.kind == Boundary::Kind::Ridge) {
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
    const std::vector<std::optional<Found>> on_top = tops();
    for (std::uint32_t place = 0; place < grid_.size(); ++place) {
        const DVec3 steep = gradient(grid_, heat_, place);
        if (steep == DVec3{0.0}) {
            continue;
        }
        const DVec3 at = cell_center(grid_.cell(place));
        if (const std::optional<Found>& over = on_top[place]) {
            const double area =
                4.0 * std::numbers::pi * static_cast<double>(grid_.area(place)) / sphere;
            found[over->plate].push_back({.at = at, .force = -forces.dome * area * steep});
        }
    }
    return found;
}

void Lithosphere::drive(Forces forces, Strength strength) {
    // Each force along the surface at a place turns its plate about the planet's middle.
    const std::vector<Boundary> edges = boundaries();
    hang(edges);
    const std::vector<std::vector<Push>> found = pushes(edges, forces, strength);
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
            const double share = static_cast<double>(grid_.area(carrier.cells[index])) / sphere;
            spread += share * (glm::dmat3{1.0} - glm::outerProduct(at, at));
            drag += (carrier.crust[index].type == CrustType::Continental ? forces.keel : 1.0) *
                    place_drag(at, share);
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
    const std::vector<std::optional<Found>> on_top = tops();
    for (std::uint32_t place = 0; place < grid_.size(); ++place) {
        const std::optional<Found>& found = on_top[place];
        const float warmest = found && found->crust.type == CrustType::Continental ? 1.0F : 0.0F;
        heat_[place] += (warmest - heat_[place]) * toward;
    }
    constexpr double gas = 8.314;
    const double now = mantle.potential + std::max(mantle.hotter - (mantle.cooling * time_), 0.0);
    softer_ = std::exp(mantle.activation / gas * ((1.0 / mantle.potential) - (1.0 / now)));
}

void Lithosphere::rift(const Forces& forces, Strength strength) {
    const std::vector<Boundary> edges = boundaries();
    hang(edges);
    const std::vector<std::vector<Push>> found = pushes(edges, forces, strength);
    const auto count = static_cast<std::uint32_t>(plates_.size());
    std::vector<std::optional<DVec3>> lines(count);
    each(jobs_, count, 1, [&](std::uint32_t plate, std::uint32_t /*thread*/) {
        lines[plate] = weakest(plate, found[plate], forces, strength);
    });
    for (std::uint32_t plate = 0; plate < count; ++plate) {
        const std::optional<DVec3>& facing = lines[plate];
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
        std::vector<Slab> slabs_kept;
        std::vector<Slab> slabs_parted_off;
        for (const Slab& slab : slabs_[plate]) {
            const DVec3 at = to_planet(plate, cell_center(grid_.cell(slab.cell)));
            (glm::dot(at, *facing) > 0.0 ? slabs_kept : slabs_parted_off).push_back(slab);
        }
        plates_[plate] = std::move(kept);
        plates_.push_back(std::move(parted_off));
        slabs_[plate] = std::move(slabs_kept);
        slabs_.push_back(std::move(slabs_parted_off));
        bounds_.emplace_back();
        bound(plate);
        bound(static_cast<std::uint32_t>(plates_.size() - 1));
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
        stretch.drag = (continental ? forces.keel : 1.0) * place_drag(stretch.at, share);
        drag += stretch.drag;
        stretch.across = std::sqrt(4.0 * std::numbers::pi * share);
        // Floor thickens as it cools, as its slab's pull grows.
        const auto age = static_cast<double>(carrier.crust[index].age);
        stretch.strong =
            strength.ocean * std::sqrt(std::min(age, forces.mature) / forces.mature) / softer_;
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
    const std::size_t count = plates_.size();
    const std::vector<Boundary> edges = boundaries();
    hang(edges);
    std::vector<double> speeds;
    speeds.reserve(edges.size());
    for (const Boundary& edge : edges) {
        const DVec3 at = cell_center(grid_.cell(edge.place));
        speeds.push_back(glm::length(velocity(edge.plate, at) - velocity(edge.other, at)));
    }
    const double half_place = 0.5 * std::sqrt(4.0 * std::numbers::pi / grid_.size());
    std::vector<bool> joined = as_one(edges, speeds, count, half_place / years);
    for (const auto& [lower, upper] : jammed_) {
        joined[(lower * count) + upper] = true;
    }
    jammed_.clear();
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
    for (std::uint32_t lower = 0; lower < count; ++lower) {
        for (std::uint32_t upper = lower + 1; upper < count; ++upper) {
            const std::uint32_t one = root(lower);
            const std::uint32_t other = root(upper);
            if (!joined[(lower * count) + upper] || one == other) {
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
    drop_bare();
}

void Lithosphere::drop_bare() {
    std::size_t kept = 0;
    for (std::size_t plate = 0; plate < plates_.size(); ++plate) {
        if (plates_[plate].cells.empty()) {
            continue;
        }
        if (kept != plate) {
            plates_[kept] = std::move(plates_[plate]);
            bounds_[kept] = bounds_[plate];
            slabs_[kept] = std::move(slabs_[plate]);
        }
        ++kept;
    }
    plates_.resize(kept);
    bounds_.resize(kept);
    slabs_.resize(kept);
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
    bound(plate);
    // Its slabs hang on, now from the plate it's part of.
    for (Slab slab : slabs_[from]) {
        const DVec3 at = to_planet(from, cell_center(grid_.cell(slab.cell)));
        slab.cell = grid_.index(cell_at(to_plate(plate, at), grid_.level()));
        slab.pull = to_plate(plate, to_planet(from, slab.pull));
        slabs_[plate].push_back(slab);
    }
    slabs_[from].clear();
    plates_[from].cells.clear();
    plates_[from].crust.clear();
    plates_[from].rate = 0.0;
    bound(from);
}

void Lithosphere::raise_arcs(std::span<const std::uint32_t> trenches, double years, Arcs arcs) {
    // Out from each trench, a ring of places at a time, while the same plate stays on top.
    const std::vector<std::optional<Found>> on_top = tops();
    std::vector<std::uint32_t> over(grid_.size(), parted);
    std::vector<std::uint32_t> frontier;
    for (const std::uint32_t place : trenches) {
        const std::optional<Found>& found = on_top[place];
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
                const std::optional<Found>& found = on_top[next];
                if (over[next] != parted || !found || found->plate != over[place]) {
                    continue;
                }
                over[next] = over[place];
                raised.emplace_back(over[place],
                                    held(over[place], cell_center(grid_.cell(next))).value_or(0));
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

glm::dmat3 place_drag(DVec3 at, double share) noexcept {
    // A disc of radius r drags as its area by r^2 / 2 spinning; its r^2 is 4 share in radians.
    const glm::dmat3 radial = glm::outerProduct(at, at);
    return share * ((glm::dmat3{1.0} - radial) + (2.0 * share * radial));
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
