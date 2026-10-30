#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/level_grid.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace hog::jobs {
class JobSystem;
} // namespace hog::jobs

namespace hog::planet {

class Tectonics;
class Volume;

enum class CrustType : std::uint8_t { Oceanic, Continental };

// Kinds of rock other than crystalline that can lie over a column, from the top down: sediment
// laid down loose, the rock it's pressed into once buried, and lava an arc erupts.
inline constexpr std::size_t loose_sediment = 0;
inline constexpr std::size_t sedimentary_rock = 1;
inline constexpr std::size_t volcanic_rock = 2;
inline constexpr std::size_t cover_kinds = 3;
// Kilometres of each kind of rock over a column's crystalline rock.
using Cover = std::array<float, cover_kinds>;
// Kilometres deep loose sediment is buried before it's pressed and cemented into rock, and other
// rock before it's baked into crystalline rock, as it's heated past some 300 degrees.
inline constexpr float lithified = 2.0F;
inline constexpr float hardened = 10.0F;

// A column of crust as its plate carries it.
struct Crust {
    CrustType type = CrustType::Oceanic;
    float thickness = 7.0F; // kilometres
    // Millions of years since it formed, and since it was last piled up into a range.
    float age = 0.0F;
    float raised = 0.0F;
    // What of it on top isn't crystalline rock.
    Cover cover{};

    friend bool operator==(const Crust&, const Crust&) = default;
};

// Of two crusts over one place, whether `one` stays on top of `other`: continents float over ocean
// floor, young warm floor over old cold floor, and the thicker of two continents over the thinner.
[[nodiscard]] bool stays_over(const Crust& one, const Crust& other) noexcept;

// How ocean floor diving under a plate thickens it: out from the trench across the plate over
// it, over so many places of the grid, by so many kilometres every million years, `erupted` of
// it as lava on top and the rest set inside as plutons, some five to one (White, Crisp & Spera
// 2006).
struct Arcs {
    int reach = 3;
    float growth = 0.06F;
    float erupted = 1.0F / 6.0F;
};

// How continental crust piled up into ranges bears its own weight: past `strongest` kilometres
// it flows out at once into the thinner crust around it, a wedge spreading out over ocean floor
// as over continent and burying it, its dense root foundering into the mantle only where the plate
// has no room left at all; short of that, what's piled over a continent's
// settled thickness spreads into it by `flow` of the difference every million years, so a range
// piled up along a line widens into a belt.
struct Ranges {
    float strongest = 65.0F;
    float flow = 0.02F;
};

// A plume rising from deep in the mantle, fixed under the plates as they move over it, and the
// cubic kilometres of magma it brings up every million years.
struct Plume {
    DVec3 at{0.0, 0.0, 1.0}; // unit, in the planet's frame
    double magma = 0.0;

    friend bool operator==(const Plume&, const Plume&) = default;
};

// How plumes rise: `count` of them, each as buoyant as from `weakest` to `strongest` megagrams a
// second, evenly by logarithm, bringing up `magma` cubic kilometres every million years for each
// under a planet of Earth's size (Sleep 1990, Hawaii's at 0.2 cubic kilometres a year). Plates
// turn as fast as Earth's whatever the planet's size, so a plume brings as much for its share of
// the surface as there, and more under a bigger planet by its area. `erupted` of it comes out as
// lava on top and the rest sets inside, some two to one under ocean islands (White, Crisp & Spera
// 2006).
struct Plumes {
    int count = 40;
    double weakest = 0.3;
    double strongest = 8.7;
    double magma = 24'000.0;
    float erupted = 1.0F / 3.0F;
};

// The plumes under a world of `seed` and `radius` metres, rising anywhere alike.
[[nodiscard]] std::vector<Plume> plumes(std::uint64_t seed, double radius,
                                        const Plumes& settings = {});

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
// Where plates close on each other they drag on each other too, each as hard as the other, so
// what one puts on the other moves them against each other and not the whole lithosphere.
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
    // How hard plates closing on each other drag on each length of their edge, as so many
    // radians across of mantle dragging under a plate. At like speeds a megathrust carries some
    // 20 MPa a hundred kilometres down the slab, against some 1 MPa under a plate, so about 2000
    // km of it; where continents collide the range takes up about three times as much, as
    // Tibet's does. Where plates part they hardly drag at all.
    double megathrust = 0.3;
    double suture = 1.0;
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
    void collide(double years, Ranges ranges = {});
    // Builds the crust on top over each of `plumes` up by the magma it brings over `years`, laid
    // along the track its plate carried over it meanwhile: a plate moving on fast leaves a chain
    // of islands behind, and one standing still a pile.
    void feed(std::span<const Plume> plumes, double years, const Plumes& settings = {});
    // Wears `worn` kilometres off the continent on top at each place of the grid, its cover
    // first, down to none, laying it down as sediment on the crust nearest it under the sea,
    // `drowned`, volume for volume: on its own continent's shelf, building it out, or past that
    // on ocean floor, which carries it off to a trench in time.
    void erode(std::span<const float> worn, const std::vector<bool>& drowned);
    // The soft rock on top over each place of the grid.
    [[nodiscard]] std::vector<Cover> cover() const;
    // Turns every plate as the forces on it now move it against the mantle's drag and each
    // other's where they close, all at once, as nothing so slow carries momentum. The first time,
    // the drag is set so the plates keep the pace they had, and it stays so after. A slab pulls
    // from where it hangs whether or not its plates still close there, through welds and rifts,
    // until it breaks off.
    void drive(Forces forces = {}, Strength strength = {});
    // Warms and cools the mantle under each place by the crust on top over `years`, and sets how
    // hot it is on the whole as of now.
    void warm(double years, Mantle mantle = {});
    // Stretches each plate carrying continent over `years` along the line through the warmest
    // mantle under its continent that the forces on it pull apart hardest against how strong it
    // is there, if they pull harder than that: as fast as its two sides would part, but for what
    // the line still holds back. A rift goes on stretching along its line while pulled apart, and
    // fails once it isn't. Stretched `breakup`, it splits the plate: the part across the line
    // becomes a new plate, turning as the other until the plates are next driven.
    void rift(double years, const Forces& forces = {}, Strength strength = {});
    // Plates that turn so alike that over `years` they'd move less than half a place against each
    // other anywhere along their edge are one plate but in name, as a microplate is captured once
    // what drove it dies, and weld into one, the plate with less crust taken into the other's
    // frame. Plates still moving against each other anywhere along their edge stay apart, however
    // their continents have collided: the range between them drags on both as they close. But a
    // plate the range drags on harder than the mantle under it does can't hold its own course
    // against the bigger plate it's run into, and is sutured onto that as a terrane. Plates left
    // with no crust, taken in or wholly dived, are gone, and those after them renumbered.
    void weld(double years, const Forces& forces = {});

    // Ocean floor an arc thickens this far floats as continent.
    static constexpr float arc_continent = 20.0F;
    // Continents as they settle when nothing piles them up.
    static constexpr float settled_continent = 35.0F;
    // Million years a slab hangs on, pulling, once floor stops going down it, before it breaks
    // off and sinks away, as one does some 10 to 20 million years after a collision stops it.
    static constexpr double breakoff = 15.0;
    // Radians a rift opens, thinning the continent, before it breaks apart and floor wells up in
    // it: about 150 km, as the Red Sea's did.
    static constexpr double breakup = 0.025;

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
    // The mantle's drag on each plate turning at w, as -D w: D sums (I - r r^T) over its crust by
    // its share of the sphere, deeper keels dragging harder.
    [[nodiscard]] std::vector<glm::dmat3> mantle_drags(const Forces& forces) const;
    // A rift under way across a plate: the plane of its line, by the side it faces, in the
    // plate's own frame, and radians it has opened.
    struct Rift {
        DVec3 facing{0.0};
        double stretch = 0.0;
    };

    // A line across a plate pulled apart: the plane of it by the side it faces, how many times
    // harder than it holds, how fast its sides would part on their own against unit drag, and
    // whether it's the rift under way.
    struct Line {
        DVec3 facing{0.0};
        double pull = 0.0;
        double opening = 0.0;
        bool going = false;
    };

    // The line across a plate that `pushes` on it pull apart harder than it holds, if any: the
    // rift `under_way` while they do, else the line through the warmest mantle under its
    // continent they pull apart hardest.
    [[nodiscard]] std::optional<Line> rifting(std::uint32_t plate, std::span<const Push> pushes,
                                              const Forces& forces, Strength strength,
                                              std::optional<DVec3> under_way) const;
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
    std::vector<std::optional<Rift>> rifts_;
    double time_ = 0.0;
    std::vector<float> heat_;
    // Of the turn the forces give a plate against unit drag, the share it takes; none until the
    // plates are first driven.
    double pace_ = 0.0;
    double foundered_ = 0.0;
    double softer_ = 1.0;
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
