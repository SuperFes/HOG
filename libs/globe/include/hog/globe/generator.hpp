#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/layers.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/planet.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hog::globe {

namespace terms {
struct Ground;
} // namespace terms

// What the world holds at a point before anything has changed it.
struct Sample {
    // Metres to the nearest solid surface, negative inside it; near the surface, a true distance
    // to within the slope's curvature.
    double distance = 0.0;
    Material material = Material::Air; // the solid's, or what fills the open space
    // Share of the point's volume that is liquid: all of open water, and a solid's pores where
    // the ground is soaked through, under the sea.
    double water = 0.0;
    Material liquid = Material::Air; // what that liquid is
    // Unit, the way the distance grows fastest: out of the solid, near its surface; none where
    // the distance doesn't change.
    DVec3 normal{0.0};
    double temperature = 0.0; // kelvin
};

// What lies under the ground in one column, top down: snow, loose cover, sedimentary beds, then
// the crust's own rock.
struct Strata {
    double snow = 0.0;            // metres
    Material top = Material::Air; // sand, soil, mud or regolith, under the snow
    double top_depth = 0.0;       // metres
    double sediment_depth = 0.0;  // loose sediment's base, metres below the snow
    double hold = 1.0;            // how much of the loose cover stays on this slope, 0 to 1
    double cover = 0.0;           // sedimentary rock's base, metres below the ground
    double fold = 0.0;            // metres the beds are lifted here
    DVec3 fold_tilt{0.0};         // how much more, per metre across
    DVec3 cover_tilt{0.0};        // how much more `cover`, per metre across
    Material basement = Material::Basalt;
};

// A block of rock fallen from a cliff and lying on the ground below it, as an ellipsoid sunk
// part way in.
struct Rock {
    DVec3 middle{0.0}; // metres from the planet's centre
    DVec3 up{0.0};     // unit, out of the ground
    DVec3 across{0.0}; // unit, along the long axis, level
    DVec3 axes{0.0};   // half-lengths in metres along `across`, level across that, and `up`
    Material material = Material::Air;

    // Metres from `point` to the rock's surface, negative inside; `normal` takes the way out.
    [[nodiscard]] double distance(DVec3 point, DVec3& normal) const noexcept;
    // The highest point's metres from the planet's centre.
    [[nodiscard]] double top() const noexcept;
};

// The world as maths: a pure function of the planet's description and a point, the same bits on
// every platform. The planet's centre is the origin and its spin axis +z.
class Generator {
public:
    explicit Generator(const Planet& planet);

    [[nodiscard]] const Planet& planet() const noexcept { return planet_; }
    [[nodiscard]] const Derived& derived() const noexcept { return derived_; }

    // Where the sea stands, metres from the centre; none on a world without one.
    [[nodiscard]] std::optional<double> sea_level() const noexcept;

    // The voxel shell: from `floor_depth` under the lowest ground up to the sky's ceiling.
    [[nodiscard]] const Layers& layers() const noexcept { return layers_; }

    // At `point`, in metres; `lod` is the size of the smallest detail wanted, in metres, so a
    // coarse sample is a smoothed fine one.
    [[nodiscard]] Sample sample(DVec3 point, double lod) const noexcept;

    // Samples along one direction, sharing all that depends on the direction alone; the same
    // bits as sample() at each point. Chunks are filled a column at a time through this.
    class Column {
    public:
        [[nodiscard]] Sample sample(double radius) const noexcept;
        // Metres from the centre to the ground before caves open it; no solid stands above it.
        [[nodiscard]] double ground() const noexcept { return ground_; }
        // Metres from the centre to where open water stands over this column, sea or lake; 0 none.
        [[nodiscard]] double water() const noexcept { return water_; }
        // Metres of loose cover between the ground and the rock under it.
        [[nodiscard]] double mantle() const noexcept { return mantle_; }
        // Metres from the centre to the highest solid, which a fallen rock lifts above the ground.
        [[nodiscard]] double top() const noexcept {
            return rock_ ? std::max(ground_, rock_->top()) : ground_;
        }
        // Where a cave may lie within `within` metres of the column under its ground, between
        // `lowest` and `highest` metres from the centre: each stretch as the radii of its bottom
        // and top, lowest first, apart from each other; nowhere outside them is any cave that
        // near. Found by casting down the column by the caves' bounded distance, so a column
        // with none near is cheap.
        [[nodiscard]] std::vector<std::pair<double, double>> caves(double lowest, double highest,
                                                                   double within) const;

    private:
        friend class Generator;
        const Generator* generator_ = nullptr;
        DVec3 direction_{0.0, 0.0, 1.0};
        double lod_ = 0.0;
        double ground_ = 0.0; // metres from the centre
        double water_ = 0.0;  // metres from the centre to where open water stands; 0 none
        double mantle_ = 0.0; // metres of loose cover over the rock
        double slope_ = 0.0;  // across the ground, at the radius
        DVec3 tilt_{0.0};     // the way the ground rises, by as much
        double surface_temperature_ = 0.0; // at the reference level
        Strata strata_;
        std::optional<Rock> rock_; // lying on this column, if one does
    };

    [[nodiscard]] Column column(DVec3 direction, double lod) const noexcept;

    // Where rain on the ground under `direction` has gathered, as the worn ground drains: the lake
    // standing there, as metres from the centre to its surface, if one does, and the square metres
    // draining through the place. Nothing on a world without wear.
    struct Gathering {
        std::optional<double> lake;
        double catchment = 0.0;
    };
    [[nodiscard]] Gathering gathered(DVec3 direction) const;
    // Where a lake's water stands over the ground under `direction`, as metres from the centre,
    // if it's flooded there.
    [[nodiscard]] std::optional<double> pond_at(DVec3 direction) const;

    // The fallen rocks, as generated at `lod`, whose cells lie within about a cell of `point`;
    // those reaching further than that from it are among them too.
    [[nodiscard]] std::vector<Rock> rocks_near(DVec3 point, double lod) const;

    // The columns through the middles of a square of cells `side` across from `corner`, on its
    // level, along i then j. Each takes the ground's slope from the cells beside it, so they
    // share the work: cheaper than column() each, and a cell's column comes out the same in any
    // square. Voxels are filled from these.
    [[nodiscard]] std::vector<Column> columns(Cell corner, int side, double lod) const;
    // The same through any cells of one level, in their order, each taking the slope from the
    // cells `apart` beside it, as where every so many cells are taken for coarser detail.
    [[nodiscard]] std::vector<Column> columns(std::span<const Cell> cells, double lod,
                                              int apart = 1) const;

    // Planet-wide constants found once from the knobs; what each means is the generator's own.
    struct Shape {
        double province_threshold = 0.0;
        double province_band = 1.0; // of the province field, across the margin
        DVec3 one_side{0.0, 0.0, 1.0};
        double datum = 0.0;   // raw height of the radius: the sea's level, or the mean ground's
        double lowest = 0.0;  // the lowest ground surveyed, metres over the radius
        double highest = 0.0; // and the highest
        bool sea = false;
        // Metres the running water has cut from the ground at each cell of `cut_level`, from the
        // erosion of the generated ground; empty on a world without wear.
        int cut_level = 0;
        std::vector<float> cut{}; // NOLINT(readability-redundant-member-init)
        // At those cells too: where a hollow in the worn ground holds a lake, the height its
        // water stands at over the radius, else `no_lake`; and the area draining through it.
        static constexpr float no_lake = -1e30F;
        std::vector<float> lake{};      // NOLINT(readability-redundant-member-init)
        std::vector<float> catchment{}; // NOLINT(readability-redundant-member-init)
        // And the drainage's network: the cell the water leaves each by (itself at the sea), and
        // the height the water table stands at under it, the worn ground with its hollows filled.
        std::vector<std::uint32_t> receiver{}; // NOLINT(readability-redundant-member-init)
        std::vector<float> table{};            // NOLINT(readability-redundant-member-init)
        // The lakes' water over the fine ground: for each cell of `cut_level` a lake floods
        // into, a layer for each level its water stands at there, and which of the cell's 64 by 64
        // columns hold it. The columns are wider than the voxels', so `rim` also marks the ring of
        // columns round the flooded ones, where the voxels' own ground may lie under the level
        // too.
        struct Pond {
            struct Layer {
                float level = 0.0F;
                std::array<std::uint64_t, 64> rows{};
                std::array<std::uint64_t, 64> rim{};
            };
            std::vector<Layer> layers{}; // NOLINT(readability-redundant-member-init)
        };
        std::unordered_map<std::uint64_t, Pond>
            ponds{}; // NOLINT(readability-redundant-member-init)
    };
    [[nodiscard]] const Shape& shape() const noexcept { return shape_; }

private:
    // `slope` is `tilt`'s length, as found along with it.
    [[nodiscard]] Column finish(DVec3 down, double lod, const terms::Ground& made, double slope,
                                DVec3 tilt) const noexcept;

    Planet planet_;
    Derived derived_;
    Shape shape_;
    Layers layers_;
};

// The finest detail generated into voxels: two of them across, as finer would alias into steps a
// voxel apart.
[[nodiscard]] double voxel_lod(const Generator& generator);

// How many times coarser than the voxels' `lod` is, at least once.
[[nodiscard]] double coarser(const Generator& generator, double lod);

// The highest solid surface under `direction` between the sky's ceiling and the floor, metres from
// the centre, found by walking down the distance field; none if the column is open to the floor.
[[nodiscard]] std::optional<double> ground_below(const Generator& generator, DVec3 direction,
                                                 double lod);

// The direction nearest `direction` whose ground stands over the sea, sought on rings a
// kilometre apart out to `within` metres; `direction` itself on a world without a sea; none if
// there's no land that near.
[[nodiscard]] std::optional<DVec3> land_near(const Generator& generator, DVec3 direction,
                                             double within = 100'000.0);

} // namespace hog::globe
