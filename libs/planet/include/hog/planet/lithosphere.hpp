#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/level_grid.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace hog::jobs {
class JobSystem;
} // namespace hog::jobs

namespace hog::planet {

class Tectonics;
class Volume;

enum class CrustType : std::uint8_t { Oceanic, Continental };

// A column of crust as its plate carries it.
struct Crust {
    CrustType type = CrustType::Oceanic;
    float thickness = 7.0F; // kilometres
    // Millions of years since it formed, and since it was last piled up into a range.
    float age = 0.0F;
    float raised = 0.0F;

    friend bool operator==(const Crust&, const Crust&) = default;
};

// Of two crusts over one place, whether `one` stays on top of `other`: continents float over ocean
// floor, young warm floor over old cold floor, and the thicker of two continents over the thinner.
[[nodiscard]] bool stays_over(const Crust& one, const Crust& other) noexcept;

// How ocean floor diving under a plate thickens it: out from the trench across the plate over
// it, over so many places of the grid, by so many kilometres every million years.
struct Arcs {
    int reach = 3;
    float growth = 0.06F;
};

// How continental crust piled up into ranges bears its own weight: past `strongest` kilometres
// it flows out at once into the thinner continent around it, or where there's none, as along an
// arc, its dense root founders into the mantle; short of that, what's piled over a continent's
// settled thickness spreads into it by `flow` of the difference every million years, so a range
// piled up along a line widens into a belt.
struct Ranges {
    float strongest = 65.0F;
    float flow = 0.02F;
};

// How the mantle warms where continents lie over it, as their thick crust holds its heat in, and
// cools again under ocean floor, which lets it out: over `years`, each place goes the share
// 1 - e^(-years / settling) of the way to fully warm or cool. The whole mantle starts `hotter`
// kelvin over today's `potential` and loses `cooling` a million years till it's as today's, as
// Earth's has lost about 250 K since plates began 3 billion years ago. The lithosphere over a
// hotter mantle is thinner and its creep faster, by e^(activation / R (1/today - 1/then)) with
// the activation energy of creep in olivine, so it's weaker by as much.
struct Mantle {
    double settling = 100.0;
    double hotter = 200.0;
    double cooling = 0.08;
    double potential = 1623.0;
    double activation = 300e3;
};

// A stretch of one plate's edge, a place of the grid long, where it meets another plate.
struct Boundary {
    enum class Kind : std::uint8_t {
        Ridge,     // they part, and new floor wells up between them
        Trench,    // they close, and this plate's floor dives under the other
        Arc,       // they close, and the other's floor dives under this plate
        Collision, // they close, continent against continent
    };

    Kind kind = Kind::Ridge;
    std::uint32_t place = 0; // of the grid, on this plate's edge
    std::uint32_t plate = 0;
    std::uint32_t other = 0;
    DVec3 normal{0.0};   // unit, along the surface from this plate toward the other
    double length = 0.0; // radians of the planet's circumference
    // Radians per million years the plates close at across it; under nought where they part.
    double closing = 0.0;
    // The crust on top on this side and on the other.
    Crust own;
    Crust facing;
};

// What moves the plates. Old, cold floor diving at a plate's trenches pulls it hardest, ridges
// push it a little, and the mantle drags on it, more under the deep keels of continents. Where
// continents collide, the range between them pushes both back as a ridge does, by the square of
// its thickness over a settled continent's less one, as a raised column's weight grows: twice as
// thick, three times a ridge's push, about what Tibet's is. Pulls and pushes act across an edge
// as far as the plates close or part across it, not slide past. Where the mantle is warm it
// lifts the plates over it into a broad dome they slide off, as off a ridge but over all of it.
struct Forces {
    // Of an old slab's pull on each length of trench, the push on each length of ridge.
    double ridge = 0.3;
    // Million years over which floor cools and thickens, so its slab pulls harder; no more after.
    double mature = 100.0;
    // How much harder the mantle drags on continent than on ocean floor.
    double keel = 3.0;
    // Of an old slab's pull on each length of trench, the push off a fully warm mantle's dome on
    // each length of its rim: it lifts the crust about a kilometre, a third of what a ridge
    // stands over old floor, so a third of a ridge's push.
    double dome = 0.1;
};

// How strong the lithosphere is against being pulled apart, against an old slab's pull on the
// same length: a continent over cool mantle, and over fully warm mantle, which thins the strong
// lid under it, and between, the warmth's share of the way by ratio, as strength falls off
// steeply with heat. Old ocean floor holds as a cool continent does, and younger floor less, as
// thin as it is, by as much as its slab would pull less. A slab is old floor too, so it pulls no
// harder than that holds before it breaks off, as slabs did over the hotter, softer young mantle.
struct Strength {
    double cool = 2.0;
    double warm = 0.1;
    double ocean = 3.0;
};

// The crust as rigid plates on the sphere, each keeping its crust on a grid in its own frame and
// turning about its own pole. Crust is never carried from cell to cell, so a plate stays whole
// however far it turns: where two plates' crust lies over one place they've met, and where none
// does they've parted.
class Lithosphere {
public:
    struct Plate {
        DQuat frame{1.0, 0.0, 0.0, 0.0}; // turns the plate's own frame into the planet's
        DVec3 pole{0.0, 0.0, 1.0};       // unit, in the planet's frame
        double rate = 0.0;               // radians per million years
        // The cells of the grid in its own frame it has crust in, ascending, and that crust.
        std::vector<std::uint32_t> cells;
        std::vector<Crust> crust;
    };

    struct Found {
        std::uint32_t plate = 0;
        Crust crust;

        friend bool operator==(const Found&, const Found&) = default;
    };

    // Precondition: each plate's cells are of `grid`, ascending, with one crust to each.
    Lithosphere(LevelGrid grid, std::vector<Plate> plates);

    // Shares its work out over the threads of `jobs` from now on, or none. It comes out the same
    // either way. `jobs` must outlive it or be taken back first.
    void share_work(jobs::JobSystem* jobs) noexcept { jobs_ = jobs; }

    // Turns every plate on by `years` million years, and ages its crust as much.
    void advance(double years);
    // Fills every place the plates have parted with new ocean floor, joined to the plate whose
    // crust is nearest across the gap, so the ridge runs down the gap's middle.
    void spread();
    // Ocean floor lying under another plate's crust dives into the mantle and is gone, and the
    // plate over it thickens into an arc behind the trench.
    void subduct(double years, Arcs arcs = {});
    // Continental crust lying under another plate's is too light to dive: it's scraped off onto
    // the crust over it, piling that up into a range, which then settles over `years` as
    // `ranges` says.
    // Plates whose collision piles up more than their continent can take, so it founders, have
    // jammed, and weld when next welded.
    void collide(double years, Ranges ranges = {});
    // Turns every plate as the forces on it now move it against the mantle's drag, at once, as
    // nothing so slow carries momentum. The first time, the drag is set so the plates keep the
    // pace they had, and it stays so after. A slab pulls from where it hangs whether or not its
    // plates still close there, through welds and rifts, until it breaks off.
    void drive(Forces forces = {}, Strength strength = {});
    // Warms and cools the mantle under each place by the crust on top over `years`, and sets how
    // hot it is on the whole as of now.
    void warm(double years, Mantle mantle = {});
    // Splits each plate carrying continent along the line through the warmest mantle under its
    // continent that the forces on it pull apart hardest against how strong it is there, if they
    // pull harder than that. The part across the line becomes a new plate, turning as the other
    // until the plates are next driven.
    void rift(const Forces& forces = {}, Strength strength = {});
    // Plates that turn so alike that over `years` they'd move less than half a place against each
    // other anywhere along their edge are one plate but in name, as a microplate is captured once
    // what drove it dies, and weld into one. So do plates whose continents have collided, piling
    // up a range where they meet, that move so alike along it. The plate with less crust is taken
    // into the other's frame, as is a plate whose collision with another has jammed, however they
    // still move, as the range between them can take up no more of their closing. Continents that
    // only touch, as where one has just split, stay apart while their plates still move. Plates
    // left with no crust, taken in or wholly dived, are gone, and those after them renumbered.
    void weld(double years);

    // Ocean floor an arc thickens this far floats as continent.
    static constexpr float arc_continent = 20.0F;
    // Continents as they settle when nothing piles them up.
    static constexpr float settled_continent = 35.0F;
    // Million years a slab hangs on, pulling, once floor stops going down it, before it breaks
    // off and sinks away, as one does some 10 to 20 million years after a collision stops it.
    static constexpr double breakoff = 15.0;

    [[nodiscard]] const LevelGrid& grid() const noexcept { return grid_; }
    [[nodiscard]] std::span<const Plate> plates() const noexcept { return plates_; }
    // Millions of years advanced.
    [[nodiscard]] double time() const noexcept { return time_; }
    // How warm the mantle is under each place of the grid, from nought, as under ocean floor, to
    // one, as under a continent that has lain there for good.
    [[nodiscard]] std::span<const float> heat() const noexcept { return heat_; }
    // How many times weaker the lithosphere is than over today's mantle, as of the last warming.
    [[nodiscard]] double softer() const noexcept { return softer_; }
    // Cubic kilometres of continental crust foundered into the mantle so far.
    [[nodiscard]] double foundered() const noexcept { return foundered_; }

    // A unit direction in a plate's own frame as it lies now in the planet's, and back.
    [[nodiscard]] DVec3 to_planet(std::uint32_t plate, DVec3 own) const;
    [[nodiscard]] DVec3 to_plate(std::uint32_t plate, DVec3 at) const;

    // The crust a plate carries over a unit direction, if any.
    [[nodiscard]] std::optional<Crust> crust_of(std::uint32_t plate, DVec3 at) const;
    // The crust left on top over a unit direction, or none where the plates have parted. Ties go
    // to the lower plate, so the same plates always give the same crust.
    [[nodiscard]] std::optional<Found> top(DVec3 at) const;

    // How fast a plate carries a unit direction along, in radians per million years.
    [[nodiscard]] DVec3 velocity(std::uint32_t plate, DVec3 at) const;
    // Every stretch of every plate's edge, by the plates on top on either side.
    [[nodiscard]] std::vector<Boundary> boundaries() const;

private:
    // Where a plate's crust lies in its own frame: nowhere further from `middle` than the angle
    // whose cosine is `reach`, so a place further out needs no lookup.
    struct Bound {
        DVec3 middle{0.0, 0.0, 1.0};
        double reach = 2.0;
    };

    // A force along the surface at a unit direction, in an old slab's pull on a radian of trench.
    struct Push {
        DVec3 at{0.0};
        DVec3 force{0.0};
    };

    // A slab hanging from a stretch of a plate's edge, in the plate's own frame: the cell of its
    // grid it hangs at, its pull as an old slab's would be, the age of the floor that last went
    // down it, and million years since then.
    struct Slab {
        std::uint32_t cell = 0;
        DVec3 pull{0.0};
        float age = 0.0F;
        double idle = 0.0;
    };

    // What's left on top over each place of the grid.
    [[nodiscard]] std::vector<std::optional<Found>> tops() const;
    // Every force on each plate: from its slabs, across the rest of its `edges`, and off the
    // mantle's dome under it.
    [[nodiscard]] std::vector<std::vector<Push>>
    pushes(std::span<const Boundary> edges, const Forces& forces, Strength strength) const;
    // Floor going down a trench among `edges` now feeds the slab hanging there, in place of
    // whatever hung there before.
    void hang(std::span<const Boundary> edges);
    // The plane, by the side it faces, of the line through the warmest mantle under a plate's
    // continent that `pushes` on it pull apart hardest against how strong it is, if harder.
    [[nodiscard]] std::optional<DVec3> weakest(std::uint32_t plate, std::span<const Push> pushes,
                                               const Forces& forces, Strength strength) const;
    // Where in a plate's cells is the crust it carries over a unit direction, if it has any.
    [[nodiscard]] std::optional<std::size_t> held(std::uint32_t plate, DVec3 at) const;
    // Of each two plates where they meet, whether the first's crust stays over the second's of
    // the same type, one flag to each pair in a row per plate.
    [[nodiscard]] std::vector<bool> overriding() const;
    // Another plate whose crust lies over a plate's crust of `type` at a unit direction, if any.
    [[nodiscard]] std::optional<std::uint32_t> overlying(std::uint32_t plate, CrustType type,
                                                         DVec3 at,
                                                         const std::vector<bool>& overrides) const;
    // Bounds a plate's crust as it lies now. Crust it has lost since may still lie inside.
    void bound(std::uint32_t plate);
    // Drops the plates with no crust, keeping the rest in order.
    void drop_bare();
    // Takes all the crust of plate `from` into `plate`'s frame, leaving `from` with none.
    void take_in(std::uint32_t plate, std::uint32_t from);
    // Thickens the plate on top out from each of the `trenches`, places of the grid.
    void raise_arcs(std::span<const std::uint32_t> trenches, double years, Arcs arcs);

    LevelGrid grid_;
    std::vector<Plate> plates_;
    std::vector<Bound> bounds_;
    std::vector<std::vector<Slab>> slabs_;
    double time_ = 0.0;
    std::vector<float> heat_;
    // Of the turn the forces give a plate against unit drag, the share it takes; none until the
    // plates are first driven.
    double pace_ = 0.0;
    double foundered_ = 0.0;
    double softer_ = 1.0;
    // Each two plates, lower first, whose collision has jammed since they were last welded.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> jammed_;
    jobs::JobSystem* jobs_ = nullptr;
};

// How the mantle drags on a place at unit direction `at`, `share` of the sphere, against its
// plate's turn about the planet's middle, in drag on the whole sphere moving at unit speed: as
// it's carried along, and as it spins about its own middle, as a disc does, so even a plate of a
// place or two can't spin unhindered.
[[nodiscard]] glm::dmat3 place_drag(DVec3 at, double share) noexcept;

// The plates and crust of `tectonics` as they stand, each plate's frame the planet's, with one of
// its steps taken as `step_years` million years.
[[nodiscard]] Lithosphere lithosphere_from(const Tectonics& tectonics, double step_years);

// Fills every column of `level` from the volume's bottom to the planet's radius with the crust on
// top there, basalt for ocean floor and granite for continents, and the mantle under it, in shells
// of `shell_level`. Where the plates have parted the mantle reaches the top. All crust tops out at
// the radius, since how high it floats depends on more than the crust alone.
void fill_lithosphere(Volume& volume, const Lithosphere& lithosphere, double radius, int level,
                      int shell_level);

} // namespace hog::planet
