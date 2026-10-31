#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/isostasy.hpp>
#include <hog/planet/lithosphere.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tectonics.hpp>
#include <hog/planet/volume.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <numbers>
#include <numeric>
#include <optional>
#include <span>
#include <tuple>
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

// Presses `crust`'s loose sediment buried past `lithified` into rock, and bakes what's buried
// past `hardened` into crystalline rock.
void bury(Crust& crust) {
    float& loose = crust.cover[loose_sediment];
    crust.cover[sedimentary_rock] += std::max(loose - lithified, 0.0F);
    loose = std::min(loose, lithified);
    float depth = loose;
    for (std::size_t kind = sedimentary_rock; kind < cover_kinds; ++kind) {
        crust.cover[kind] = std::min(crust.cover[kind], std::max(hardened - depth, 0.0F));
        depth += crust.cover[kind];
    }
}

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

// Takes `off` kilometres off the top of `crust`, its cover first.
void strip(Crust& crust, float off) {
    float left = off;
    for (float& layer : crust.cover) {
        const float taken = std::min(layer, left);
        layer -= taken;
        left -= taken;
    }
    crust.thickness -= off;
}

// Lays `laid` kilometres over `crust`, `soft` of them on top, burying what was on top before:
// under crystalline rock, it's as good as crystalline.
void lay(Crust& crust, float laid, const Cover& soft) {
    if (laid > std::accumulate(soft.begin(), soft.end(), 0.0F)) {
        crust.cover = soft;
    } else {
        for (std::size_t kind = 0; kind < cover_kinds; ++kind) {
            crust.cover[kind] += soft[kind];
        }
    }
    bury(crust);
    crust.thickness += laid;
}

// Each cell next to the plate's crust at `cells` on its own grid not yet marked `mark`, as indices
// into its cells, now marked.
std::vector<std::size_t> crust_around(const Lithosphere::Plate& plate, const LevelGrid& grid,
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
            if (marks[at] != mark) {
                marks[at] = mark;
                around.push_back(at);
            }
        }
    }
    return around;
}

// Raises the plate's crust at `ring` toward `strongest` kilometres, all by the same share of its
// room, with up to `excess` kilometres deep over square metres; says how much it took. Ocean floor
// buried so becomes new continent.
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
        if (crust.type != CrustType::Continental) {
            crust.type = CrustType::Continental;
            crust.age = 0.0F;
        }
    }
    return std::min(space, excess);
}

// Continental crust over `strongest` kilometres sheds what it can't hold up into the plate's
// nearest crust with room for it, volume for volume, filling that up to `strongest` a ring of cells
// at a time out from it. What finds no room on the whole plate founders into the mantle; says how
// much, in kilometres deep over square metres.
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
            const std::vector<std::size_t> around = crust_around(plate, grid, ring, marks, mark);
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
             ring = with_room(crust_around(plate, grid, ring, marks, mark))) {
            excess -= fill(plate, grid, ring, excess, strongest);
        }
        foundered += excess;
    }
    return foundered;
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

// Of each place of `grid`, how much of the mantle under it continent `on_top` holds the heat in,
// as it mixes with the mantle around it out to some `depth` of the planet's radius, the nearer
// the more. Summed on the coarsest grid whose cells are still small beside that, as it mixes
// over nothing finer.
std::vector<double> insulated(const LevelGrid& grid,
                              std::span<const std::optional<Lithosphere::Found>> on_top,
                              double depth, jobs::JobSystem* jobs) {
    // A cube face spans a quarter turn, halved at each level.
    int level = 0;
    while (level < grid.level() && std::numbers::pi / 2.0 / std::exp2(level) > depth / 4.0) {
        ++level;
    }
    double sphere = 0.0;
    for (std::uint32_t place = 0; place < grid.size(); ++place) {
        sphere += static_cast<double>(grid.area(place));
    }
    const LevelGrid coarse{level, std::sqrt(sphere / (4.0 * std::numbers::pi))};
    std::vector<double> covered(coarse.size(), 0.0);
    std::vector<double> under(coarse.size(), 0.0);
    for (std::uint32_t place = 0; place < grid.size(); ++place) {
        const std::uint32_t at = coarse.index(cell_at(cell_center(grid.cell(place)), level));
        const auto area = static_cast<double>(grid.area(place));
        under[at] += area;
        if (const std::optional<Lithosphere::Found>& found = on_top[place];
            found && found->crust.type == CrustType::Continental) {
            covered[at] += area;
        }
    }
    std::vector<DVec3> centres(coarse.size());
    for (std::uint32_t at = 0; at < coarse.size(); ++at) {
        centres[at] = cell_center(coarse.cell(at));
    }
    constexpr auto unit = static_cast<double>(fixed_unit<fine_fraction_bits>);
    std::vector<std::int64_t> share(coarse.size(), 0);
    each(jobs, coarse.size(), 16, [&](std::uint32_t here, std::uint32_t /*thread*/) {
        double warm = 0.0;
        double all = 0.0;
        for (std::uint32_t there = 0; there < coarse.size(); ++there) {
            const double apart =
                std::acos(std::clamp(glm::dot(centres[here], centres[there]), -1.0, 1.0)) / depth;
            const double near = std::exp(-apart * apart);
            warm += near * covered[there];
            all += near * under[there];
        }
        share[here] = std::llround(warm / all * unit);
    });
    std::vector<double> found(grid.size());
    for (std::uint32_t place = 0; place < grid.size(); ++place) {
        found[place] = fixed_to_double<fine_fraction_bits>(coarse.smooth(share, grid.cell(place)));
    }
    return found;
}

// Whether ocean floor has been thickened as far over the basalt it formed as as it takes to float
// floor of today's as continent: what's added is arc rock and sediment, lighter than basalt.
bool floats_as_continent(const Crust& crust) {
    return crust.type == CrustType::Oceanic &&
           crust.thickness - crust.basalt + Crust{}.basalt >= Lithosphere::arc_continent;
}

// How strong continent is over mantle `heat` warm, `softer` times weaker than today's.
double continent_strength(float heat, Strength strength, double softer) {
    return strength.cool * std::pow(strength.warm / strength.cool, heat) / softer;
}

// Where in a plate's cells a cell of its own grid is, if it has crust there.
std::optional<std::size_t> index_in(const Lithosphere::Plate& plate, std::uint32_t cell) {
    const auto found = std::ranges::lower_bound(plate.cells, cell);
    if (found == plate.cells.end() || *found != cell) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(plate.cells.begin(), found));
}

// The way along the surface from a plate's cell at `index` to the continent around it on the
// same plate that holds less than `weight`, if any, in the plate's own frame.
std::optional<DVec3> weak_margin(const Lithosphere& lithosphere, std::uint32_t plate,
                                 std::size_t index, double weight, Strength strength) {
    const LevelGrid& grid = lithosphere.grid();
    const Lithosphere::Plate& carrier = lithosphere.plates()[plate];
    const DVec3 at = cell_center(grid.cell(carrier.cells[index]));
    DVec3 toward{0.0};
    for (const std::uint32_t next : grid.neighbors(carrier.cells[index])) {
        const std::optional<std::size_t> there = index_in(carrier, next);
        if (!there || carrier.crust[*there].type != CrustType::Continental) {
            continue;
        }
        const DVec3 middle = cell_center(grid.cell(next));
        const float heat =
            lithosphere
                .heat()[grid.index(cell_at(lithosphere.to_planet(plate, middle), grid.level()))];
        if (continent_strength(heat, strength, lithosphere.softer()) >= weight) {
            continue;
        }
        DVec3 offset = middle - at;
        offset -= glm::dot(offset, at) * at;
        toward += glm::normalize(offset);
    }
    if (toward == DVec3{0.0}) {
        return std::nullopt;
    }
    return glm::normalize(toward);
}

// A plate's cell as a line across the plate meets it: where it lies, how the mantle drags on
// it, how long a line across it is and how strong it is along that.
struct Stretch {
    DVec3 at{0.0};
    glm::dmat3 drag{0.0};
    double across = 0.0;
    double strong = 0.0;
};

// Each cell of a plate of `lithosphere` as a line across the plate meets it, cells of continent
// as strong as the mantle under them is cool.
std::vector<Stretch> stretches_of(const Lithosphere& lithosphere, std::uint32_t plate,
                                  const Forces& forces, Strength strength) {
    const LevelGrid& grid = lithosphere.grid();
    double sphere = 0.0;
    for (std::uint32_t place = 0; place < grid.size(); ++place) {
        sphere += static_cast<double>(grid.area(place));
    }
    const Lithosphere::Plate& carrier = lithosphere.plates()[plate];
    const double softer = lithosphere.softer();
    std::vector<Stretch> stretches(carrier.cells.size());
    for (std::size_t index = 0; index < stretches.size(); ++index) {
        const std::uint32_t cell = carrier.cells[index];
        const bool continental = carrier.crust[index].type == CrustType::Continental;
        Stretch& stretch = stretches[index];
        stretch.at = lithosphere.to_planet(plate, cell_center(grid.cell(cell)));
        const double share = static_cast<double>(grid.area(cell)) / sphere;
        stretch.drag = (continental ? forces.keel : 1.0) * place_drag(stretch.at, share);
        stretch.across = std::sqrt(4.0 * std::numbers::pi * share);
        if (continental) {
            const float heat = lithosphere.heat()[grid.index(cell_at(stretch.at, grid.level()))];
            stretch.strong = continent_strength(heat, strength, softer);
        } else {
            // Floor thickens as it cools, as its slab's pull grows.
            const auto age = static_cast<double>(carrier.crust[index].age);
            stretch.strong =
                strength.ocean * std::sqrt(std::min(age, forces.mature) / forces.mature) / softer;
        }
    }
    return stretches;
}

// A whole plate as the forces on it turn it: the mantle's drag on it, the torque on it, and the
// turn the one gives against the other.
struct Load {
    glm::dmat3 drag{0.0};
    DVec3 torque{0.0};
    DVec3 turn{0.0};
};

// How a line across a plate is pulled apart: how many times harder than it holds, and how fast,
// on the whole along it, its two sides would part on their own against unit drag.
struct Pull {
    double ratio = 0.0;
    double opening = 0.0;
};

// How a plate of `lithosphere` under `whole` is pulled apart along the line whose plane faces
// `facing`, with `pushed` the torque on the side it faces: what the other side puts on that one
// to keep it turning with the plate, against how strong the line is where that pulls it apart.
// Tension t at a place p on the line puts -t (p x facing) on the side. Across a plate from edge
// to edge it's taken as even along the line. A line on the plate all the way round can't hold
// so, as its levers cancel, but by pulling on one side and pushing on the other: the least
// tension that does varies along it as p x facing does.
Pull pulled(const Lithosphere& lithosphere, std::uint32_t plate, std::span<const Stretch> stretches,
            const Load& whole, DVec3 pushed, DVec3 facing) {
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
            round.push_back(std::atan2(glm::dot(stretch.at, second), glm::dot(stretch.at, first)));
        }
    }
    if (round.empty()) {
        return {};
    }
    std::ranges::sort(round);
    double gap = round.front() + (2.0 * std::numbers::pi) - round.back();
    double amid = round.back() + (0.5 * gap);
    for (std::size_t index = 1; index < round.size(); ++index) {
        if (const double between = round[index] - round[index - 1]; between > gap) {
            gap = between;
            amid = round[index - 1] + (0.5 * between);
        }
    }
    // The line passes between cells it crosses near their corners, so it runs off the plate only
    // where the plate has no crust amid its widest gap.
    const LevelGrid& grid = lithosphere.grid();
    const DVec3 gone = (std::cos(amid) * first) + (std::sin(amid) * second);
    const bool edge_to_edge =
        !index_in(lithosphere.plates()[plate],
                  grid.index(cell_at(lithosphere.to_plate(plate, gone), grid.level())));
    const DVec3 needed = pushed - (held * whole.turn);
    // Tension at a place as c . (p x facing), plus `even` all along.
    DVec3 c{0.0};
    double even = 0.0;
    const double det = (xx * yy) - (xy * xy);
    if (edge_to_edge) {
        if (glm::dot(levers, levers) == 0.0) {
            return {};
        }
        even = glm::dot(needed, levers) / glm::dot(levers, levers);
    } else if (det > 0.0) {
        const double bx = glm::dot(needed, first);
        const double by = glm::dot(needed, second);
        c = ((((yy * bx) - (xy * by)) / det) * first) + ((((xx * by) - (xy * bx)) / det) * second);
    } else {
        return {};
    }
    // Each side turning on its own as the forces on it would turn it.
    const glm::dmat3 rest = whole.drag - held;
    const DVec3 parting =
        glm::determinant(held) != 0.0 && glm::determinant(rest) != 0.0
            ? (glm::inverse(held) * pushed) - (glm::inverse(rest) * (whole.torque - pushed))
            : DVec3{0.0};
    double pulling = 0.0;
    double holds = 0.0;
    double opening = 0.0;
    double length = 0.0;
    for (const Stretch& stretch : stretches) {
        if (!on_line(stretch)) {
            continue;
        }
        const double tension = even + glm::dot(c, glm::cross(stretch.at, facing));
        if (tension > 0.0) {
            pulling += tension * stretch.across;
            holds += stretch.strong * stretch.across;
            opening +=
                std::max(glm::dot(glm::cross(parting, stretch.at), facing), 0.0) * stretch.across;
            length += stretch.across;
        }
    }
    if (holds == 0.0) {
        return {};
    }
    return {.ratio = pulling / holds, .opening = opening / length};
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
// each other everywhere they meet, as `speeds` says at each of the `edges`.
std::vector<bool> as_one(std::span<const Boundary> edges, std::span<const double> speeds,
                         std::size_t count, double slowest) {
    std::vector<double> anywhere(count * count, -1.0);
    for (std::size_t index = 0; index < edges.size(); ++index) {
        const Boundary& edge = edges[index];
        const std::size_t pair =
            (std::min(edge.plate, edge.other) * count) + std::max(edge.plate, edge.other);
        anywhere[pair] = std::max(anywhere[pair], speeds[index]);
    }
    std::vector<bool> joined(count * count, false);
    for (std::size_t pair = 0; pair < joined.size(); ++pair) {
        joined[pair] = anywhere[pair] >= 0.0 && anywhere[pair] < slowest;
    }
    return joined;
}

// Where two plates meet, how they drag on each other: on the first as -drag (w_first - w_second)
// with each turning at w, and as much back on the second.
struct Coupling {
    std::uint32_t first = 0;
    std::uint32_t second = 0;
    glm::dmat3 drag{0.0};
};

// The turns w that the mantle's `drags` on each plate, D w, and the `couplings` between them
// hold against the `torques` on each, by conjugate gradients, as the drags are symmetric and
// resist every turn.
std::vector<DVec3> turned_together(std::span<const glm::dmat3> drags,
                                   std::span<const Coupling> couplings,
                                   std::span<const DVec3> torques) {
    const std::size_t count = drags.size();
    const auto against = [&](std::span<const DVec3> turns) {
        std::vector<DVec3> held(count);
        for (std::size_t plate = 0; plate < count; ++plate) {
            held[plate] = drags[plate] * turns[plate];
        }
        for (const Coupling& coupling : couplings) {
            const DVec3 drag = coupling.drag * (turns[coupling.first] - turns[coupling.second]);
            held[coupling.first] += drag;
            held[coupling.second] -= drag;
        }
        return held;
    };
    const auto dot = [](std::span<const DVec3> one, std::span<const DVec3> other) {
        double sum = 0.0;
        for (std::size_t plate = 0; plate < one.size(); ++plate) {
            sum += glm::dot(one[plate], other[plate]);
        }
        return sum;
    };
    // Each plate's drag with all it meets held still, to ease each step by.
    std::vector<glm::dmat3> own{drags.begin(), drags.end()};
    for (const Coupling& coupling : couplings) {
        own[coupling.first] += coupling.drag;
        own[coupling.second] += coupling.drag;
    }
    std::vector<glm::dmat3> ease(count, glm::dmat3{0.0});
    for (std::size_t plate = 0; plate < count; ++plate) {
        if (glm::determinant(own[plate]) != 0.0) {
            ease[plate] = glm::inverse(own[plate]);
        }
    }
    const auto eased = [&](std::span<const DVec3> left) {
        std::vector<DVec3> out(count);
        for (std::size_t plate = 0; plate < count; ++plate) {
            out[plate] = ease[plate] * left[plate];
        }
        return out;
    };
    std::vector<DVec3> turns(count, DVec3{0.0});
    std::vector<DVec3> left{torques.begin(), torques.end()};
    std::vector<DVec3> way = eased(left);
    double fit = dot(left, way);
    const double close_enough = 1e-24 * dot(torques, torques);
    for (std::size_t step = 0; step < 6 * count && dot(left, left) > close_enough; ++step) {
        const std::vector<DVec3> held = against(way);
        const double along = dot(way, held);
        if (along <= 0.0) {
            break;
        }
        const double length = fit / along;
        for (std::size_t plate = 0; plate < count; ++plate) {
            turns[plate] += length * way[plate];
            left[plate] -= length * held[plate];
        }
        const std::vector<DVec3> next = eased(left);
        const double next_fit = dot(left, next);
        for (std::size_t plate = 0; plate < count; ++plate) {
            way[plate] = next[plate] + ((next_fit / fit) * way[plate]);
        }
        fit = next_fit;
    }
    return turns;
}

// How hard plates drag on each other across an edge of `kind`, as so many radians across of
// mantle dragging under a plate.
double drag_across(Boundary::Kind kind, const Forces& forces) {
    switch (kind) {
    case Boundary::Kind::Ridge:
        return 0.0;
    case Boundary::Kind::Collision:
        return forces.suture;
    case Boundary::Kind::Trench:
    case Boundary::Kind::Arc:
        return forces.megathrust;
    }
    return 0.0;
}

// How each two plates meeting along `edges` of `grid` drag on each other, a pair at a time.
std::vector<Coupling> couplings(const LevelGrid& grid, std::span<const Boundary> edges,
                                const Forces& forces) {
    // Each edge is found from both sides, so takes half the drag along it, as a strip of mantle
    // so wide would.
    std::vector<Coupling> found;
    for (const Boundary& edge : edges) {
        const double width = drag_across(edge.kind, forces);
        if (width == 0.0) {
            continue;
        }
        const DVec3 at = cell_center(grid.cell(edge.place));
        found.push_back({.first = std::min(edge.plate, edge.other),
                         .second = std::max(edge.plate, edge.other),
                         .drag = (0.5 * width * edge.length / (4.0 * std::numbers::pi)) *
                                 (glm::dmat3{1.0} - glm::outerProduct(at, at))});
    }
    std::ranges::sort(found, [](const Coupling& one, const Coupling& other) {
        return std::tie(one.first, one.second) < std::tie(other.first, other.second);
    });
    std::vector<Coupling> pairs;
    for (const Coupling& coupling : found) {
        if (!pairs.empty() && pairs.back().first == coupling.first &&
            pairs.back().second == coupling.second) {
            pairs.back().drag += coupling.drag;
        } else {
            pairs.push_back(coupling);
        }
    }
    return pairs;
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
      slabs_(plates_.size()), rifts_(plates_.size()), heat_(grid_.size(), 0.0F) {
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
            crust.faulted += aged;
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
                fresh[owner[next]].emplace_back(
                    grid_.index(cell_at(own, grid_.level())),
                    Crust{.thickness = ridge_crust_, .basalt = ridge_crust_});
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

namespace {

// Of the places of the grid `under` the sea's level, those of the open sea: the largest body of
// them joined together, the rest cut off from it.
[[nodiscard]] std::vector<bool> open_sea(const LevelGrid& grid, const std::vector<bool>& under) {
    constexpr auto none = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> body(grid.size(), none);
    std::uint32_t largest = none;
    double widest = 0.0;
    std::uint32_t count = 0;
    for (std::uint32_t first = 0; first < grid.size(); ++first) {
        if (!under[first] || body[first] != none) {
            continue;
        }
        double area = 0.0;
        std::vector<std::uint32_t> frontier{first};
        body[first] = count;
        while (!frontier.empty()) {
            const std::uint32_t place = frontier.back();
            frontier.pop_back();
            area += static_cast<double>(grid.area(place));
            for (const std::uint32_t next : grid.neighbors(place)) {
                if (under[next] && body[next] == none) {
                    body[next] = count;
                    frontier.push_back(next);
                }
            }
        }
        if (area > widest) {
            widest = area;
            largest = count;
        }
        ++count;
    }
    std::vector<bool> open(grid.size(), false);
    for (std::uint32_t place = 0; place < grid.size(); ++place) {
        open[place] = under[place] && body[place] == largest;
    }
    return open;
}

// Kilometres laid at each place of sediment `carried` into the `open` sea there, in square metre
// kilometres: from each place it comes in at, filling the nearest water up to `sea_level` first,
// out across what's filled already, as deltas and shelves build out from the coast.
[[nodiscard]] std::vector<double> laid_down(const LevelGrid& grid, std::span<const double> carried,
                                            std::span<const std::int64_t> heights,
                                            std::int64_t sea_level, const std::vector<bool>& open) {
    constexpr double metres_per_km = 1'000.0;
    std::vector<double> room(grid.size(), 0.0);
    for (std::uint32_t place = 0; place < grid.size(); ++place) {
        if (open[place]) {
            room[place] = std::max(fixed_to_double(sea_level - heights[place]), 0.0) /
                          metres_per_km * static_cast<double>(grid.area(place));
        }
    }
    std::vector<double> laid(grid.size(), 0.0);
    std::vector<std::uint32_t> seen(grid.size(), 0);
    std::uint32_t search = 0;
    std::vector<std::uint32_t> nearest;
    for (std::uint32_t entry = 0; entry < grid.size(); ++entry) {
        double left = carried[entry];
        if (!open[entry] || left <= 0.0) {
            continue;
        }
        ++search;
        nearest.assign(1, entry);
        seen[entry] = search;
        for (std::size_t at = 0; at < nearest.size() && left > 0.0; ++at) {
            const std::uint32_t place = nearest[at];
            const double taken = std::min(left, room[place]);
            room[place] -= taken;
            laid[place] += taken;
            left -= taken;
            for (const std::uint32_t next : grid.neighbors(place)) {
                if (open[next] && seen[next] != search) {
                    seen[next] = search;
                    nearest.push_back(next);
                }
            }
        }
        // The whole sea filled, it's laid where it came in.
        laid[entry] += left;
    }
    for (std::uint32_t place = 0; place < grid.size(); ++place) {
        laid[place] /= static_cast<double>(grid.area(place));
    }
    return laid;
}

} // namespace

void Lithosphere::erode(std::span<const float> worn, std::span<const std::int64_t> heights,
                        std::int64_t sea_level) {
    HOG_ASSERT(worn.size() == grid_.size(), "one depth to each place");
    HOG_ASSERT(heights.size() == grid_.size(), "one height to each place");
    const std::vector<std::optional<Found>> on_top = tops();
    std::vector<bool> under(grid_.size());
    for (std::uint32_t place = 0; place < grid_.size(); ++place) {
        under[place] = on_top[place] && heights[place] < sea_level;
    }
    const std::vector<bool> drowned = open_sea(grid_, under);
    // Each place's nearest crust under the sea, out from the coasts a ring of places at a time.
    std::vector<std::uint32_t> sea(grid_.size(), parted);
    std::vector<std::uint32_t> frontier;
    for (std::uint32_t place = 0; place < grid_.size(); ++place) {
        if (drowned[place]) {
            sea[place] = place;
            frontier.push_back(place);
        }
    }
    while (!frontier.empty()) {
        std::vector<std::uint32_t> further;
        for (const std::uint32_t place : frontier) {
            for (const std::uint32_t next : grid_.neighbors(place)) {
                if (sea[next] == parted) {
                    sea[next] = sea[place];
                    further.push_back(next);
                }
            }
        }
        frontier = std::move(further);
    }
    const auto crust_at = [&](std::uint32_t place) -> Crust& {
        const std::uint32_t plate = on_top[place]->plate;
        return plates_[plate].crust[held(plate, cell_center(grid_.cell(place))).value_or(0)];
    };
    std::vector<double> carried(grid_.size(), 0.0);
    for (std::uint32_t place = 0; place < grid_.size(); ++place) {
        const std::optional<Found>& found = on_top[place];
        if (!found || drowned[place] || worn[place] <= 0.0F || sea[place] == parted) {
            continue;
        }
        Crust& land = crust_at(place);
        const float off = std::min(worn[place], land.thickness);
        strip(land, off);
        carried[sea[place]] += static_cast<double>(off) * static_cast<double>(grid_.area(place));
    }
    const std::vector<double> laid = laid_down(grid_, carried, heights, sea_level, drowned);
    for (std::uint32_t place = 0; place < grid_.size(); ++place) {
        if (laid[place] > 0.0) {
            const auto thick = static_cast<float>(laid[place]);
            lay(crust_at(place), thick, Cover{thick});
        }
    }
}

std::vector<Cover> Lithosphere::cover() const {
    const std::vector<std::optional<Found>> on_top = tops();
    std::vector<Cover> soft(grid_.size(), Cover{});
    for (std::uint32_t place = 0; place < grid_.size(); ++place) {
        if (const std::optional<Found>& found = on_top[place]) {
            soft[place] = found->crust.cover;
        }
    }
    return soft;
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
    std::vector<std::optional<std::uint32_t>> overs;
    std::vector<std::tuple<std::uint32_t, std::size_t, std::uint32_t>> scraped;
    for (std::uint32_t plate = 0; plate < plates_.size(); ++plate) {
        const Plate& carrier = plates_[plate];
        const auto cells = static_cast<std::uint32_t>(carrier.cells.size());
        overs.assign(cells, std::nullopt);
        each(jobs_, cells, places_per_job, [&](std::uint32_t index, std::uint32_t /*thread*/) {
            const Crust& crust = carrier.crust[index];
            if (crust.type == CrustType::Oceanic) {
                const DVec3 at = to_planet(plate, cell_center(grid_.cell(carrier.cells[index])));
                overs[index] = overlying(plate, crust.type, at, overrides);
            }
        });
        for (std::uint32_t index = 0; index < cells; ++index) {
            if (const std::optional<std::uint32_t>& over = overs[index]) {
                diving[plate].push_back(index);
                const DVec3 at = to_planet(plate, cell_center(grid_.cell(carrier.cells[index])));
                trenches.push_back(grid_.index(cell_at(at, grid_.level())));
                scraped.emplace_back(plate, index, *over);
            }
        }
    }
    for (const auto& [plate, index, over] : scraped) {
        scrape_off(plate, index, over, arcs);
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

void Lithosphere::scrape_off(std::uint32_t plate, std::size_t index, std::uint32_t over,
                             const Arcs& arcs) {
    // Plates turn as fast as Earth's, so their speeds are Earth's at its size.
    constexpr double mm_a_year_per_radian_a_million = 6'371.0;
    const Crust& floor = plates_[plate].crust[index];
    const DVec3 at = to_planet(plate, cell_center(grid_.cell(plates_[plate].cells[index])));
    const std::optional<std::size_t> onto = held(over, at);
    const double closing =
        glm::length(velocity(plate, at) - velocity(over, at)) * mm_a_year_per_radian_a_million;
    const float piled = floor.thickness - floor.basalt - arcs.subducted;
    if (!onto || piled <= 0.0F || closing >= static_cast<double>(arcs.scraping)) {
        return;
    }
    Crust& front = plates_[over].crust[*onto];
    // Volume for volume, between cells of the same grid in different places.
    const auto share =
        static_cast<float>(static_cast<double>(grid_.area(plates_[plate].cells[index])) /
                           static_cast<double>(grid_.area(plates_[over].cells[*onto])));
    Cover cover = floor.cover;
    for (float& layer : cover) {
        layer *= share;
    }
    lay(front, piled * share, cover);
    if (floats_as_continent(front)) {
        front.type = CrustType::Continental;
        front.raised = 0.0F;
    }
}

std::vector<Plume> plumes(std::uint64_t seed, double radius, const Plumes& settings) {
    constexpr double earth_radius = 6.371e6;
    constexpr std::uint64_t salt = 0x706c756d65ULL;
    const double size = radius / earth_radius;
    const auto count = static_cast<std::size_t>(settings.count);
    std::vector<Plume> rising(count);
    for (std::size_t index = 0; index < count; ++index) {
        const double buoyancy =
            settings.weakest * std::pow(settings.strongest / settings.weakest,
                                        unit(hash_values(seed, salt, index, 1U)));
        rising[index] = {.at = direction(hash_values(seed, salt, index, 0U)),
                         .magma = buoyancy * settings.magma * size * size};
    }
    return rising;
}

void Lithosphere::feed(std::span<const Plume> plumes, double years, const Plumes& settings) {
    constexpr double km2_per_m2 = 1e-6;
    const double across = std::sqrt(4.0 * std::numbers::pi / static_cast<double>(grid_.size()));
    for (const Plume& plume : plumes) {
        const std::optional<Found> found = top(plume.at);
        if (!found) {
            continue;
        }
        Plate& plate = plates_[found->plate];
        // What lay over it so long ago has since been carried on by the plate's turn, so its
        // track runs back along that turn; laid twice to each place it crosses, so none is missed.
        const double swept =
            std::abs(plate.rate) * years * glm::length(glm::cross(plate.pole, plume.at));
        const int samples = static_cast<int>(std::ceil(2.0 * swept / across)) + 1;
        const double each = plume.magma * years / samples;
        for (int sample = 0; sample < samples; ++sample) {
            const double ago = years * (sample + 0.5) / samples;
            const DVec3 at = glm::angleAxis(plate.rate * ago, plate.pole) * plume.at;
            const std::optional<std::size_t> index = held(found->plate, at);
            if (!index) {
                continue;
            }
            Crust& crust = plate.crust[*index];
            const auto thicker = static_cast<float>(
                each / (static_cast<double>(grid_.area(plate.cells[*index])) * km2_per_m2));
            crust.cover[volcanic_rock] += thicker * settings.erupted;
            bury(crust);
            crust.thickness += thicker;
        }
    }
}

std::vector<Impact> impacts(std::uint64_t seed, double ago, double years, double smallest,
                            const Impacts& settings) {
    constexpr double earth_km = 6'371.0;
    constexpr double years_per_billion = 1'000.0;
    constexpr std::uint64_t salt = 0x696d70616374ULL;
    // Over the years since, the heavy bombardment's share as if it were so many more years.
    const double falling = settings.falling / years_per_billion;
    const double heavy =
        settings.heavy / falling * (std::exp(falling * (ago + years)) - std::exp(falling * ago));
    const double expected = settings.rate * 4.0 * std::numbers::pi * earth_km * earth_km *
                            (years + heavy) *
                            std::pow(smallest * earth_km / settings.smallest, -settings.slope);
    const std::uint64_t when = hash_values(seed, salt, std::bit_cast<std::uint64_t>(ago));
    // As many as strike by then, waiting for each in turn.
    std::vector<Impact> struck;
    double waited = -std::log1p(-unit(hash_values(when, 0U, 0U)));
    for (std::uint64_t index = 0; waited < expected; ++index) {
        struck.push_back({.at = direction(hash_values(when, index, 1U)),
                          .diameter = smallest * std::pow(1.0 - unit(hash_values(when, index, 2U)),
                                                          -1.0 / settings.slope)});
        waited -= std::log1p(-unit(hash_values(when, index + 1, 0U)));
    }
    return struck;
}

namespace {

// How stiffly each of `plate`'s crust bends, on a planet of `radius` metres.
[[nodiscard]] std::vector<double> stiffness_of(const Lithosphere::Plate& plate, double radius) {
    std::vector<double> stiff(plate.crust.size());
    std::ranges::transform(plate.crust, stiff.begin(), [&](const Crust& crust) {
        return stiffness(elastic_thickness(crust), radius);
    });
    return stiff;
}

} // namespace

void Lithosphere::bend(double radius) {
    constexpr double metres_per_km = 1'000.0;
    // Each on its own cells, so every column bends under its neighbours' loads as under its own.
    each(jobs_, static_cast<std::uint32_t>(plates_.size()), 1,
         [&](std::uint32_t index, std::uint32_t /*thread*/) {
             Plate& plate = plates_[index];
             const std::vector<double> stiff = stiffness_of(plate, radius);
             const std::vector<double> alone = sinking_alone(grid_, plate.cells, stiff);
             std::vector<double> loads(plate.crust.size());
             for (std::size_t cell = 0; cell < plate.crust.size(); ++cell) {
                 Crust& crust = plate.crust[cell];
                 // What floated while it was weaker stays floating as it stiffens.
                 const auto holding = static_cast<float>(1.0 - alone[cell]);
                 if (holding > crust.holding) {
                     crust.held *= crust.holding / holding;
                 }
                 crust.holding = holding;
                 crust.held += crust.thickness - crust.borne.value_or(crust.thickness);
                 crust.borne = crust.thickness;
                 const double buoyancy =
                     crust.type == CrustType::Continental ? continental_buoyancy : oceanic_buoyancy;
                 loads[cell] = (1.0 - buoyancy) * metres_per_km * static_cast<double>(crust.held);
             }
             const std::vector<double> bent = deflected(grid_, plate.cells, loads, stiff);
             for (std::size_t cell = 0; cell < plate.crust.size(); ++cell) {
                 plate.crust[cell].sunk =
                     static_cast<float>((bent[cell] - loads[cell]) / metres_per_km);
             }
         });
}

namespace {

// The continent of `plate` once its lower crust has crept for `years`, each kilometre of channel
// under its brittle lid flowing `ease` over its viscosity cubed, and its ground lowering `alone`
// of each kilometre more under it.
void creep_plate(const LevelGrid& grid, Lithosphere::Plate& plate, std::span<const double> alone,
                 double years, double ease, const LowerCrust& lower) {
    constexpr double gas = 8.314;
    constexpr double metres_per_km = 1'000.0;
    constexpr int passes = 32;
    constexpr double settled = 1e-4;
    std::vector<std::uint32_t> cells;
    std::vector<std::size_t> held_at;
    std::vector<double> sunk;
    std::vector<double> lowers;
    std::vector<double> runny;
    for (std::size_t cell = 0; cell < plate.crust.size(); ++cell) {
        const Crust& crust = plate.crust[cell];
        if (crust.type != CrustType::Continental) {
            continue;
        }
        const double viscosity = std::max(
            lower.viscosity * std::exp(lower.activation / gas *
                                       ((1.0 / moho_temperature(crust)) - (1.0 / lower.reference))),
            lower.runniest);
        cells.push_back(plate.cells[cell]);
        held_at.push_back(cell);
        // Ground already borne stays held as it was bent, so only what's added since sinks alone.
        sunk.push_back(static_cast<double>(crust.sunk) +
                       ((1.0 - alone[cell]) * (1.0 - continental_buoyancy) *
                        static_cast<double>(crust.borne.value_or(crust.thickness))));
        lowers.push_back(1.0 - (alone[cell] * (1.0 - continental_buoyancy)));
        runny.push_back(ease / viscosity);
    }
    if (cells.empty()) {
        return;
    }
    const auto channel_of = [&](double thick) {
        return std::clamp(thick - lower.brittle, 0.0, lower.channel);
    };
    std::vector<double> thickness(cells.size());
    for (std::size_t node = 0; node < cells.size(); ++node) {
        thickness[node] = static_cast<double>(plate.crust[held_at[node]].thickness);
    }
    // Its channel flows the slower the thinner it's drawn, down to not at all once drained, so each
    // flows as it would by the end of the step, worked toward from where it starts. A channel can't
    // then flow out more than it holds.
    std::vector<double> ends = thickness;
    std::vector<double> eases(cells.size());
    std::vector<double> crept = thickness;
    for (int pass = 0; pass < passes; ++pass) {
        for (std::size_t node = 0; node < cells.size(); ++node) {
            const double channel = channel_of(ends[node]) * metres_per_km;
            eases[node] = channel * channel * channel * runny[node];
        }
        crept = flowed(grid, cells, thickness, sunk, lowers, eases, years, crept);
        double moved = 0.0;
        for (std::size_t node = 0; node < cells.size(); ++node) {
            moved = std::max(moved, std::abs(crept[node] - ends[node]));
            ends[node] = (ends[node] + crept[node]) / 2.0;
        }
        if (moved < settled) {
            break;
        }
    }
    for (std::size_t node = 0; node < cells.size(); ++node) {
        plate.crust[held_at[node]].thickness = static_cast<float>(ends[node]);
    }
}

} // namespace

void Lithosphere::creep(double years, double radius, LowerCrust lower) {
    constexpr double earth_radius = 6'371'000.0;
    constexpr double seconds_a_million_years = 3.15576e13;
    constexpr double crust_density = 2'800.0;
    // A channel's flux under a pressure gradient, h³/(12η) ∇P (Turcotte & Schubert 2002), the
    // pressure the weight of the crust over it, its lengths along the ground Earth's scaled.
    const double ease = crust_density * earth_gravity / 12.0 * seconds_a_million_years *
                        std::pow(radius / earth_radius, 2.0);
    each(jobs_, static_cast<std::uint32_t>(plates_.size()), 1,
         [&](std::uint32_t index, std::uint32_t /*thread*/) {
             Plate& plate = plates_[index];
             creep_plate(grid_, plate,
                         sinking_alone(grid_, plate.cells, stiffness_of(plate, radius)), years,
                         ease, lower);
         });
}

std::vector<double> Lithosphere::lowering(double radius) const {
    std::vector<std::vector<double>> alone(plates_.size());
    for (std::size_t plate = 0; plate < plates_.size(); ++plate) {
        alone[plate] =
            sinking_alone(grid_, plates_[plate].cells, stiffness_of(plates_[plate], radius));
    }
    const std::vector<std::optional<Found>> on_top = tops();
    std::vector<double> lowers(grid_.size(), continental_buoyancy);
    for (std::uint32_t place = 0; place < grid_.size(); ++place) {
        if (const std::optional<Found>& found = on_top[place]) {
            const std::optional<std::size_t> index =
                held(found->plate, cell_center(grid_.cell(place)));
            const double share = index ? alone[found->plate][*index] : 1.0;
            const double buoyancy = found->crust.type == CrustType::Continental
                                        ? continental_buoyancy
                                        : oceanic_buoyancy;
            lowers[place] = 1.0 - (share * (1.0 - buoyancy));
        }
    }
    return lowers;
}

void Lithosphere::strike(std::span<const Impact> impacts, Craters craters) {
    constexpr double earth_km = 6'371.0;
    if (impacts.empty()) {
        return;
    }
    // Each place's crust on top and where it lies, found once for every impact.
    const std::vector<std::optional<Found>> on_top = tops();
    std::vector<Crust*> crust_at(grid_.size(), nullptr);
    std::vector<DVec3> centres(grid_.size());
    for (std::uint32_t place = 0; place < grid_.size(); ++place) {
        centres[place] = cell_center(grid_.cell(place));
        if (const std::optional<Found>& found = on_top[place]) {
            const std::uint32_t plate = found->plate;
            crust_at[place] = &plates_[plate].crust[held(plate, centres[place]).value_or(0)];
        }
    }
    std::vector<double> apart(grid_.size());
    for (const Impact& impact : impacts) {
        const double transient = std::pow(
            impact.diameter * earth_km * std::pow(craters.simple, 0.13) / 1.17, 1.0 / 1.13);
        const double deepest = craters.excavated * transient;
        const double rim = transient / 2.0 / earth_km;
        double dug = 0.0;
        double around = 0.0;
        for (std::uint32_t place = 0; place < grid_.size(); ++place) {
            if (crust_at[place] == nullptr) {
                continue;
            }
            const DVec3 at = centres[place];
            apart[place] =
                std::atan2(glm::length(glm::cross(at, impact.at)), glm::dot(at, impact.at));
            const auto area = static_cast<double>(grid_.area(place));
            if (apart[place] < rim) {
                const double out = apart[place] / rim;
                Crust& crust = *crust_at[place];
                const float off =
                    std::min(static_cast<float>(deepest * (1.0 - (out * out))), crust.thickness);
                strip(crust, off);
                dug += static_cast<double>(off) * area;
            } else {
                around += area * std::pow(rim / apart[place], 3.0);
            }
        }
        if (dug <= 0.0 || around <= 0.0) {
            continue;
        }
        for (std::uint32_t place = 0; place < grid_.size(); ++place) {
            if (crust_at[place] != nullptr && apart[place] >= rim) {
                const auto laid =
                    static_cast<float>(dug * std::pow(rim / apart[place], 3.0) / around);
                lay(*crust_at[place], laid, Cover{laid});
            }
        }
    }
}

std::vector<std::optional<std::pair<std::uint32_t, std::size_t>>>
Lithosphere::going_under(std::uint32_t plate, const std::vector<bool>& overrides) const {
    const Plate& carrier = plates_[plate];
    const auto cells = static_cast<std::uint32_t>(carrier.cells.size());
    std::vector<std::optional<std::pair<std::uint32_t, std::size_t>>> unders(cells);
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
    return unders;
}

std::vector<std::size_t> Lithosphere::along_fault(std::uint32_t plate, std::uint32_t over, DVec3 at,
                                                  double reach, std::size_t onto) const {
    const DVec3 closing = velocity(plate, at) - velocity(over, at);
    const double length = glm::length(closing);
    if (length <= 0.0) {
        return {onto};
    }
    const DVec3 way = closing / length;
    const double across = std::sqrt(4.0 * std::numbers::pi / static_cast<double>(grid_.size()));
    std::vector<std::size_t> cells(
        static_cast<std::size_t>(std::max(std::ceil(2.0 * reach / across), 1.0)));
    for (std::size_t step = 0; step < cells.size(); ++step) {
        const double along =
            reach * (static_cast<double>(step) + 0.5) / static_cast<double>(cells.size());
        cells[step] = held(over, glm::normalize((at * std::cos(along)) + (way * std::sin(along))))
                          .value_or(onto);
    }
    return cells;
}

void Lithosphere::collide(Ranges ranges, LowerCrust lower) {
    constexpr double earth_radius_km = 6'371.0;
    constexpr double degrees = std::numbers::pi / 180.0;
    // A cell of continent goes under once its middle lies under another plate's.
    const std::vector<bool> overrides = overriding();
    std::vector<std::vector<std::size_t>> scraped(plates_.size());
    struct Pile {
        std::uint32_t plate;
        std::size_t onto;
        float thickness;
        Cover cover;
        bool beneath = false;
    };
    std::vector<Pile> piles;
    for (std::uint32_t plate = 0; plate < plates_.size(); ++plate) {
        const Plate& carrier = plates_[plate];
        const auto unders = going_under(plate, overrides);
        for (std::size_t index = 0; index < unders.size(); ++index) {
            const std::optional<std::pair<std::uint32_t, std::size_t>>& under = unders[index];
            if (!under) {
                continue;
            }
            const Crust& crust = carrier.crust[index];
            const auto [over, onto] = *under;
            const Plate& top = plates_[over];
            const auto from = static_cast<double>(grid_.area(carrier.cells[index]));
            // Volume for volume, between cells of the same grid in different places.
            const auto share = [&](std::size_t to) {
                return static_cast<float>(from / static_cast<double>(grid_.area(top.cells[to])));
            };
            const float brittle = std::min(crust.thickness, static_cast<float>(lower.brittle));
            Cover cover = crust.cover;
            for (float& layer : cover) {
                layer *= share(onto);
            }
            piles.push_back(
                {.plate = over, .onto = onto, .thickness = brittle * share(onto), .cover = cover});
            scraped[plate].push_back(index);
            const float warm = crust.thickness - brittle;
            if (warm <= 0.0F) {
                continue;
            }
            // Down to the base of the crust on top, in Earth's lengths as the crust's thickness is.
            const double reach = static_cast<double>(top.crust[onto].thickness) /
                                 std::tan(static_cast<double>(ranges.dip) * degrees) /
                                 earth_radius_km;
            const std::vector<std::size_t> fault = along_fault(
                plate, over, to_planet(plate, cell_center(grid_.cell(carrier.cells[index]))), reach,
                onto);
            for (const std::size_t under_it : fault) {
                piles.push_back(
                    {.plate = over,
                     .onto = under_it,
                     .thickness = warm / static_cast<float>(fault.size()) * share(under_it),
                     .cover = {},
                     .beneath = true});
            }
        }
    }
    for (const Pile& pile : piles) {
        Crust& crust = plates_[pile.plate].crust[pile.onto];
        // Thrust over it, its own soft rock on top, or under it, leaving its top as it was.
        if (pile.beneath) {
            crust.thickness += pile.thickness;
        } else {
            lay(crust, pile.thickness, pile.cover);
        }
        crust.raised = 0.0F;
    }
    for (std::uint32_t plate = 0; plate < plates_.size(); ++plate) {
        remove_crust(plates_[plate], std::move(scraped[plate]));
        foundered_ += collapse(plates_[plate], grid_, ranges.strongest) * 1e-6;
    }
}

namespace {

// How rock moving at `moving` against the rock across an edge facing `toward` slips: by whichever
// it does more, close or part across it or slide along it.
[[nodiscard]] Slip slip_of(DVec3 moving, DVec3 toward) {
    const double across = glm::dot(moving, toward);
    if (glm::length(moving - (across * toward)) > std::abs(across)) {
        return Slip::Strike;
    }
    return across > 0.0 ? Slip::Reverse : Slip::Normal;
}

} // namespace

void Lithosphere::fault(std::span<const Boundary> edges) {
    for (const Boundary& edge : edges) {
        const DVec3 at = cell_center(grid_.cell(edge.place));
        const std::optional<std::size_t> index = held(edge.plate, at);
        if (!index) {
            continue;
        }
        Crust& crust = plates_[edge.plate].crust[*index];
        crust.fault = slip_of(velocity(edge.plate, at) - velocity(edge.other, at), edge.normal);
        crust.faulted = 0.0F;
    }
}

std::vector<Fault> Lithosphere::faults(std::uint64_t seed) const {
    constexpr std::uint64_t salt = 0x6661756c74ULL;
    const std::vector<std::optional<Found>> on_top = tops();
    std::vector<Fault> at_place(grid_.size());
    for (std::uint32_t place = 0; place < grid_.size(); ++place) {
        const Crust crust = on_top[place].value_or(Found{}).crust;
        at_place[place] = {.place = place,
                           .slip = crust.fault,
                           .rate = 0.0,
                           .idle = crust.faulted,
                           .stress = static_cast<float>(unit(hash_values(seed, salt, place)))};
    }
    // Where plates still move against each other, as fast as they do now; of several, the fastest.
    for (const Boundary& edge : boundaries()) {
        const DVec3 at = cell_center(grid_.cell(edge.place));
        const DVec3 moving = velocity(edge.plate, at) - velocity(edge.other, at);
        Fault& here = at_place[edge.place];
        if (const double rate = glm::length(moving); rate > here.rate) {
            here.slip = slip_of(moving, edge.normal);
            here.rate = rate;
            here.idle = 0.0F;
        }
    }
    std::vector<Fault> found;
    std::ranges::copy_if(at_place, std::back_inserter(found),
                         [](const Fault& fault) { return fault.slip != Slip::None; });
    return found;
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
    fault(edges);
    const std::vector<std::vector<Push>> found = pushes(edges, forces, strength);
    std::vector<DVec3> torques(plates_.size(), DVec3{0.0});
    for (std::uint32_t plate = 0; plate < plates_.size(); ++plate) {
        for (const Push& push : found[plate]) {
            torques[plate] += glm::cross(push.at, push.force);
        }
    }
    const std::vector<glm::dmat3> drags = mantle_drags(forces);
    // Summing (I - r r^T) over each plate's crust by its share of the sphere, to tell how fast
    // its crust moves on the whole.
    double sphere = 0.0;
    for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
        sphere += static_cast<double>(grid_.area(cell));
    }
    std::vector<glm::dmat3> spreads(plates_.size(), glm::dmat3{0.0});
    for (std::uint32_t plate = 0; plate < plates_.size(); ++plate) {
        const Plate& carrier = plates_[plate];
        for (const std::uint32_t cell : carrier.cells) {
            const DVec3 at = to_planet(plate, cell_center(grid_.cell(cell)));
            spreads[plate] += (static_cast<double>(grid_.area(cell)) / sphere) *
                              (glm::dmat3{1.0} - glm::outerProduct(at, at));
        }
    }
    const std::vector<DVec3> turns =
        turned_together(drags, couplings(grid_, edges, forces), torques);
    double before = 0.0;
    double after = 0.0;
    for (std::uint32_t plate = 0; plate < plates_.size(); ++plate) {
        const DVec3 now = plates_[plate].rate * plates_[plate].pole;
        before += glm::dot(now, spreads[plate] * now);
        after += glm::dot(turns[plate], spreads[plate] * turns[plate]);
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
    const std::vector<double> warmest = insulated(grid_, tops(), mantle.depth, jobs_);
    for (std::uint32_t place = 0; place < grid_.size(); ++place) {
        heat_[place] += (static_cast<float>(warmest[place]) - heat_[place]) * toward;
    }
    constexpr double gas = 8.314;
    const double now = mantle.potential + std::max(mantle.hotter - (mantle.cooling * time_), 0.0);
    softer_ = std::exp(mantle.activation / gas * ((1.0 / mantle.potential) - (1.0 / now)));
    ridge_crust_ = Crust{}.basalt + static_cast<float>(mantle.melting * (now - mantle.potential));
}

void Lithosphere::rift(double years, const Forces& forces, Strength strength) {
    const std::vector<Boundary> edges = boundaries();
    hang(edges);
    const std::vector<std::vector<Push>> found = pushes(edges, forces, strength);
    const auto count = static_cast<std::uint32_t>(plates_.size());
    std::vector<std::optional<Line>> lines(count);
    each(jobs_, count, 1, [&](std::uint32_t plate, std::uint32_t /*thread*/) {
        const std::optional<Rift>& under = rifts_[plate];
        lines[plate] =
            rifting(plate, found[plate], forces, strength,
                    under ? std::optional{to_planet(plate, under->facing)} : std::nullopt);
    });
    for (std::uint32_t plate = 0; plate < count; ++plate) {
        const std::optional<Line>& line = lines[plate];
        if (!line) {
            rifts_[plate].reset();
            continue;
        }
        std::optional<Rift>& under = rifts_[plate];
        if (!line->going || !under) {
            under = Rift{.facing = to_plate(plate, line->facing), .stretch = 0.0};
        }
        // As fast as its sides would part, but for what the line still holds back.
        under->stretch += pace_ * line->opening * (1.0 - (1.0 / line->pull)) * years;
        if (under->stretch < breakup) {
            continue;
        }
        rifts_[plate].reset();
        const Plate& carrier = plates_[plate];
        Plate kept{.frame = carrier.frame,
                   .pole = carrier.pole,
                   .rate = carrier.rate,
                   .cells = {},
                   .crust = {}};
        Plate parted_off = kept;
        for (std::size_t index = 0; index < carrier.cells.size(); ++index) {
            const DVec3 at = to_planet(plate, cell_center(grid_.cell(carrier.cells[index])));
            Plate& side = glm::dot(at, line->facing) > 0.0 ? kept : parted_off;
            side.cells.push_back(carrier.cells[index]);
            side.crust.push_back(carrier.crust[index]);
        }
        std::vector<Slab> slabs_kept;
        std::vector<Slab> slabs_parted_off;
        for (const Slab& slab : slabs_[plate]) {
            const DVec3 at = to_planet(plate, cell_center(grid_.cell(slab.cell)));
            (glm::dot(at, line->facing) > 0.0 ? slabs_kept : slabs_parted_off).push_back(slab);
        }
        plates_[plate] = std::move(kept);
        plates_.push_back(std::move(parted_off));
        slabs_[plate] = std::move(slabs_kept);
        slabs_.push_back(std::move(slabs_parted_off));
        rifts_.emplace_back();
        bounds_.emplace_back();
        bound(plate);
        bound(static_cast<std::uint32_t>(plates_.size() - 1));
    }
}

std::optional<Lithosphere::Line> Lithosphere::rifting(std::uint32_t plate,
                                                      std::span<const Push> pushes,
                                                      const Forces& forces, Strength strength,
                                                      std::optional<DVec3> under_way) const {
    const std::vector<Stretch> stretches = stretches_of(*this, plate, forces, strength);
    glm::dmat3 drag{0.0};
    std::optional<std::size_t> warmest;
    float most = 0.0F;
    for (std::size_t index = 0; index < stretches.size(); ++index) {
        drag += stretches[index].drag;
        if (plates_[plate].crust[index].type != CrustType::Continental) {
            continue;
        }
        const float heat = heat_[grid_.index(cell_at(stretches[index].at, grid_.level()))];
        if (!warmest || heat > most) {
            warmest = index;
            most = heat;
        }
    }
    if (!warmest || glm::determinant(drag) == 0.0) {
        return std::nullopt;
    }
    Load whole{.drag = drag, .torque = DVec3{0.0}, .turn = DVec3{0.0}};
    for (const Push& push : pushes) {
        whole.torque += glm::cross(push.at, push.force);
    }
    whole.turn = glm::inverse(drag) * whole.torque;
    const auto along = [&](DVec3 facing) {
        DVec3 pushed{0.0};
        for (const Push& push : pushes) {
            if (glm::dot(push.at, facing) > 0.0) {
                pushed += glm::cross(push.at, push.force);
            }
        }
        return pulled(*this, plate, stretches, whole, pushed, facing);
    };
    if (under_way) {
        if (const Pull pull = along(*under_way); pull.ratio > 1.0) {
            return Line{
                .facing = *under_way, .pull = pull.ratio, .opening = pull.opening, .going = true};
        }
    }
    // Lines through the warmest mantle, so many ways round.
    constexpr int ways = 12;
    const DVec3 middle = stretches[*warmest].at;
    const DVec3 east = glm::normalize(
        glm::cross(middle, std::abs(middle.x) < 0.9 ? DVec3{1.0, 0.0, 0.0} : DVec3{0.0, 1.0, 0.0}));
    const DVec3 north = glm::cross(middle, east);
    // Only a line pulled harder than it holds rifts.
    std::optional<Line> hardest;
    for (int way = 0; way < ways; ++way) {
        const double angle = std::numbers::pi * way / ways;
        const DVec3 facing = (std::cos(angle) * east) + (std::sin(angle) * north);
        const Pull pull = along(facing);
        if (pull.ratio > (hardest ? hardest->pull : 1.0)) {
            hardest =
                Line{.facing = facing, .pull = pull.ratio, .opening = pull.opening, .going = false};
        }
    }
    return hardest;
}

void Lithosphere::collapse_margins(const Forces& forces, Strength strength) {
    const auto count = static_cast<std::uint32_t>(plates_.size());
    std::vector<std::vector<Tear>> found(count);
    each(jobs_, count, 1, [&](std::uint32_t plate, std::uint32_t /*thread*/) {
        found[plate] = tears(plate, forces, strength);
    });
    for (std::uint32_t plate = 0; plate < count; ++plate) {
        if (!found[plate].empty()) {
            tear_off(plate, found[plate]);
        }
    }
}

std::vector<Lithosphere::Tear> Lithosphere::tears(std::uint32_t plate, const Forces& forces,
                                                  Strength strength) const {
    const Plate& carrier = plates_[plate];
    const double floated_today = std::sqrt(buoyant_floor / forces.mature);
    std::vector<Tear> found;
    for (std::size_t index = 0; index < carrier.cells.size(); ++index) {
        const Crust& floor = carrier.crust[index];
        if (floor.type != CrustType::Oceanic) {
            continue;
        }
        const auto age = static_cast<double>(floor.age);
        const double floated =
            floated_today * static_cast<double>(floor.basalt) / static_cast<double>(Crust{}.basalt);
        const double weight = std::sqrt(std::min(age, forces.mature) / forces.mature) - floated;
        if (weight <= 0.0) {
            continue;
        }
        if (const std::optional<DVec3> toward =
                weak_margin(*this, plate, index, weight, strength)) {
            found.push_back({.index = index, .toward = *toward});
        }
    }
    return found;
}

void Lithosphere::tear_off(std::uint32_t plate, std::span<const Tear> tears) {
    const Plate& carrier = plates_[plate];
    std::vector<bool> torn(carrier.cells.size(), false);
    std::vector<std::size_t> frontier;
    for (const Tear& tear : tears) {
        torn[tear.index] = true;
        frontier.push_back(tear.index);
    }
    while (!frontier.empty()) {
        const std::size_t index = frontier.back();
        frontier.pop_back();
        for (const std::uint32_t next : grid_.neighbors(carrier.cells[index])) {
            const std::optional<std::size_t> there = index_in(carrier, next);
            if (there && !torn[*there] && carrier.crust[*there].type == CrustType::Oceanic) {
                torn[*there] = true;
                frontier.push_back(*there);
            }
        }
    }
    Plate kept{.frame = carrier.frame,
               .pole = carrier.pole,
               .rate = carrier.rate,
               .cells = {},
               .crust = {}};
    Plate off = kept;
    for (std::size_t index = 0; index < carrier.cells.size(); ++index) {
        Plate& side = torn[index] ? off : kept;
        side.cells.push_back(carrier.cells[index]);
        side.crust.push_back(carrier.crust[index]);
    }
    std::vector<Slab> slabs_kept;
    std::vector<Slab> slabs_off;
    for (const Slab& slab : slabs_[plate]) {
        const std::optional<std::size_t> there = index_in(carrier, slab.cell);
        (there && torn[*there] ? slabs_off : slabs_kept).push_back(slab);
    }
    double sphere = 0.0;
    for (std::uint32_t place = 0; place < grid_.size(); ++place) {
        sphere += static_cast<double>(grid_.area(place));
    }
    // It hangs from all along where it tore as a slab going straight down.
    for (const Tear& tear : tears) {
        const std::uint32_t cell = carrier.cells[tear.index];
        const double length =
            std::sqrt(4.0 * std::numbers::pi * static_cast<double>(grid_.area(cell)) / sphere);
        slabs_off.push_back({.cell = cell,
                             .pull = length * tear.toward,
                             .age = carrier.crust[tear.index].age,
                             .idle = 0.0});
    }
    plates_[plate] = std::move(kept);
    plates_.push_back(std::move(off));
    slabs_[plate] = std::move(slabs_kept);
    slabs_.push_back(std::move(slabs_off));
    rifts_.emplace_back();
    bounds_.emplace_back();
    bound(plate);
    bound(static_cast<std::uint32_t>(plates_.size() - 1));
}

std::vector<glm::dmat3> Lithosphere::mantle_drags(const Forces& forces) const {
    double sphere = 0.0;
    for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
        sphere += static_cast<double>(grid_.area(cell));
    }
    std::vector<glm::dmat3> drags(plates_.size(), glm::dmat3{0.0});
    for (std::uint32_t plate = 0; plate < plates_.size(); ++plate) {
        const Plate& carrier = plates_[plate];
        for (std::size_t index = 0; index < carrier.cells.size(); ++index) {
            const DVec3 at = to_planet(plate, cell_center(grid_.cell(carrier.cells[index])));
            const double share = static_cast<double>(grid_.area(carrier.cells[index])) / sphere;
            drags[plate] +=
                (carrier.crust[index].type == CrustType::Continental ? forces.keel : 1.0) *
                place_drag(at, share);
        }
    }
    return drags;
}

void Lithosphere::weld(double years, const Forces& forces) {
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
    // Terranes, by the drag across ranges alone against the mantle's on the smaller plate.
    Forces ranges = forces;
    ranges.megathrust = 0.0;
    const std::vector<glm::dmat3> drags = mantle_drags(forces);
    const auto trace = [](const glm::dmat3& drag) { return drag[0][0] + drag[1][1] + drag[2][2]; };
    for (const Coupling& pair : couplings(grid_, edges, ranges)) {
        if (trace(pair.drag) > std::min(trace(drags[pair.first]), trace(drags[pair.second]))) {
            joined[(pair.first * count) + pair.second] = true;
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
            rifts_[kept] = rifts_[plate];
        }
        ++kept;
    }
    plates_.resize(kept);
    bounds_.resize(kept);
    slabs_.resize(kept);
    rifts_.resize(kept);
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
    rifts_[from].reset();
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
        // Its lava under what sediment there is, which mostly washed in off the arc itself.
        crust.cover[volcanic_rock] += thicker * arcs.erupted;
        bury(crust);
        crust.thickness += thicker;
        crust.raised = 0.0F;
        if (floats_as_continent(crust)) {
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
