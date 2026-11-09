#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/layers.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/planet.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace hog::globe {

class Formed;

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
    double temperature = 0.0;        // kelvin
};

// What lies under the ground in one column, top down: sedimentary beds, then the crust's own rock.
struct Strata {
    double cover = 0.0; // sedimentary rock's base, metres below the ground
    double fold = 0.0;  // metres the beds are lifted here
    Material basement = Material::Basalt;
    // What a formed world laid over its beds, metres each, from the top down; within `cover`.
    double ice = 0.0;
    double beach = 0.0; // what the sea's waves laid along the shore, of `beach_of`
    Material beach_of = Material::Sand;
    double organic = 0.0; // dead plant matter: litter, or peat
    double topsoil = 0.0;
    double subsoil = 0.0;
    double loess = 0.0;     // silt the wind laid
    double sand = 0.0;      // sand the wind laid
    double alluvium = 0.0;  // what rivers laid
    double lake_beds = 0.0; // what settled in hollows
    double till = 0.0;      // what ice laid
    double saprolite = 0.0; // the rock under those rotted in place
    double lava = 0.0;
};

// A block of rock fallen from a crag and lying on the ground below it, as the box its joints
// broke it out as, its edges knocked off in the fall, sunk part way in.
struct Rock {
    DVec3 middle{0.0}; // metres from the planet's centre
    DVec3 up{0.0};     // unit, out of the ground it lies on
    DVec3 across{0.0}; // unit, along its long side, square to `up`
    DVec3 axes{0.0};   // half-lengths in metres along `across`, square to it, and `up`
    double edge = 0.0; // metres its edges and corners are rounded off
    Material material = Material::Air;

    // Metres from `point` to the rock's surface, negative inside; `normal` takes the way out.
    [[nodiscard]] double distance(DVec3 point, DVec3& normal) const noexcept;
    // The highest point's metres from the planet's centre.
    [[nodiscard]] double top() const noexcept;
    // Cubic metres of it.
    [[nodiscard]] double volume() const noexcept;
};

// The world as maths: a pure function of the planet's description and a point, the same bits on
// every platform. The planet's centre is the origin and its spin axis +z.
class Generator {
public:
    // Told what it's at as each long stage of making the planet starts, if it's given a way to.
    using Progress = std::function<void(std::string_view)>;
    explicit Generator(const Planet& planet, const Progress& progress = {});

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
        // Without `caves`, as though none were near: for radii outside the stretches caves()
        // gives, where none can be.
        [[nodiscard]] Sample sample(double radius, bool caves = true) const noexcept;
        // Metres from the centre to the ground before caves open it; no solid stands above it.
        [[nodiscard]] double ground() const noexcept { return ground_; }
        // Metres from the centre to where open water stands over this column, sea or lake; 0 none.
        [[nodiscard]] double water() const noexcept { return water_; }
        // Kelvin, the mean the year round at the planet's reference level.
        [[nodiscard]] double surface_temperature() const noexcept { return surface_temperature_; }
        // Rise per metre across the ground.
        [[nodiscard]] double slope() const noexcept { return slope_; }
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
        // A stretch of radius, from its bottom to its top, and whether a cave may be in it.
        struct Boundary {
            double bottom = 0.0;
            double top = 0.0;
            bool cave = false;
            friend constexpr auto operator<=>(const Boundary&, const Boundary&) noexcept = default;
        };
        // The stretches of radius between `lowest` and `highest`, lowest first, where voxels may
        // differ from one layer to the next but for their temperature: about the ground, open
        // water, the beds' edges and caves, each `margin` wider either way.
        // Outside them, voxels are alike all the way to the next, and no cave reaches them.
        // Without `beds`, the beds' edges are left out, as inside() tells them; without `caves`,
        // the caves.
        [[nodiscard]] std::vector<Boundary> boundaries(double lowest, double highest, double margin,
                                                       bool beds = true, bool caves = true) const;
        // The matter a voxel `radius` from the centre shows, deep in the ground clear of its
        // surface, caves and fallen rock: as sample() has it there.
        [[nodiscard]] Material inside(double radius) const noexcept;

    private:
        friend class Generator;
        const Generator* generator_ = nullptr;
        DVec3 direction_{0.0, 0.0, 1.0};
        double lod_ = 0.0;
        double ground_ = 0.0; // metres from the centre
        double water_ = 0.0;  // metres from the centre to where open water stands; 0 none
        double slope_ = 0.0;  // across the ground, at the radius
        double surface_temperature_ = 0.0; // at the reference level
        Strata strata_;
        std::optional<Rock> rock_; // lying on this column, if one does
    };

    [[nodiscard]] Column column(DVec3 direction, double lod) const noexcept;

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
        double lowest = 0.0;  // the lowest ground formed, metres over the radius
        double highest = 0.0; // and the highest
        bool sea = false;
        // The world formed stage by stage the land is read from.
        std::shared_ptr<const Formed> formed{}; // NOLINT(readability-redundant-member-init)
    };
    [[nodiscard]] const Shape& shape() const noexcept { return shape_; }

private:
    [[nodiscard]] Column finish(DVec3 down, double lod, const terms::Ground& made, double slope,
                                const std::optional<Rock>& rock) const noexcept;

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
