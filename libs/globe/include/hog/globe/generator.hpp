#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/layers.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/planet.hpp>

#include <optional>
#include <span>
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
        // Metres of loose cover between the ground and the rock under it.
        [[nodiscard]] double mantle() const noexcept { return mantle_; }
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
        double ground_ = 0.0;              // metres from the centre
        double mantle_ = 0.0;              // metres of loose cover over the rock
        double slope_ = 0.0;               // across the ground, at the radius
        DVec3 tilt_{0.0};                  // the way the ground rises, by as much
        double surface_temperature_ = 0.0; // at the reference level
        Strata strata_;
    };

    [[nodiscard]] Column column(DVec3 direction, double lod) const noexcept;

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
        double datum = 0.0;  // raw height of the radius: the sea's level, or the mean ground's
        double lowest = 0.0; // the lowest ground surveyed, metres over the radius
        bool sea = false;
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
