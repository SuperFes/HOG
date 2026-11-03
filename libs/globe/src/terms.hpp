#pragma once

#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/noise.hpp>

#include <cstdint>
#include <optional>

// The generator's terms, one file each. Heights are metres over the planet's raw radius, as
// functions of direction, their gradients taken against the point on the sphere at that radius.
namespace hog::globe::terms {

// Seeds for each term's noise, fixed forever.
enum class Term : std::uint8_t {
    Province = 1,
    OneSide,
    Belts,
    Peaks,
    Hills,
    Ridges,
    Gullies,
    Craters,
    Strata,
    Basins,
    Folds,
    CaveDistricts,
    CaveA,
    CaveB,
    Chambers,
    Rockfall,
};

struct Context {
    const Planet* planet;
    const Derived* derived;
    const Generator::Shape* shape;

    [[nodiscard]] std::uint64_t seed(Term term) const noexcept;
};

[[nodiscard]] DVec3 tangential(DVec3 gradient, DVec3 direction) noexcept;

[[nodiscard]] double smoothstep(double x) noexcept; // 0 below 0, 1 above 1
[[nodiscard]] double smoothstep_slope(double x) noexcept;

// max(a, b), rounded where they meet over `width`, and how much of its rate is a's.
[[nodiscard]] double smooth_max(double a, double b, double width) noexcept;
[[nodiscard]] double smooth_max_share(double a, double b, double width) noexcept;

// The crustal province field, before its threshold: the high crust stands where it's above.
[[nodiscard]] Noise province_field(const Context& context, DVec3 direction) noexcept;

// Metres across the margin between the provinces, as wide as the isostatic step's slope lets it.
[[nodiscard]] double margin(const Context& context) noexcept;

// How much of the high crust is here, from 0 to 1.
struct Province {
    double high = 0.0;
    DVec3 gradient{0.0};
};

[[nodiscard]] Province province(const Context& context, DVec3 direction) noexcept;

// How much mountain building has gone on here, from 0 to 1: the belts, by the regime.
[[nodiscard]] Noise built_up(const Context& context, DVec3 direction,
                             const Province& crust) noexcept;

// Mountains, hills and ocean ridges, by the regime and the erosion at work.
[[nodiscard]] Noise relief(const Context& context, DVec3 direction, const Province& crust,
                           const Noise& built, double lod) noexcept;

// One width of gullies, cut straight down `slope` (the ground's, coarser than them), as deep as
// `share` of the fall across their width, faded in by `weight`.
[[nodiscard]] Noise gullies(const Context& context, DVec3 direction, DVec3 slope, double wavelength,
                            int octave, double share, double weight) noexcept;

// A crater's height over the ground it struck, `x` radii from its middle, and its slope per
// radius: a bowl below `transition` across, a flat floor and a central peak above it. Its rim's
// crest is rounded over `rounding` radii either side, up to half a radius, as creep wears it.
struct CraterProfile {
    double height = 0.0;
    double slope = 0.0;
};

[[nodiscard]] CraterProfile crater_profile(double x, double diameter, double transition,
                                           double rounding) noexcept;

// Craters over `diameter` metres across per km² that the surface keeps, before wear.
[[nodiscard]] double craters_over(const Planet& planet, double diameter) noexcept;

// Every crater's shape over the ground, worn by its age.
[[nodiscard]] Noise craters(const Context& context, DVec3 direction, double lod) noexcept;

// The cones and craters of the planet's volcanoes, outlines warped.
[[nodiscard]] Noise volcanoes(const Context& context, DVec3 direction, double lod) noexcept;

// The ground's raw height along a direction, the sum of every column term.
struct Column {
    Noise height;
    double high = 0.0;  // share of high crust
    double built = 0.0; // share of mountain building
};

[[nodiscard]] Column column(const Context& context, DVec3 direction, double lod) noexcept;

using Layering = Strata;

// From the ground's height over the radius, its slope and its year-mean temperature: the
// layers as deep as they'd lie on ground that holds them all, with `hold` for how much of them
// this slope keeps.
[[nodiscard]] Layering layering(const Context& context, DVec3 direction, const Column& column,
                                double height, double slope, double temperature) noexcept;

// Metres of loose cover the layers would lie, snow, top and sediment together.
[[nodiscard]] double loose_depth(const Layering& layers) noexcept;

// The layers as they lie where the cover is `thickness` deep: each thinned alike where there is
// less than they'd have, the sediment deepened where there is more.
void lie_in(Layering& layers, double thickness) noexcept;

// Loose cover creeps downhill until its surface is the rock's smoothed over the creep length and
// lifted by the depth it's made at: it fills the hollows under that surface and leaves the knobs
// above it bare. `rock` and `smooth` are heights, `depth` metres the cover would lie on even
// ground, `hold` how much of it the slope keeps. Metres of cover over the rock.
[[nodiscard]] double mantle(double rock, double smooth, double depth, double hold) noexcept;

// A column's ground before its slope across is known: the rock, the loose cover crept smooth on
// it, and the layers as they lie.
struct Ground {
    Column rock;
    double height = 0.0;              // the ground's, over the raw radius: the rock's and its cover
    double mantle = 0.0;              // metres of loose cover over the rock
    double surface_temperature = 0.0; // kelvin, at the reference level
    Layering layers;
};

[[nodiscard]] Ground ground(const Context& context, DVec3 direction, double lod) noexcept;

// Caves never reach a point more than this many metres from open rock.
inline constexpr double cave_reach = 50.0;
// How far under the ground the deepest cave on this planet can open: the stress round a passage
// overcomes any rock's strength below it, more slowly where gravity is weaker.
[[nodiscard]] double cave_floor(const Context& context) noexcept;

// Where caves gather, from 0 where none open to 1.
[[nodiscard]] double cave_district(const Context& context, DVec3 direction) noexcept;

// Metres from a point to the nearest cave's open space: negative inside it, `cave_reach` at most.
// Each cave keeps to the rock it was dissolved or flowed in, a bed or the basement, as the shape
// both share. `ground` is the column's, metres from the centre, before caves open it; `cover` is
// how much rock lies between the point and open air.
[[nodiscard]] double cave_distance(const Context& context, const Layering& layers, DVec3 direction,
                                   double radius, double ground, double cover, double lod) noexcept;
// As cave_distance, but never more than the true distance: the noise's steepest slope stands in
// for its slope at the point, so no cave lies nearer than this in any direction, and a cast
// can step that far.
[[nodiscard]] double cave_bound(const Context& context, const Layering& layers, DVec3 direction,
                                double radius, double ground, double cover, double lod) noexcept;

// The block of rock lying on the ground a column stands on, fallen from the cliffs above: one
// at most to a cell about `rock_cell` metres across, kept wholly inside it, where the ground
// round about is steep enough to shed rock. Blocks too small for `lod` to resolve are left out.
inline constexpr double rock_cell = 32.0;
// Rise per run over a stretch about `rock_cell` wide below which the ground sheds no rock.
inline constexpr double sheds_from = 0.3;
// The level of the cells that each keep a rock.
[[nodiscard]] int rock_level(const Planet& planet) noexcept;
// How steep the ground climbs about `direction` over the stretch that sheds rock, a rise per run.
[[nodiscard]] double steepness(const Context& context, DVec3 direction) noexcept;
// The rock in one cell, wherever its columns stand.
[[nodiscard]] std::optional<Rock> rock_in(const Context& context, Cell cell, double lod) noexcept;
[[nodiscard]] std::optional<Rock> rockfall(const Context& context, DVec3 direction,
                                           double lod) noexcept;

// What's `depth` metres under the ground at `radius`.
[[nodiscard]] Material material_at(const Context& context, const Layering& layers, double depth,
                                   double radius) noexcept;

// Sedimentary beds run a few metres thick; a cliff shows dozens.
inline constexpr double bed_thickness = 8.0;

// The bed at `radius`, counted up from the one at the planet's radius where unfolded, its bottom
// in metres from the centre, and its rock; beds lie only within the layers' cover.
[[nodiscard]] std::int64_t bed_at(const Context& context, const Layering& layers,
                                  double radius) noexcept;
[[nodiscard]] double bed_bottom(const Context& context, const Layering& layers,
                                std::int64_t bed) noexcept;
[[nodiscard]] Material bed_rock(const Context& context, std::int64_t bed) noexcept;

} // namespace hog::globe::terms
