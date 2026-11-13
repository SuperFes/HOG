#pragma once

#include "dissection.hpp"
#include "lake_fill.hpp"
#include "shoreface.hpp"
#include "valleys.hpp"

#include <hog/formation/smooth.hpp>
#include <hog/formation/world.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/noise.hpp>
#include <hog/globe/planet.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace hog::globe {

// A planet's land formed stage by stage, from plates to soil, at the planet's own size, read
// anywhere on it.
class Formed {
public:
    explicit Formed(const Planet& planet);

    struct Here {
        Noise top;                  // metres over the sea, slope per metre at the radius
        std::optional<double> lake; // metres over the sea a lake's water stands at
        // Metres over the sea a river's water stands at the year round, and in the floods that
        // fill its channel bank to bank; none off its channel.
        struct River {
            double level = 0.0;
            double flood = 0.0;
        };
        std::optional<River> river;
        Strata layers;
        double light = 0.0;   // share of light crust
        double folding = 0.0; // how folded the beds are, 0 to 1
        // Metres of the bed gullies and the hollows between wore off it, negative where spurs
        // stand over it.
        double cut = 0.0;
        // Metres the rotted core of a block of rock stands out of the ground about it as a crag.
        double crag = 0.0;
        // How the rock at the top weathers where it stands bare, which crags and the rock that
        // falls off them come of.
        struct Weathering {
            double rate = 0.0;     // metres a year bare rock here breaks up
            double worn = 0.0;     // metres a year the ground here is worn down
            double years = 0.0;    // years it has weathered
            double strength = 0.0; // how well the rock stands steep, 0 to 1
            double bare = 0.0;     // share of the ground that stands as bare rock, 0 to 1
            // Metres a year of rock coming loose from it in blocks, weathered or slid out.
            double loosening = 0.0;
        } weathering;
    };
    // Gullies and river channels narrower than twice `lod` are left out.
    [[nodiscard]] Here at(DVec3 direction, double lod = 0.0) const;

    [[nodiscard]] bool has_sea() const noexcept { return has_sea_; }
    // Where a lake that brims runs out over its rim, where a river leaves it: a unit direction,
    // and the level it brims at, metres over the sea.
    struct Outlet {
        DVec3 at{0.0};
        double level = 0.0;
    };
    [[nodiscard]] std::vector<Outlet> lake_outlets() const;
    [[nodiscard]] double lowest() const noexcept { return lowest_; }
    [[nodiscard]] double highest() const noexcept { return highest_; }
    [[nodiscard]] const formation::World& world() const noexcept { return world_; }

private:
    [[nodiscard]] std::uint32_t start_near(DVec3 direction) const noexcept;
    struct Lake;
    // The ground's top at `direction`, read from the points weighed for it there: as the points
    // have it smoothly between them, but nowhere over the walls of the valleys their water runs
    // down, which rise as steeply as the ground about each stands. Crests stand where walls meet.
    // With it, which way water runs off it, and how far it stands over the floor of the valley
    // it lies in.
    struct Carved {
        formation::Smooth::Read top;
        DVec3 fall{0.0}; // per radian, as the top's gradient
        double above_floor = 1e30;
        DVec3 above_floor_gradient{0.0}; // per metre, the floor's own fall left out
        // Metres of a river's water over it, the year round and in flood, and the levels its
        // water stands at, metres over the sea: along its course, never the ground's.
        double river = 0.0;
        double flood = 0.0;
        double river_level = 0.0;
        double flood_level = 0.0;
    };
    [[nodiscard]] Carved carved(DVec3 direction, const formation::Smooth::Weights& weights,
                                double lod) const;
    // The walls of the valley the river from `point` cuts, as they stand at `direction`, reaching
    // `reach` metres from it, the spurs between its bends rounded `rounding` metres across, over
    // no more than `blend` in height; none off the sea's own points or out of its reach.
    [[nodiscard]] std::optional<Wall> valley_wall(std::uint32_t point, DVec3 direction,
                                                  double reach, double lod, double rounding,
                                                  double blend) const;
    // The course the river from `point` cuts down to the point it runs to: directions from one to
    // the other.
    [[nodiscard]] const std::vector<DVec3>& course_of(std::uint32_t point) const {
        return courses_[point];
    }
    void fill_courses();
    [[nodiscard]] std::vector<DVec3> make_course(std::uint32_t point) const;
    [[nodiscard]] std::vector<double> floors_along(std::uint32_t point,
                                                   const std::vector<DVec3>& line) const;
    // The ground carved, then cut by gullies no narrower than twice `lod`, what was carried onto
    // it settled into its hollows.
    struct Surface {
        Dissected ground;
        double river = 0.0; // as carved
        double flood = 0.0;
        double river_level = 0.0;
        double flood_level = 0.0;
        // Where the ground stood as cut, before what was carried settled on it.
        double bed = 0.0;
        // The share of the points' sand, alluvium, lake beds and till that settled here.
        double settled = 1.0;
        // The bars, levees and hummocks laid ground stands in, about -1 to 1 of their height;
        // gradient per metre along the ground.
        Noise bedforms;
    };
    [[nodiscard]] Surface surface(DVec3 direction, const formation::Smooth::Weights& weights,
                                  double lod) const;
    // Settles what the rivers, lakes, wind and ice carried to the points about `direction` into
    // the hollows of the ground cut there, the crests left bare.
    void settle(DVec3 direction, const formation::Smooth::Weights& weights, double lod,
                Surface& surface) const;
    // The share of what was carried there that drapes `ground` as it lies rather than settling
    // into its hollows; gradient per metre.
    [[nodiscard]] Noise draping(DVec3 direction, const formation::Smooth::Weights& weights,
                                double lod, const Dissected& ground) const;
    // Each point's walls, as steep as the ground falls to its nearest and its cover holds.
    void fill_walls();
    // Fills each lake on the ground as it's read here, lowest first from its floor, until it
    // runs out over its rim or stands at the level formed for it.
    void fill_lakes();
    // The shores' fields, carried a few points either way from the waterline so they read
    // whole along it.
    void fill_shores();
    // What the waves make of the shore about a place, from the shores' fields as `value` reads
    // them there; nothing where they make no beach.
    struct Beach {
        Sea sea;
        Grains grains;
        Profile profile;
        double volume = 0.0; // cubic metres a metre of shore
        bool mud = false;
    };
    template <typename Value>
    [[nodiscard]] std::optional<Beach> beach_from(const Value& value) const;
    // Builds the beach the waves laid up on the ground `top` at `direction`, the beach its top.
    void lay_beach(DVec3 direction, const formation::Smooth::Weights& weights, const Surface& under,
                   Noise& top, Strata& layers) const;
    // The lake with its floor at the point `floor`, its points `reach` radians round it, standing
    // at `most` at the highest.
    [[nodiscard]] Lake fill_lake(std::uint32_t floor, double reach, double most,
                                 formation::Smooth::Weights& weights) const;
    // Lists the lake at the points by its cells, unless `listed` already has it there.
    void list_lake(std::uint32_t lake, std::uint32_t floor, std::vector<std::uint32_t>& listed);

    formation::World world_;
    formation::Smooth smooth_;
    double radius_;
    double voxel_ = 1.0; // metres across the finest voxels
    [[nodiscard]] double sphere_spacing_metres() const noexcept {
        return world_.sphere.spacing() * radius_;
    }
    Gullies gullies_;
    // How far apart in height the ground's ways down may be and still blend into where it runs.
    double fall_blend_ = 0.0;
    bool has_sea_ = false;
    double lowest_ = 0.0;
    double highest_ = 0.0;
    // At each point: the solid top over the sea, unscaled soil, and scaled deposits and rock.
    std::vector<double> top_;
    std::vector<double> organic_;
    std::vector<double> topsoil_;
    std::vector<double> subsoil_;
    std::vector<double> loess_;
    std::vector<double> sand_;
    std::vector<double> alluvium_;
    std::vector<double> lake_beds_;
    std::vector<double> till_;
    std::vector<double> saprolite_;
    std::vector<double> ice_;
    std::vector<double> lava_;
    std::vector<double> rock_cover_;
    std::vector<double> light_;
    // Metres a year bare rock breaks up and the ground is worn down, the years it has had to,
    // and how well it stands.
    std::vector<double> weathering_;
    std::vector<double> worn_;
    std::vector<double> years_;
    std::vector<double> strength_;
    std::uint64_t joints_seed_ = 0;
    // Rivers' courses, and the seeds of the rock they cut through.
    std::vector<std::vector<DVec3>> courses_;
    // The floor of each course at each of its turns.
    std::vector<std::vector<double>> course_floors_;
    std::uint64_t rock_seed_ = 0;
    std::uint64_t faults_seed_ = 0;
    // How steeply valley walls rise about each point, metres up per metre across.
    std::vector<double> wall_;
    // The steepest soil lies on about each point, the wetter the less steep, likewise.
    std::vector<double> soil_holds_;
    // What the sea's waves made of the shore, held at the points about the waterline: storms'
    // wave height, the beach a metre of coast holds and the mud, and what the beach is made of.
    std::vector<double> waves_;
    std::vector<double> usual_;
    std::vector<double> beach_;
    std::vector<double> mud_;
    std::vector<double> quartz_;
    std::vector<double> volcanic_;
    std::vector<double> carbonate_;
    std::vector<double> shingle_;
    // Each point's beach about where the ground comes out of the water, built once along the
    // ground across the shore there; points beside them share theirs.
    void lay_shores();
    [[nodiscard]] std::optional<Laid> lay_shore(std::uint32_t point,
                                                formation::Smooth::Weights& weights) const;
    std::vector<std::uint32_t> laid_at_; // each point's in `laid_`, none without
    std::vector<Laid> laid_;
    // Beaches' grains: sand, volcanic sand, shell sand, shingle, and mud.
    std::array<Grains, 5> grains_{};
    std::uint64_t bars_seed_ = 0;
    // At each point a lake was formed over, 1 and the level formed for it; elsewhere nothing.
    std::vector<double> lake_share_;
    std::vector<double> lake_cap_;
    // A lake's water about its floor, on a grid of cells `step` radians across laid on the plane
    // touching the sphere there.
    struct Lake {
        DVec3 middle{0.0};
        DVec3 east{0.0};
        DVec3 north{0.0};
        double step = 0.0;
        Pool pool;

        // The middle of the cell at `x`, `y`, or anywhere across the grid in cells.
        [[nodiscard]] DVec3 toward(double x, double y) const noexcept;
    };
    std::vector<Lake> lakes_;
    // Each lake's level once filled, for the rivers running into it; none while filling.
    std::vector<std::optional<double>> lake_levels_;
    // The lakes whose water may stand about each point.
    std::vector<std::vector<std::uint32_t>> lakes_near_;
    // The nearest point to each cell of a cube's faces, for searches to start near.
    static constexpr std::size_t starts_across = 64;
    std::vector<std::uint32_t> starts_;
};

} // namespace hog::globe
