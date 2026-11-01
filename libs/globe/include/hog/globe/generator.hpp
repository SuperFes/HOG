#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/layers.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/planet.hpp>

#include <optional>

namespace hog::globe {

// What the world holds at a point before anything has changed it.
struct Sample {
    // Metres to the nearest solid surface, negative inside it; near the surface, a true distance
    // to within the slope's curvature.
    double distance = 0.0;
    Material material = Material::Air; // the solid's, or what fills the open space
    double water = 0.0;                // share of open space that is standing liquid
    double temperature = 0.0;          // kelvin
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

    private:
        friend class Generator;
        const Generator* generator_ = nullptr;
        double ground_ = 0.0;              // metres from the centre
        double slope_ = 0.0;               // across the ground, at the radius
        double surface_temperature_ = 0.0; // at the reference level
        Material rock_ = Material::Air;
    };

    [[nodiscard]] Column column(DVec3 direction, double lod) const noexcept;

    // Planet-wide constants found once from the knobs; what each means is the generator's own.
    struct Shape {
        double province_threshold = 0.0;
        double province_band = 1.0; // of the province field, across the margin
        DVec3 one_side{0.0, 0.0, 1.0};
        double datum = 0.0;  // raw height of the radius: the sea's level, or the mean ground's
        double lowest = 0.0; // the lowest ground surveyed, metres over the radius
        bool sea = false;
    };

private:
    Planet planet_;
    Derived derived_;
    Shape shape_;
    Layers layers_;
};

// The highest solid surface under `direction` between the sky's ceiling and the floor, metres from
// the centre, found by walking down the distance field; none if the column is open to the floor.
[[nodiscard]] std::optional<double> ground_below(const Generator& generator, DVec3 direction,
                                                 double lod);

} // namespace hog::globe
