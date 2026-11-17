#pragma once

#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/noise.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The generator's terms, one file each. Heights are metres over the planet's raw radius, as
// functions of direction, their gradients taken against the point on the sphere at that radius.
namespace hog::globe::terms {

// Seeds for each term's noise, fixed forever.
enum class Term : std::uint8_t {
    Strata = 9,
    CaveDistricts = 12,
    CaveA = 13,
    CaveB = 14,
    Chambers = 15,
    Rockfall = 16,
    Contacts = 17,
    Basement = 18,
    Beach = 19,
    Bedding = 20,
};

struct Context {
    const Planet* planet = nullptr;
    const Derived* derived = nullptr;
    const Generator::Shape* shape = nullptr;
    bool crags = true; // standing out of the ground, rather than placed apart

    [[nodiscard]] std::uint64_t seed(Term term) const noexcept;
};

[[nodiscard]] DVec3 tangential(DVec3 gradient, DVec3 direction) noexcept;

[[nodiscard]] double smoothstep(double x) noexcept; // 0 below 0, 1 above 1
[[nodiscard]] double smoothstep_slope(double x) noexcept;

using Layering = Strata;

// A column's ground before its slope across is known: its height and what lies under it.
struct Ground {
    double height = 0.0;              // over the radius
    double surface_temperature = 0.0; // kelvin, at the reference level
    Layering layers;
    std::optional<double> lake;       // a lake's water, over the radius
    std::optional<double> river;      // a river's, at its lowest in the year
    WaterBody body;                   // which that water is
    std::optional<CragStanding> crag; // left out of it, where one stands
    // How far either's water rises over that through the year, and when it's highest, as a share
    // of the year from the March equinox.
    double water_rise = 0.0;
    double water_peak = 0.0;
    DVec3 rise{0.0}; // which way it climbs and how steeply, metres up per metre along
    // The rock at its top: metres a year of it coming loose in blocks, metres a year it breaks up
    // where bare and the ground wears down, the years it has had, how well it stands, 0 to 1, and
    // the share standing bare.
    double loosening = 0.0;
    double weathering = 0.0;
    double worn = 0.0;
    double years = 0.0;
    double strength = 0.0;
    double bare = 0.0;
};

[[nodiscard]] Ground ground(const Context& context, DVec3 direction, double lod) noexcept;
// The same at the middle of each of `cells`, all of one level: the same bits as one at a time.
[[nodiscard]] std::vector<Ground> ground(const Context& context, std::span<const Cell> cells,
                                         double lod);

// Caves never reach a point more than this many metres from open rock.
inline constexpr double cave_reach = 50.0;
// How far under the ground the deepest cave on this planet can open: the stress round a passage
// overcomes any rock's strength below it, more slowly where gravity is weaker.
[[nodiscard]] double cave_floor(const Context& context) noexcept;

// Where caves gather, from 0 where none open to 1.
[[nodiscard]] double cave_district(const Context& context, DVec3 direction) noexcept;

// Metres from a point to the nearest cave's open space: negative inside it, `cave_reach` at most.
// Each cave keeps to the rock it was dissolved or flowed in, a bed or the basement, as the shape
// both share. `ground` is the column's, metres from the centre, before caves open it.
[[nodiscard]] double cave_distance(const Context& context, const Layering& layers, DVec3 direction,
                                   double radius, double ground, double lod) noexcept;
// As cave_distance, but never more than the true distance: the noise's steepest slope stands in
// for its slope at the point, so no cave lies nearer than this in any direction, and a cast
// can step that far.
[[nodiscard]] double cave_bound(const Context& context, const Layering& layers, DVec3 direction,
                                double radius, double ground, double lod) noexcept;

// The blocks of rock lying on the ground, fallen from the crags above or broken where they lie:
// one at most to a cell, kept wholly inside it, in cells of `rock_sizes` levels from about
// `rock_cell` metres across down, each block in the finest its size fits, none in a cell inside
// one holding a block. Blocks too small for `lod` to resolve are left out.
inline constexpr double rock_cell = 32.0;
inline constexpr int rock_sizes = 4;
// The level of the coarsest cells that keep a rock.
[[nodiscard]] int rock_level(const Planet& planet) noexcept;
// The share of the ground about a coarsest cell that blocks of rock cover: what comes loose from
// the rock upslope, falling no further out than the shadow angle reaches, spread over how far it
// runs out and lasting until it breaks down, with what comes loose from the rock there. None
// lies on ground steeper than its rubble rests on.
[[nodiscard]] double fallen_cover(const Context& context, Cell coarsest) noexcept;
// The rock in one cell of any of those levels, wherever its columns stand.
[[nodiscard]] std::optional<Rock> rock_in(const Context& context, Cell cell, double lod);
// Every rock in the coarsest cells `coarsest` and the cells inside them.
[[nodiscard]] std::vector<Rock> rocks_in(const Context& context, std::span<const Cell> coarsest,
                                         double lod);
[[nodiscard]] std::optional<Rock> rockfall(const Context& context, DVec3 direction, double lod);
// What the blocks too small for `lod` to resolve make of the ground they lie on, spread evenly
// over it as every level coarser holds them: metres of rubble of their rock, and the share of the
// ground they cover.
struct Rubble {
    double depth = 0.0;
    double share = 0.0;
    Material of = Material::Gravel;
};
// Whether the column under `direction` shows its rubble at its top: as many of the columns about
// as the share its blocks cover, scattered, so coarse ground looks as fine ground does on the
// whole.
[[nodiscard]] bool rubble_shows(const Context& context, DVec3 direction, double lod,
                                double share) noexcept;
// The block lying across a column, with the rubble of those too small to show.
struct Fall {
    std::optional<Rock> rock;
    Rubble rubble;
};
[[nodiscard]] Fall fallen(const Context& context, DVec3 direction, double lod);
// Over the cells `lod` resolves where they're coarser than the rock cells, whose rock falls
// within them. `known` are cells of any level whose ground at `lod` is `grounds`.
[[nodiscard]] std::vector<Fall> fallen(const Context& context, std::span<const Cell> cells,
                                       double lod, std::span<const Cell> known = {},
                                       std::span<const Ground> grounds = {});
// The same at the middle of each of `cells`, all of one level, each rock made once.
[[nodiscard]] std::vector<std::optional<Rock>> rockfall(const Context& context,
                                                        std::span<const Cell> cells, double lod);

// Metres a contact of `material` with what lies over or under it wanders up and down: most for
// loose ground, least for hard rock.
inline constexpr double widest_contact = 1.5;
// The furthest any contact wanders from where it was laid.
inline constexpr double contact_reach = widest_contact / 2.0;
[[nodiscard]] double contact_width(Material material) noexcept;

// Noise from 0 to 1 at `direction`, spread nearly evenly over that: under a share at that share
// of places.
[[nodiscard]] double scatter(const Context& context, Term term, DVec3 direction) noexcept;
// The same at a point anywhere, metres from the planet's centre, for what varies with depth too.
[[nodiscard]] double scatter_in(const Context& context, Term term, DVec3 point) noexcept;
// Each the column under `direction` takes, those it has no use for under `layers` left out.
[[nodiscard]] Scatter scatter_at(const Context& context, DVec3 direction,
                                 const Layering& layers) noexcept;

// The rock under the beds at `point`, metres from the planet's centre: two basements lie mixed in
// bodies, not columns.
[[nodiscard]] Material basement_at(const Context& context, const Layering& layers,
                                   DVec3 point) noexcept;

// What's `depth` metres under the ground at `radius`, each contact wandering over the wider
// width of the materials either side of it: half of it either way of where it was laid, but up
// no more than half a thinner layer, so a layer thinning out shows in patches, ever fewer, and
// none where it ends. Each contact is one surface, wavy as `scatter` has it.
[[nodiscard]] Material material_at(const Context& context, const Layering& layers,
                                   const Scatter& scatter, DVec3 direction, double depth,
                                   double radius) noexcept;

// Sedimentary beds run a few metres thick on average; a cliff shows dozens.
inline constexpr double bed_thickness = 8.0;

// The beds in one column, counted up from the one at the planet's radius where unfolded, each
// bottom in metres from the centre. Beds come in sets, each bed in a set drawn once for the planet
// as thin or massive, so a bed is as thick wherever it shows; walking through them, each set is
// drawn once. Beds lie only within the layers' cover.
class Beds {
public:
    static constexpr std::int64_t in_set = 8;

    Beds(const Context& context, const Layering& layers) noexcept;

    [[nodiscard]] std::int64_t at(double radius) noexcept;
    [[nodiscard]] double bottom(std::int64_t bed) noexcept;
    [[nodiscard]] double top(std::int64_t bed) noexcept { return bottom(bed + 1); }

private:
    void draw(std::int64_t set) noexcept;

    std::uint64_t seed_ = 0;
    double base_ = 0.0; // where the beds at the planet's radius would lie unfolded
    std::int64_t set_ = 0;
    bool drawn_ = false;
    std::array<double, in_set + 1> bottoms_{}; // up from its set's bottom, the last its top
};

[[nodiscard]] std::int64_t bed_at(const Context& context, const Layering& layers,
                                  double radius) noexcept;
[[nodiscard]] Material bed_rock(const Context& context, std::int64_t bed) noexcept;

} // namespace hog::globe::terms
