#include <hog/core/assert.hpp>
#include <hog/globe/detail.hpp>
#include <hog/globe/qef.hpp>
#include <hog/globe/surface_mesh.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hog::globe {

int detail_of(Cell chunk, const Layers& layers) {
    return layers.level - chunk_levels - chunk.level;
}

Cell chunk_under(DVec3 direction, const Layers& layers, int detail) {
    return cell_under(direction, layers.level - chunk_levels - detail);
}

std::vector<Cell> chunks_near(const World& world, DVec3 around, double reach) {
    const Layers& layers = world.generator().layers();
    const double radius = world.generator().planet().radius;
    const Cell first = chunk_under(around, layers);
    // A chunk's middle is never farther than this from its corners.
    const double slack = 2.0 * glm::length(corners(first)[0] - center(first)) * radius;
    const auto apart = [&](Cell chunk) { return glm::length(center(chunk) - around) * radius; };
    std::set<Cell> seen{first};
    std::vector<Cell> open{first};
    std::vector<Cell> near;
    while (!open.empty()) {
        const Cell chunk = open.back();
        open.pop_back();
        if (apart(chunk) <= reach) {
            near.push_back(chunk);
        }
        const CellNeighbors beside = sides(chunk);
        for (const Cell next : beside.all()) {
            if (apart(next) <= reach + slack && seen.insert(next).second) {
                open.push_back(next);
            }
        }
    }
    std::ranges::sort(near, {}, apart);
    return near;
}

namespace {

// Two of a point's three axes: across a chunk's grid, along it, and up the layers.
constexpr std::array<std::array<int, 2>, 3> others{{{1, 2}, {2, 0}, {0, 1}}};

constexpr std::uint32_t no_vertex = std::numeric_limits<std::uint32_t>::max();

// A point of a chunk's grid: steps across and along from the chunk's corner, and a layer.
using Point = std::array<int, 3>;

// From `from`, the last cell toward an edge of a lattice of every `step` cells on its face,
// `steps` more of the lattice on over the edge, where it's the next face's own lattice, as that
// face's chunks have it: its first cell lies on the edge's row, or a step less one in from it.
std::optional<Cell> over_edge(Cell from, int di, int dj, int steps, int step) {
    const std::int64_t n = std::int64_t{1} << from.level;
    const std::uint32_t at = di != 0 ? from.i : from.j;
    const int by = di != 0 ? di : dj;
    const auto to_edge = static_cast<int>(by > 0 ? n - 1 - at : at);
    const auto straight = [&](int cells) { return next_to(from, di * cells, dj * cells); };
    const std::optional<Cell> onto = straight(to_edge + 1);
    const std::optional<Cell> further = straight(to_edge + 2);
    if (!onto || !further) {
        return std::nullopt;
    }
    const std::uint32_t row = onto->i != further->i ? onto->i : onto->j;
    const int first = row == 0 ? 0 : step - 1;
    return straight(to_edge + 1 + first + ((steps - 1) * step));
}

// The cell `a` and `b` steps of `step` cells across and along from `corner`, a cell of that
// lattice; over the cube's edges the next face's lattice's, and none past the cube's corners.
std::optional<Cell> lattice_cell(Cell corner, int a, int b, int step) {
    const std::int64_t n = std::int64_t{1} << corner.level;
    const std::int64_t i = std::int64_t{corner.i} + (std::int64_t{a} * step);
    const std::int64_t j = std::int64_t{corner.j} + (std::int64_t{b} * step);
    const bool in_i = i >= 0 && i < n;
    const bool in_j = j >= 0 && j < n;
    if (!in_i && !in_j) {
        return std::nullopt;
    }
    const auto last = [&](std::int64_t at) { return at < 0 ? 0 : n - step; };
    Cell base = corner;
    base.i = static_cast<std::uint32_t>(in_i ? i : last(i));
    base.j = static_cast<std::uint32_t>(in_j ? j : last(j));
    const auto past = [&](std::int64_t at) {
        return static_cast<int>(at < 0 ? -at / step : (at - (n - step)) / step);
    };
    if (!in_i) {
        return over_edge(base, i < 0 ? -1 : 1, 0, past(i), step);
    }
    if (!in_j) {
        return over_edge(base, 0, j < 0 ? -1 : 1, past(j), step);
    }
    return base;
}

// The layers of bricks held under a chunk's columns.
std::set<int> held_layers(const VoxelStore& store, Cell chunk, int apart) {
    const auto bricks_across =
        static_cast<std::uint32_t>(chunk_side / brick_side) * static_cast<std::uint32_t>(apart);
    const std::uint32_t first_i = chunk.i * bricks_across;
    const std::uint32_t first_j = chunk.j * bricks_across;
    const auto under = [&](const BrickSite& site) {
        return site.face == chunk.face && site.i - first_i < bricks_across &&
               site.j - first_j < bricks_across;
    };
    std::set<int> layers;
    // By what's held, where that's fewer than the chunk's bricks a layer.
    if (store.size() < std::size_t{bricks_across} * bricks_across) {
        for (const BrickSite& site : store.sites()) {
            if (under(site)) {
                layers.insert(static_cast<int>(site.layer));
            }
        }
        return layers;
    }
    for (std::uint32_t layer = 0; layer < store.bricks_up(); ++layer) {
        for (std::uint32_t j = 0; j < bricks_across; ++j) {
            for (std::uint32_t i = 0; i < bricks_across; ++i) {
                if (store.brick(
                        {.face = chunk.face, .i = first_i + i, .j = first_j + j, .layer = layer}) !=
                    nullptr) {
                    layers.insert(static_cast<int>(layer));
                }
            }
        }
    }
    return layers;
}

// Bands of layers the surfaces lie in, lowest first, apart from each other, each from and to
// (inclusive) a layer of a lattice of every `coarsest` layers. `apart` is the chunk's own.
std::vector<std::pair<int, int>> bands_of(const World& world, Cell chunk,
                                          std::span<const Generator::Column> columns, int apart,
                                          int coarsest) {
    const Generator& generator = world.generator();
    const Layers& layers = generator.layers();
    double lowest = std::numeric_limits<double>::max();
    double highest = std::numeric_limits<double>::lowest();
    for (const Generator::Column& column : columns) {
        lowest = std::min(lowest, column.ground());
        highest = std::max(highest, column.ground());
    }
    const double band = distance_band(layers, apart) + (layers.thickness * apart);
    std::vector<std::pair<int, int>> bands{{layers.layer_at(lowest - std::max(surface_depth, band)),
                                            layers.layer_at(highest + band) + 1}};
    if (const auto sea = generator.sea_level(); sea && *sea > lowest) {
        bands.emplace_back(layers.layer_at(*sea - band), layers.layer_at(*sea + band) + 1);
    }
    // Changes held next to a band widen it, a brick at a time.
    const std::set<int> held = held_layers(world.store(), chunk, apart);
    const auto held_in = [&](int brick_layer) { return held.contains(brick_layer); };
    // The last layer of the lattice under the ceiling.
    const int ceiling = ((layers.count - 1) / coarsest) * coarsest;
    for (auto& [bottom, top] : bands) {
        bottom = std::clamp(bottom, 0, layers.count);
        top = std::clamp(top, 0, layers.count);
        while (bottom > 0 && held_in((bottom - 1) / brick_side)) {
            bottom = ((bottom - 1) / brick_side) * brick_side;
        }
        while (top < layers.count && held_in(top / brick_side)) {
            top = std::min(layers.count, ((top / brick_side) + 1) * brick_side);
        }
        bottom = std::min(bottom - (bottom % coarsest), ceiling);
        top = std::min(ceiling, ((top + coarsest - 1) / coarsest) * coarsest);
    }
    std::ranges::sort(bands);
    std::vector<std::pair<int, int>> merged;
    for (const auto& [bottom, top] : bands) {
        if (!merged.empty() && bottom <= merged.back().second) {
            merged.back().second = std::max(merged.back().second, top);
        } else if (top - bottom >= coarsest) {
            merged.emplace_back(bottom, top);
        }
    }
    return merged;
}

// A column of a chunk's grid: the voxels' cell it stands on and the chunk drawn there.
struct Column {
    bool exists = false; // not past the cube's corner
    Cell cell{};
    Cell chunk{};
    int chunk_apart = 1; // layers and cells between the voxels of its chunk's lattice
    // Layers between its voxels: the finest of the boxes it's a corner of; none if it's no corner.
    int apart = 0;
    bool filled = false;
    std::uint64_t index = 0;
    DVec3 direction{0.0};
    std::size_t first = 0; // its lowest voxel's place among the grid's
};

// Where boxes of a chunk's lattice stand: from a column of the lattice to the next across and
// along. A box of the grid is one of these by a step of the chunk's lattice up the layers.
struct Footprint {
    int a = 0;
    int b = 0;
    int across = 0;
    int along = 0;
    int apart = 1;
    // At the cube's corners, where three columns stand round a box rather than four, the one
    // that's missing: 1 if it's across, 2 if along, 3 if both.
    std::optional<int> missing;

    // A corner column, 1 across and 2 along.
    [[nodiscard]] std::pair<int, int> corner(int k) const {
        return {(k & 1) != 0 ? a + across : a, (k & 2) != 0 ? b + along : b};
    }
};

// A box: its footprint and how many of its own heights up from the floor.
struct Leaf {
    std::size_t footprint = 0;
    int up = 0;

    friend bool operator==(const Leaf&, const Leaf&) = default;
};

// The voxels a chunk's surfaces are drawn from: on a grid as fine as the finest chunk beside it,
// from the chunk's corner with a margin all round. Each chunk's lattice stands on it in boxes, so
// where neighbours are drawn at other details, their boxes meet the chunk's. Voxels are filled
// only at the corners of boxes that touch the chunk, each at the finest detail of the boxes it's
// a corner of, so every chunk finds the same voxel there.
class Grid {
public:
    Grid(const World& world, Cell chunk, const Drawn& drawn, DVec3 origin)
        : world_{&world}, layers_{&world.generator().layers()}, chunk_{chunk},
          sea_{world.generator().sea_level()}, apart_{apart_at(detail_of(chunk, *layers_))},
          origin_{origin}, step_{finer_beside(drawn, chunk, apart_) ? apart_ / 2 : apart_},
          size_{chunk_side * apart_ / step_},
          // The widest box of a chunk a detail coarser, across a cube's edge, and one more.
          margin_{(2 * (2 * apart_ / step_)) + 1}, width_{size_ + (2 * margin_)} {
        lay_columns(drawn);
        lay_footprints();
        choose_filled();
    }

    [[nodiscard]] Cell chunk() const noexcept { return chunk_; }
    // Whether boxes of chunks beside each other were found overlapping, which they never should.
    [[nodiscard]] bool overlapped() const noexcept { return overlapped_; }
    [[nodiscard]] int apart() const noexcept { return apart_; }
    [[nodiscard]] int step() const noexcept { return step_; }
    [[nodiscard]] int size() const noexcept { return size_; }
    [[nodiscard]] int coarsest() const noexcept { return 2 * apart_; }
    [[nodiscard]] std::span<const Generator::Column> generated() const noexcept {
        return generated_;
    }

    [[nodiscard]] bool in_grid(int a, int b) const noexcept {
        return a >= -margin_ && a < size_ + margin_ && b >= -margin_ && b < size_ + margin_;
    }
    [[nodiscard]] const Column& column(int a, int b) const { return columns_[spot(a, b)]; }
    [[nodiscard]] const Column& column(Point point) const { return column(point[0], point[1]); }

    // Whether a voxel stands at the point, filled or not.
    [[nodiscard]] bool voxel_there(Point point) const {
        if (!in_grid(point[0], point[1])) {
            return false;
        }
        const Column& at = column(point);
        return at.exists && at.apart > 0 && point[2] >= bottom_ && point[2] <= top_ &&
               point[2] % at.apart == 0;
    }
    // Whether the point's voxel is filled, to be drawn from.
    [[nodiscard]] bool holds(Point point) const {
        return voxel_there(point) && column(point).filled;
    }
    [[nodiscard]] const Voxel& at(Point point) const { return voxels_[index(point)]; }
    // Its distance in metres.
    [[nodiscard]] double metres(Point point) const {
        return globe::metres(at(point).distance) * column(point).apart;
    }
    [[nodiscard]] Material soaked(Point point) const { return soaked_[index(point)]; }
    // Metres a voxel's middle stands over the sea's level, if the world has a sea.
    [[nodiscard]] std::optional<double> over_sea(Point point) const {
        return sea_ ? std::optional{layers_->middle(point[2]) - *sea_} : std::nullopt;
    }
    [[nodiscard]] DVec3 position(Point point) const {
        return (column(point).direction * layers_->middle(point[2])) - origin_;
    }
    // Which voxel of the world it is, the same from any chunk.
    [[nodiscard]] std::pair<std::uint64_t, int> key(Point point) const {
        return {column(point).index, point[2]};
    }

    // The next voxel from a point one way along an axis, if it's filled; none if the next one
    // isn't, or there's none near.
    [[nodiscard]] std::optional<Point> next(Point point, int axis, int by) const {
        if (axis == 2) {
            Point up = point;
            up[2] += by * column(point).apart;
            return holds(up) ? std::optional{up} : std::nullopt;
        }
        for (int steps = 1; steps <= 2 * margin_; ++steps) {
            Point on = point;
            on.at(static_cast<std::size_t>(axis)) += by * steps;
            if (!in_grid(on[0], on[1]) || !column(on).exists) {
                return std::nullopt;
            }
            if (voxel_there(on)) {
                return holds(on) ? std::optional{on} : std::nullopt;
            }
        }
        return std::nullopt;
    }

    // The box a unit of the grid lies in, by its lowest corner.
    [[nodiscard]] std::optional<Leaf> leaf_at(Point corner) const {
        if (!in_grid(corner[0], corner[1]) || corner[2] < 0) {
            return std::nullopt;
        }
        const int print = footprint_of_[spot(corner[0], corner[1])];
        if (print < 0) {
            return std::nullopt;
        }
        const auto index = static_cast<std::size_t>(print);
        return Leaf{.footprint = index, .up = corner[2] / footprints_[index].apart};
    }
    [[nodiscard]] const Footprint& footprint(std::size_t index) const { return footprints_[index]; }

    // Fills the voxels from `bottom` to `top`, both a layer of the coarsest lattice.
    void fill(int bottom, int top) {
        bottom_ = bottom;
        top_ = top;
        voxels_.clear();
        for (auto& [apart, group] : groups_) {
            const std::vector<Voxel> filled = fill_group(group, bottom, top, apart);
            place(group, filled, ((top - bottom) / apart) + 1);
        }
        soak();
    }

    // Fills more voxels under those filled, down to `bottom`.
    void deepen(int bottom) {
        const std::vector<Voxel> before = std::move(voxels_);
        voxels_.clear();
        for (auto& [apart, group] : groups_) {
            const int more = (bottom_ - bottom) / apart;
            const std::vector<Voxel> under = fill_group(group, bottom, bottom_ - apart, apart);
            std::vector<Voxel> joined;
            const int up = ((top_ - bottom_) / apart) + 1;
            joined.reserve(group.size() * static_cast<std::size_t>(more + up));
            for (std::size_t k = 0; k < group.size(); ++k) {
                const Column& at = columns_[group[k]];
                const auto run = [](const std::vector<Voxel>& from, std::size_t start, int count) {
                    return std::span{from}.subspan(start, static_cast<std::size_t>(count));
                };
                const auto lower = run(under, k * static_cast<std::size_t>(more), more);
                const auto upper = run(before, at.first, up);
                joined.insert(joined.end(), lower.begin(), lower.end());
                joined.insert(joined.end(), upper.begin(), upper.end());
            }
            place(group, joined, more + up);
        }
        bottom_ = bottom;
        soak();
    }

    [[nodiscard]] int bottom() const noexcept { return bottom_; }

    // Whether a cave runs open out of the filled voxels' foot, under some of the chunk's own
    // columns' ground.
    [[nodiscard]] bool open_at_foot() const {
        const double radius = layers_->middle(bottom_);
        for (std::size_t k = 0; k < filled_.size(); ++k) {
            const Column& at = columns_[filled_[k]];
            if (at.chunk != chunk_) {
                continue;
            }
            if (voxels_[at.first].solid == Material::Air && radius < generated_[k].ground()) {
                return true;
            }
        }
        return false;
    }

    // Filled columns, as places across and along.
    [[nodiscard]] std::span<const std::pair<int, int>> places() const noexcept { return places_; }

private:
    // Columns of shaped matter the liquid's surface runs on under.
    static constexpr int shore_reach = 2;

    [[nodiscard]] std::size_t spot(int a, int b) const {
        return static_cast<std::size_t>(a + margin_) +
               (static_cast<std::size_t>(b + margin_) * static_cast<std::size_t>(width_));
    }

    [[nodiscard]] std::size_t index(Point point) const {
        const Column& at = column(point);
        return at.first + static_cast<std::size_t>((point[2] - bottom_) / at.apart);
    }

    // Whether a chunk a detail finer is drawn beside this one.
    [[nodiscard]] static bool finer_beside(const Drawn& drawn, Cell chunk, int apart) {
        if (apart == 1) {
            return false;
        }
        const CellNeighbors near = around(chunk);
        for (const Cell beside : near.all()) {
            for (const Cell finer : children(beside)) {
                if (drawn.contains(finer)) {
                    return true;
                }
            }
        }
        return false;
    }

    void lay_columns(const Drawn& drawn) {
        const auto chunk_apart = static_cast<std::uint32_t>(apart_);
        const Cell corner{.level = static_cast<std::uint8_t>(layers_->level),
                          .face = chunk_.face,
                          .i = chunk_.i * chunk_side * chunk_apart,
                          .j = chunk_.j * chunk_side * chunk_apart};
        columns_.resize(static_cast<std::size_t>(width_) * static_cast<std::size_t>(width_));
        for (int b = -margin_; b < size_ + margin_; ++b) {
            for (int a = -margin_; a < size_ + margin_; ++a) {
                const std::optional<Cell> cell = lattice_cell(corner, a, b, step_);
                if (!cell) {
                    continue;
                }
                Column& at = columns_[spot(a, b)];
                at.exists = true;
                at.cell = *cell;
                at.chunk = drawn.over(*cell).value_or(undrawn_as(drawn, *cell));
                at.chunk_apart = apart_at(detail_of(at.chunk, *layers_));
            }
        }
    }

    // What an undrawn cell is drawn as if it were: a chunk at this chunk's detail, unless a coarser
    // chunk faces it across the cube's edge, whose boxes straddle the edge into it at their own
    // size, when it's one at that chunk's detail, so nothing smaller is laid under them. On one
    // face the lattices nest, so the coarser chunk's boxes meet the finer ones' as they are.
    [[nodiscard]] Cell undrawn_as(const Drawn& drawn, Cell cell) const {
        Cell over = cell;
        while (over.level > chunk_.level) {
            over = parent(over);
        }
        const CellNeighbors beside = sides(over);
        for (const Cell facing : beside.all()) {
            if (facing.face == over.face) {
                continue;
            }
            const std::optional<Cell> drawn_facing = drawn.over(facing);
            if (drawn_facing && drawn_facing->level < over.level) {
                while (over.level > drawn_facing->level) {
                    over = parent(over);
                }
            }
        }
        return over;
    }

    [[nodiscard]] static bool on_lattice(const Column& at, int apart) {
        const auto step = static_cast<std::uint32_t>(apart);
        return at.exists && at.cell.i % step == 0 && at.cell.j % step == 0;
    }

    // Steps from a column to the next of a lattice one way along an axis; none off the grid.
    [[nodiscard]] std::optional<int> to_next(int a, int b, int axis, int apart, int by = 1) const {
        for (int steps = 1;; ++steps) {
            const int to_a = axis == 0 ? a + (by * steps) : a;
            const int to_b = axis == 1 ? b + (by * steps) : b;
            if (!in_grid(to_a, to_b) || !column(to_a, to_b).exists) {
                return std::nullopt;
            }
            if (on_lattice(column(to_a, to_b), apart)) {
                return steps;
            }
        }
    }

    void lay_footprints() {
        footprint_of_.assign(columns_.size(), -1);
        for (int b = -margin_; b < size_ + margin_; ++b) {
            for (int a = -margin_; a < size_ + margin_; ++a) {
                const Column& at = column(a, b);
                if (!on_lattice(at, at.chunk_apart)) {
                    continue;
                }
                const std::optional<int> across = to_next(a, b, 0, at.chunk_apart);
                const std::optional<int> along = to_next(a, b, 1, at.chunk_apart);
                if (!across || !along || !in_grid(a + *across, b + *along) ||
                    !column(a + *across, b + *along).exists) {
                    continue;
                }
                paint({.a = a,
                       .b = b,
                       .across = *across,
                       .along = *along,
                       .apart = at.chunk_apart,
                       .missing = std::nullopt});
            }
        }
        lay_corner_footprints();
    }

    // The box at the cube's corner, with three columns round it, by the chunk's high or low
    // sides there; none if the chunk doesn't meet it.
    [[nodiscard]] std::optional<Footprint> corner_footprint(bool high_a, bool high_b) const {
        const std::uint32_t last = (std::uint32_t{1} << chunk_.level) - 1;
        if (chunk_.i != (high_a ? last : 0) || chunk_.j != (high_b ? last : 0)) {
            return std::nullopt;
        }
        // From the face's own corner column, to the next faces' first of the lattice.
        const int across = apart_ / step_;
        const int a = high_a ? size_ - across : 0;
        const int b = high_b ? size_ - across : 0;
        const std::optional<int> over_a = to_next(a, b, 0, apart_, high_a ? 1 : -1);
        const std::optional<int> over_b = to_next(a, b, 1, apart_, high_b ? 1 : -1);
        if (!over_a || !over_b) {
            return std::nullopt;
        }
        const Footprint print{.a = high_a ? a : a - *over_a,
                              .b = high_b ? b : b - *over_b,
                              .across = *over_a,
                              .along = *over_b,
                              .apart = apart_,
                              .missing = (high_a ? 1 : 0) | (high_b ? 2 : 0)};
        for (int k = 0; k < 4; ++k) {
            const auto [x, y] = print.corner(k);
            if (!in_grid(x, y) || (k == print.missing) == column(x, y).exists) {
                return std::nullopt;
            }
        }
        return print;
    }

    void lay_corner_footprints() {
        for (const bool high_a : {false, true}) {
            for (const bool high_b : {false, true}) {
                if (const std::optional<Footprint> print = corner_footprint(high_a, high_b)) {
                    paint(*print);
                }
            }
        }
    }

    // Holds a footprint, laid over its units of the grid.
    void paint(const Footprint& print) {
        const auto index = static_cast<int>(footprints_.size());
        footprints_.push_back(print);
        for (int y = print.b; y < print.b + print.along; ++y) {
            for (int x = print.a; x < print.a + print.across; ++x) {
                overlapped_ = overlapped_ || footprint_of_[spot(x, y)] >= 0;
                footprint_of_[spot(x, y)] = index;
            }
        }
        for (int k = 0; k < 4; ++k) {
            if (k == print.missing) {
                continue;
            }
            const auto [x, y] = print.corner(k);
            Column& corner = columns_[spot(x, y)];
            corner.apart = corner.apart == 0 ? print.apart : std::min(corner.apart, print.apart);
        }
    }

    // The columns at the corners of boxes that touch the chunk, and on their sides, by their
    // voxels' spacing.
    void choose_filled() {
        for (const Footprint& print : footprints_) {
            if (print.a >= size_ || print.b >= size_ || print.a + print.across < 0 ||
                print.b + print.along < 0) {
                continue;
            }
            for (int y = print.b; y <= print.b + print.along; ++y) {
                for (int x = print.a; x <= print.a + print.across; ++x) {
                    Column& at = columns_[spot(x, y)];
                    if (at.exists && at.apart > 0 && !at.filled) {
                        at.filled = true;
                        at.index = index_of(at.cell);
                        at.direction = center(at.cell);
                        groups_[at.apart].push_back(spot(x, y));
                    }
                }
            }
        }
        const Generator& generator = world_->generator();
        for (auto& [apart, group] : groups_) {
            std::vector<Cell> cells;
            cells.reserve(group.size());
            for (const std::size_t at : group) {
                cells.push_back(columns_[at].cell);
            }
            const std::vector<Generator::Column> made =
                generator.columns(cells, voxel_lod(generator) * apart, apart);
            group_columns_[apart] = made;
            for (std::size_t k = 0; k < group.size(); ++k) {
                filled_.push_back(group[k]);
                generated_.push_back(made[k]);
                const auto at = static_cast<int>(group[k]);
                places_.emplace_back((at % width_) - margin_, (at / width_) - margin_);
            }
        }
    }

    [[nodiscard]] std::vector<Voxel> fill_group(const std::vector<std::size_t>& group, int bottom,
                                                int top, int apart) const {
        std::vector<Cell> cells;
        cells.reserve(group.size());
        for (const std::size_t at : group) {
            cells.push_back(columns_[at].cell);
        }
        return world_->fill(cells, group_columns_.at(apart), bottom, ((top - bottom) / apart) + 1,
                            apart);
    }

    // Appends a group's voxels, a run of `up` a column, and notes where each column's begin.
    void place(const std::vector<std::size_t>& group, const std::vector<Voxel>& voxels, int up) {
        for (std::size_t k = 0; k < group.size(); ++k) {
            columns_[group[k]].first = voxels_.size();
            const auto run = std::span{voxels}.subspan(k * static_cast<std::size_t>(up),
                                                       static_cast<std::size_t>(up));
            voxels_.insert(voxels_.end(), run.begin(), run.end());
        }
    }

    // The liquid each voxel counts as full of, if any: its own; the sea's, for the seabed under
    // it, so the sea's surface is drawn over a shelf shallower than a voxel; or beside some,
    // under the ground at the shore, so the liquid's surface runs on under the ground rather
    // than stopping a voxel short of it in steps.
    void soak() {
        soaked_.assign(voxels_.size(), Material::Air);
        for (std::size_t k = 0; k < voxels_.size(); ++k) {
            const Voxel& voxel = voxels_[k];
            if (voxel.solid == Material::Air && voxel.liquid_amount >= Voxel::full / 2) {
                soaked_[k] = voxel.liquid;
            }
        }
        if (sea_) {
            for (std::size_t k = 0; k < places_.size(); ++k) {
                if (generated_[k].ground() >= *sea_) {
                    continue;
                }
                const auto [a, b] = places_[k];
                const Column& at = column(a, b);
                for (int layer = bottom_; layer <= top_; layer += at.apart) {
                    const Point point{a, b, layer};
                    if (voxels_[index(point)].solid != Material::Air && over_sea(point) < 0.0) {
                        soaked_[index(point)] = Material::Water;
                    }
                }
            }
        }
        for (int reach = 0; reach < shore_reach; ++reach) {
            std::vector<Material> next = soaked_;
            for (const auto& [a, b] : places_) {
                const Column& at = column(a, b);
                for (int layer = bottom_; layer <= top_; layer += at.apart) {
                    const Point point{a, b, layer};
                    const std::size_t k = index(point);
                    if (voxels_[k].solid != Material::Air && soaked_[k] == Material::Air) {
                        next[k] = soaked_beside(point);
                    }
                }
            }
            soaked_ = std::move(next);
        }
    }

    [[nodiscard]] Material soaked_beside(Point point) const {
        for (const int axis : {0, 1}) {
            for (const int by : {-1, 1}) {
                if (const std::optional<Point> beside = next(point, axis, by)) {
                    if (const Material wet = soaked(*beside); wet != Material::Air) {
                        return wet;
                    }
                }
            }
        }
        return Material::Air;
    }

    const World* world_;
    const Layers* layers_;
    Cell chunk_;
    std::optional<double> sea_;
    int apart_;
    DVec3 origin_;
    int step_;
    int size_;
    int margin_;
    int width_;
    int bottom_ = 0;
    int top_ = 0;
    std::vector<Column> columns_;
    std::vector<int> footprint_of_;
    bool overlapped_ = false;
    std::vector<Footprint> footprints_;
    std::map<int, std::vector<std::size_t>> groups_;
    std::map<int, std::vector<Generator::Column>> group_columns_;
    std::vector<std::size_t> filled_;
    std::vector<Generator::Column> generated_;
    std::vector<std::pair<int, int>> places_;
    std::vector<Voxel> voxels_;
    std::vector<Material> soaked_;
};

// Where shaped matter meets open space: the distance's zero, increasing outward. What it's made
// of says which side a voxel is on, as a distance under half a unit rounds to none.
struct Ground {
    static bool inside(const Grid& grid, Point point) {
        return grid.at(point).solid != Material::Air;
    }
    static bool crosses(const Grid& grid, Point a, Point b) {
        return inside(grid, a) != inside(grid, b);
    }
    static double value(const Grid& grid, Point point) { return grid.metres(point); }
    // The surface's normal where it crosses from `a` to `b`, `t` of the way: as the voxel nearer
    // it holds it, so at a crease each crossing keeps its own side's; none if neither holds one.
    static DVec3 normal(const Grid& grid, Point a, Point b, double t) {
        const DVec3 near = unpacked_normal(grid.at(t < 0.5 ? a : b).normal);
        return glm::length(near) > 0.0 ? near : unpacked_normal(grid.at(t < 0.5 ? b : a).normal);
    }
    // The solid nearest the surface.
    static Material material(const Grid& grid, std::span<const Point> corners) {
        std::optional<Point> nearest;
        for (const Point corner : corners) {
            if (inside(grid, corner) && (!nearest || grid.metres(corner) > grid.metres(*nearest))) {
                nearest = corner;
            }
        }
        return nearest ? grid.at(*nearest).solid : Material::Air;
    }
};

// Where liquid filling more than half its voxel meets air.
struct Liquid {
    static bool wet(const Grid& grid, Point point) { return grid.soaked(point) != Material::Air; }
    static bool dry(const Grid& grid, Point point) {
        return !wet(grid, point) && grid.at(point).solid == Material::Air;
    }
    static bool crosses(const Grid& grid, Point a, Point b) {
        return (wet(grid, a) && dry(grid, b)) || (dry(grid, a) && wet(grid, b));
    }
    static double value(const Grid& grid, Point point) {
        // The sea's surface stands at its level exactly, wherever the voxels stand.
        if (const std::optional<double> over = grid.over_sea(point)) {
            return *over;
        }
        const Voxel& voxel = grid.at(point);
        const double share = wet(grid, point) || voxel.solid != Material::Air
                                 ? 1.0
                                 : voxel.liquid_amount / double{Voxel::full};
        return 0.5 - std::min(share, 1.0);
    }
    // Not held: the field's own gradient serves.
    static DVec3 normal(const Grid& /*grid*/, Point /*a*/, Point /*b*/, double /*t*/) {
        return DVec3{0.0};
    }
    static Material material(const Grid& grid, std::span<const Point> corners) {
        for (const Point corner : corners) {
            if (wet(grid, corner)) {
                return grid.soaked(corner);
            }
        }
        return Material::Air;
    }
};

// A box's corners and how its own axes run: from its corner first among the world's voxels, so
// chunks whose grids run other ways, across the cube's edges, take them in the same order and
// find the same vertex to the last bit.
struct Frame {
    Point low{};
    Point high{};
    std::array<Point, 8> corners{};
    std::array<std::size_t, 3> axes{}; // the grid's axis each of the box's own runs along
    std::array<bool, 3> flipped{};     // whether it runs down the grid's

    // Where a point on the box lies in its own terms, each axis 0 to 1, from whole steps so
    // it's the same whichever way a grid runs.
    [[nodiscard]] double along(Point point, std::size_t axis) const {
        const std::size_t on = axes.at(axis);
        const int offset =
            flipped.at(axis) ? high.at(on) - point.at(on) : point.at(on) - low.at(on);
        return static_cast<double>(offset) / (high.at(on) - low.at(on));
    }
};

template <typename Surface>
class Net {
public:
    Net(const Grid& grid, SurfaceMesh& mesh) : grid_{&grid}, mesh_{&mesh} {}

    void add() {
        for (const auto& [a, b] : grid_->places()) {
            const Column& at = grid_->column(a, b);
            for (int layer = grid_->bottom(); grid_->holds({a, b, layer}); layer += at.apart) {
                for (int axis = 0; axis < 3; ++axis) {
                    polygon({a, b, layer}, axis);
                }
            }
        }
    }

private:
    // A unit of the grid along an axis: a step across or along, or a step's layers up.
    [[nodiscard]] int unit(std::size_t axis) const { return axis == 2 ? grid_->step() : 1; }

    // The boxes round the edge from `from` along `axis`, anticlockwise about it.
    [[nodiscard]] std::optional<std::array<Leaf, 4>> round(Point from, int axis) const {
        const auto [p, q] = others.at(static_cast<std::size_t>(axis));
        std::array<Leaf, 4> found{};
        for (std::size_t k = 0; k < 4; ++k) {
            Point corner = from;
            corner.at(static_cast<std::size_t>(p)) -=
                (k == 0 || k == 3) ? unit(static_cast<std::size_t>(p)) : 0;
            corner.at(static_cast<std::size_t>(q)) -=
                (k == 0 || k == 1) ? unit(static_cast<std::size_t>(q)) : 0;
            const std::optional<Leaf> leaf = grid_->leaf_at(corner);
            if (!leaf) {
                return std::nullopt;
            }
            found.at(k) = *leaf;
        }
        return found;
    }

    // Boxes in turn round an edge, each once: three or four where it's an edge of some.
    [[nodiscard]] static std::vector<Leaf> distinct(const std::array<Leaf, 4>& round) {
        std::vector<Leaf> found;
        for (std::size_t k = 0; k < 4; ++k) {
            if (round.at(k) != round.at((k + 3) % 4)) {
                found.push_back(round.at(k));
            }
        }
        if (found.empty()) {
            found.push_back(round[0]);
        }
        return found;
    }

    // Whether the edge is a whole edge of the boxes round it, not inside one or across a face.
    [[nodiscard]] bool is_edge(Point from, int axis) const {
        const auto leaves = round(from, axis);
        return leaves && distinct(*leaves).size() >= 3;
    }

    // The chunk that draws the edge from `from` to `to`: the one holding its lower end, or
    // across the cube's edges, the lower face's.
    [[nodiscard]] Cell owner(Point from, Point to, int axis) const {
        if (axis == 2) {
            return grid_->column(from).chunk;
        }
        const Cell a = grid_->column(from).cell;
        const Cell b = grid_->column(to).cell;
        const bool lower = std::tie(a.face, a.i, a.j) < std::tie(b.face, b.i, b.j);
        return grid_->column(lower ? from : to).chunk;
    }

    void polygon(Point from, int axis) {
        const std::optional<Point> to = grid_->next(from, axis, 1);
        if (!to || !Surface::crosses(*grid_, from, *to) ||
            owner(from, *to, axis) != grid_->chunk()) {
            return;
        }
        const auto leaves = round(from, axis);
        if (!leaves) {
            return;
        }
        const std::vector<Leaf> ring = distinct(*leaves);
        if (ring.size() < 3) {
            return;
        }
        std::vector<std::uint32_t> corners;
        corners.reserve(ring.size());
        for (const Leaf leaf : ring) {
            const std::uint32_t made = vertex(leaf);
            if (made == no_vertex) {
                return;
            }
            corners.push_back(made);
        }
        // So far it faces along the axis; the outside is where the value is higher.
        if (Surface::value(*grid_, from) > Surface::value(*grid_, *to)) {
            std::ranges::reverse(corners);
        }
        if (corners.size() == 3) {
            mesh_->indices.insert(mesh_->indices.end(), corners.begin(), corners.end());
            return;
        }
        const auto apart = [&](std::uint32_t a, std::uint32_t b) {
            return glm::length(mesh_->vertices[a].position - mesh_->vertices[b].position);
        };
        // Split along the shorter diagonal.
        if (apart(corners[0], corners[2]) <= apart(corners[1], corners[3])) {
            mesh_->indices.insert(mesh_->indices.end(), {corners[0], corners[1], corners[2],
                                                         corners[0], corners[2], corners[3]});
        } else {
            mesh_->indices.insert(mesh_->indices.end(), {corners[0], corners[1], corners[3],
                                                         corners[1], corners[2], corners[3]});
        }
    }

    [[nodiscard]] std::optional<Frame> frame_of(Leaf leaf) const {
        const Footprint& print = grid_->footprint(leaf.footprint);
        Frame frame;
        frame.low = {print.a, print.b, leaf.up * print.apart};
        frame.high = {print.a + print.across, print.b + print.along, (leaf.up + 1) * print.apart};
        std::array<Point, 8> in_grid{};
        for (std::size_t k = 0; k < 8; ++k) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                in_grid.at(k).at(axis) =
                    ((k >> axis) & 1U) != 0 ? frame.high.at(axis) : frame.low.at(axis);
            }
            if (!grid_->holds(in_grid.at(k))) {
                return std::nullopt;
            }
        }
        const auto key = [&](std::size_t k) { return grid_->key(in_grid.at(k)); };
        std::size_t first = 0;
        for (std::size_t k = 1; k < 8; ++k) {
            first = key(k) < key(first) ? k : first;
        }
        std::array<std::size_t, 3> axes{0, 1, 2};
        std::ranges::sort(axes, {}, [&](std::size_t axis) { return key(first ^ (1U << axis)); });
        frame.axes = axes;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            frame.flipped.at(axis) = ((first >> axes.at(axis)) & 1U) != 0;
        }
        for (std::size_t k = 0; k < 8; ++k) {
            std::size_t from = first;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                from ^= ((k >> axis) & 1U) << axes.at(axis);
            }
            frame.corners.at(k) = in_grid.at(from);
        }
        return frame;
    }

    // Whether a box's neighbours are no finer than it, so its only edges are its own twelve.
    [[nodiscard]] bool plain(const Frame& frame) const {
        const int apart = (frame.high[2] - frame.low[2]);
        for (int y = frame.low[1]; y <= frame.high[1]; ++y) {
            for (int x = frame.low[0]; x <= frame.high[0]; ++x) {
                const bool on_x = x == frame.low[0] || x == frame.high[0];
                const bool on_y = y == frame.low[1] || y == frame.high[1];
                const int there = grid_->column(x, y).apart;
                if ((on_x && on_y && there != apart) || (on_x != on_y && there != 0)) {
                    return false;
                }
            }
        }
        return true;
    }

    // Edges of the grid in one face of a box that are edges of some box.
    void add_face_edges(const Frame& frame, std::size_t across, int face,
                        std::vector<std::pair<Point, int>>& found) const {
        const auto [u, v] = others.at(across);
        const auto su = static_cast<std::size_t>(u);
        const auto sv = static_cast<std::size_t>(v);
        for (int y = frame.low.at(sv); y <= frame.high.at(sv); y += unit(sv)) {
            for (int x = frame.low.at(su); x <= frame.high.at(su); x += unit(su)) {
                Point point{};
                point.at(across) = face;
                point.at(su) = x;
                point.at(sv) = y;
                if (!grid_->holds(point)) {
                    continue;
                }
                for (const int axis : {u, v}) {
                    const auto on = static_cast<std::size_t>(axis);
                    const std::optional<Point> to = grid_->next(point, axis, 1);
                    if (to && to->at(on) <= frame.high.at(on) && is_edge(point, axis)) {
                        found.emplace_back(point, axis);
                    }
                }
            }
        }
    }

    // Edges of the grid on the box's surface that are edges of some box: its own, and where a
    // finer box lies against a face, that box's on it. Each from its lower end.
    [[nodiscard]] std::vector<std::pair<Point, int>> edges_of(const Frame& frame) const {
        std::vector<std::pair<Point, int>> found;
        if (plain(frame)) {
            for (std::size_t k = 0; k < 8; ++k) {
                Point corner{};
                for (std::size_t on = 0; on < 3; ++on) {
                    corner.at(on) = ((k >> on) & 1U) != 0 ? frame.high.at(on) : frame.low.at(on);
                }
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    if (((k >> axis) & 1U) == 0) {
                        found.emplace_back(corner, static_cast<int>(axis));
                    }
                }
            }
            return found;
        }
        for (std::size_t across = 0; across < 3; ++across) {
            add_face_edges(frame, across, frame.low.at(across), found);
            add_face_edges(frame, across, frame.high.at(across), found);
        }
        std::ranges::sort(found);
        const auto [last, end] = std::ranges::unique(found);
        found.erase(last, end);
        return found;
    }

    // Where the surface crosses an edge, in the box's own terms, and its normal there.
    struct Crossing {
        std::pair<std::uint64_t, int> from_key;
        std::pair<std::uint64_t, int> to_key;
        DVec3 point{0.0};
        DVec3 normal{0.0};
        DVec3 held{0.0};
    };

    // The box's vertex, if the surface passes through it and its voxels are filled. It's placed
    // by dual contouring: where the planes tangent to the surface at its crossings of the box's
    // edges meet, so creases and corners keep their edge.
    std::uint32_t vertex(Leaf leaf) {
        const std::uint64_t key =
            (std::uint64_t{leaf.footprint} << 32U) | static_cast<std::uint32_t>(leaf.up);
        if (const auto found = made_.find(key); found != made_.end()) {
            return found->second;
        }
        const std::uint32_t made = make_vertex(leaf);
        made_.emplace(key, made);
        return made;
    }

    // Where the surface crosses the edge from `from` along `axis`, if it does.
    [[nodiscard]] std::optional<Crossing> crossing_at(const Frame& frame,
                                                      const std::array<double, 8>& values,
                                                      const std::array<DVec3, 3>& edges, Point from,
                                                      int axis) const {
        const std::optional<Point> to = grid_->next(from, axis, 1);
        if (!to || !Surface::crosses(*grid_, from, *to)) {
            return std::nullopt;
        }
        // From the end first among the world's voxels, so it's the same from any grid.
        const bool forward = grid_->key(from) < grid_->key(*to);
        const Point low = forward ? from : *to;
        const Point high = forward ? *to : from;
        const double va = Surface::value(*grid_, low);
        const double vb = Surface::value(*grid_, high);
        // Values alike either side, as when both round to none, cross halfway.
        const double t = va != vb ? std::clamp(va / (va - vb), 0.0, 1.0) : 0.5;
        Crossing crossing{.from_key = grid_->key(low), .to_key = grid_->key(high)};
        for (std::size_t on = 0; on < 3; ++on) {
            const double a = frame.along(low, on);
            crossing.point[static_cast<glm::length_t>(on)] = a + (t * (frame.along(high, on) - a));
        }
        const DVec3 normal = Surface::normal(*grid_, low, high, t);
        crossing.held = normal;
        DVec3 slope = slope_at(values, crossing.point);
        if (glm::length(normal) > 0.0) {
            for (std::size_t e = 0; e < 3; ++e) {
                slope[static_cast<glm::length_t>(e)] = glm::dot(normal, edges.at(e));
            }
        }
        crossing.normal = glm::length(slope) > 0.0 ? glm::normalize(slope) : DVec3{0.0};
        return crossing;
    }

    // A box at the cube's corner: its three columns' voxels at its foot and top, by column.
    [[nodiscard]] std::optional<std::vector<Point>> corner_box(const Footprint& print, int low,
                                                               int high) const {
        std::vector<Point> corners;
        for (int k = 0; k < 4; ++k) {
            const auto [a, b] = print.corner(k);
            for (const int layer : {low, high}) {
                if (k == print.missing) {
                    continue;
                }
                if (!grid_->holds({a, b, layer})) {
                    return std::nullopt;
                }
                corners.push_back({a, b, layer});
            }
        }
        return corners;
    }

    // Its edges: up each column, and between each two at the foot and the top.
    [[nodiscard]] std::optional<std::vector<std::pair<Point, Point>>>
    corner_edges(const std::vector<Point>& corners, int high) const {
        std::vector<std::pair<Point, Point>> edges;
        for (std::size_t k = 0; k < corners.size(); k += 2) {
            for (Point at = corners[k]; at[2] < high;) {
                const std::optional<Point> up = grid_->next(at, 2, 1);
                if (!up) {
                    return std::nullopt;
                }
                edges.emplace_back(at, *up);
                at = *up;
            }
        }
        for (std::size_t k = 0; k < corners.size(); ++k) {
            for (std::size_t l = k + 2; l < corners.size(); l += 2) {
                edges.emplace_back(corners[k], corners[l]);
            }
        }
        return edges;
    }

    // The vertex of a box at the cube's corner, with three columns round it: placed in the
    // world's terms, as such a box has no axes of its own to take.
    std::uint32_t make_corner_vertex(Leaf leaf) {
        const Footprint& print = grid_->footprint(leaf.footprint);
        const int low = leaf.up * print.apart;
        const int high = low + print.apart;
        const std::optional<std::vector<Point>> corners = corner_box(print, low, high);
        if (!corners) {
            return no_vertex;
        }
        const std::optional<std::vector<std::pair<Point, Point>>> edges =
            corner_edges(*corners, high);
        if (!edges) {
            return no_vertex;
        }
        std::vector<Crossing> found;
        for (const auto& [from, to] : *edges) {
            if (!Surface::crosses(*grid_, from, to)) {
                continue;
            }
            const bool forward = grid_->key(from) < grid_->key(to);
            const Point a = forward ? from : to;
            const Point b = forward ? to : from;
            const double va = Surface::value(*grid_, a);
            const double vb = Surface::value(*grid_, b);
            const double t = va != vb ? std::clamp(va / (va - vb), 0.0, 1.0) : 0.5;
            const DVec3 normal = Surface::normal(*grid_, a, b, t);
            found.push_back(
                {.from_key = grid_->key(a),
                 .to_key = grid_->key(b),
                 .point = grid_->position(a) + (t * (grid_->position(b) - grid_->position(a))),
                 .normal = normal,
                 .held = normal});
        }
        if (found.empty()) {
            return no_vertex;
        }
        std::ranges::sort(found, {},
                          [](const Crossing& c) { return std::pair{c.from_key, c.to_key}; });
        std::vector<DVec3> points;
        std::vector<DVec3> normals;
        DVec3 held{0.0};
        DVec3 mean{0.0};
        for (const Crossing& crossing : found) {
            points.push_back(crossing.point);
            normals.push_back(crossing.normal);
            held += crossing.held;
            mean += crossing.point;
        }
        mean /= static_cast<double>(found.size());
        // Without normals, at the crossings' mean.
        DVec3 position = glm::length(held) > 0.0 ? nearest_to_planes(points, normals) : mean;
        DVec3 lowest{std::numeric_limits<double>::max()};
        DVec3 highest{std::numeric_limits<double>::lowest()};
        for (const Point corner : *corners) {
            lowest = glm::min(lowest, grid_->position(corner));
            highest = glm::max(highest, grid_->position(corner));
        }
        position = glm::clamp(position, lowest, highest);
        const DVec3 normal = glm::length(held) > 0.0 ? held : position + mesh_->origin;
        const auto index = static_cast<std::uint32_t>(mesh_->vertices.size());
        mesh_->vertices.push_back({.position = Vec3{position},
                                   .normal = Vec3{glm::normalize(normal)},
                                   .material = Surface::material(*grid_, *corners)});
        return index;
    }

    std::uint32_t make_vertex(Leaf leaf) {
        if (grid_->footprint(leaf.footprint).missing) {
            return make_corner_vertex(leaf);
        }
        const std::optional<Frame> frame = frame_of(leaf);
        if (!frame) {
            return no_vertex;
        }
        std::array<double, 8> values{};
        std::array<DVec3, 8> places{};
        for (std::size_t k = 0; k < 8; ++k) {
            values.at(k) = Surface::value(*grid_, frame->corners.at(k));
            places.at(k) = grid_->position(frame->corners.at(k));
        }
        // The box's own edges from its first corner, to take normals into its terms.
        std::array<DVec3, 3> edges{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            edges.at(axis) = places.at(std::size_t{1} << axis) - places[0];
        }
        std::vector<Crossing> found;
        for (const auto& [from, axis] : edges_of(*frame)) {
            if (const auto crossing = crossing_at(*frame, values, edges, from, axis)) {
                found.push_back(*crossing);
            }
        }
        if (found.empty()) {
            return no_vertex;
        }
        std::ranges::sort(found, {},
                          [](const Crossing& c) { return std::pair{c.from_key, c.to_key}; });
        std::vector<DVec3> points;
        std::vector<DVec3> normals;
        DVec3 held{0.0};
        for (const Crossing& crossing : found) {
            points.push_back(crossing.point);
            normals.push_back(crossing.normal);
            held += crossing.held;
        }
        const DVec3 local = glm::clamp(nearest_to_planes(points, normals), DVec3{0.0}, DVec3{1.0});
        DVec3 position{0.0};
        DVec3 normal{0.0};
        for (std::size_t k = 0; k < 8; ++k) {
            const double weight = weight_of(k, local);
            position += weight * places.at(k);
            normal += weight * gradient(frame->corners.at(k));
        }
        // Shaded by the normals held, where they are.
        if (glm::length(held) > 0.0) {
            normal = held;
        }
        if (!(glm::length(normal) > 0.0)) {
            normal = position + mesh_->origin;
        }
        const auto index = static_cast<std::uint32_t>(mesh_->vertices.size());
        mesh_->vertices.push_back({.position = Vec3{position},
                                   .normal = Vec3{glm::normalize(normal)},
                                   .material = Surface::material(*grid_, frame->corners)});
        return index;
    }

    // The field's gradient at a point, by differences with the voxels either side.
    [[nodiscard]] DVec3 gradient(Point point) const {
        std::array<DVec3, 3> along{};
        DVec3 rise{0.0};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto on = static_cast<int>(axis);
            const Point low = grid_->next(point, on, -1).value_or(point);
            const Point high = grid_->next(point, on, 1).value_or(point);
            along.at(axis) = grid_->position(high) - grid_->position(low);
            rise[static_cast<glm::length_t>(axis)] =
                Surface::value(*grid_, high) - Surface::value(*grid_, low);
        }
        const glm::dmat3 edges = glm::transpose(glm::dmat3{along[0], along[1], along[2]});
        return std::abs(glm::determinant(edges)) > 0.0 ? glm::inverse(edges) * rise : DVec3{0.0};
    }

    // A corner's place in its box, each axis 0 or 1.
    [[nodiscard]] static DVec3 within(std::size_t k) {
        return {static_cast<double>(k & 1U), static_cast<double>((k >> 1U) & 1U),
                static_cast<double>((k >> 2U) & 1U)};
    }

    // A corner's share of a point in the box, by trilinear interpolation.
    [[nodiscard]] static double weight_of(std::size_t k, DVec3 local) {
        const DVec3 corner = within(k);
        double weight = 1.0;
        for (glm::length_t axis = 0; axis < 3; ++axis) {
            weight *= corner[axis] > 0.5 ? local[axis] : 1.0 - local[axis];
        }
        return weight;
    }

    // The gradient of the corners' trilinear field at a point in the box, in the box's terms.
    [[nodiscard]] static DVec3 slope_at(const std::array<double, 8>& values, DVec3 local) {
        DVec3 slope{0.0};
        for (std::size_t k = 0; k < 8; ++k) {
            const DVec3 corner = within(k);
            for (glm::length_t axis = 0; axis < 3; ++axis) {
                // The weight's rate along this axis: the others' shares, signed by the side.
                DVec3 across = local;
                across[axis] = corner[axis];
                const double rest = weight_of(k, across);
                slope[axis] += (corner[axis] > 0.5 ? rest : -rest) * values.at(k);
            }
        }
        return slope;
    }

    const Grid* grid_;
    SurfaceMesh* mesh_;
    std::unordered_map<std::uint64_t, std::uint32_t> made_;
};

} // namespace

void add_chunk(SurfaceMesh& mesh, const World& world, Cell chunk) {
    add_chunk(mesh, world, chunk, Drawn{});
}

bool boxes_tile(const World& world, Cell chunk, const Drawn& drawn) {
    const Grid grid{world, chunk, drawn, center(chunk) * world.generator().planet().radius};
    return !grid.overlapped();
}

void add_chunk(SurfaceMesh& mesh, const World& world, Cell chunk, const Drawn& drawn) {
    const Layers& layers = world.generator().layers();
    HOG_ASSERT(chunk.is_valid() && detail_of(chunk, layers) >= 0,
               "a chunk is a cell at least chunk_levels coarser than the voxels'");
    Grid grid{world, chunk, drawn, mesh.origin};
    HOG_ASSERT(!grid.overlapped(),
               std::format("boxes of chunks beside each other overlap, at chunk level {} face {} "
                           "i {} j {}",
                           chunk.level, chunk.face, chunk.i, chunk.j));
    const double lowest =
        std::ranges::min(grid.generated(), {}, &Generator::Column::ground).ground() - cave_depth;
    const double depth = std::max(surface_depth, distance_band(layers, grid.apart()));
    const int coarsest = grid.coarsest();
    for (const auto& [bottom, top] :
         bands_of(world, chunk, grid.generated(), grid.apart(), coarsest)) {
        grid.fill(bottom, top);
        // Caves are followed down while they're open at the foot.
        while (grid.bottom() > 0 && layers.middle(grid.bottom()) > lowest && grid.open_at_foot()) {
            int deeper = std::max(0, layers.layer_at(layers.bottom(grid.bottom()) - depth));
            deeper -= deeper % coarsest;
            grid.deepen(deeper);
        }
        Net<Ground>{grid, mesh}.add();
        Net<Liquid>{grid, mesh}.add();
    }
}

} // namespace hog::globe
