#pragma once

#include "dissection.hpp"
#include "lake_fill.hpp"
#include "shoreface.hpp"

#include <hog/formation/smooth.hpp>
#include <hog/formation/world.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/noise.hpp>
#include <hog/globe/planet.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
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
        Strata layers;
        double light = 0.0;   // share of light crust
        double folding = 0.0; // how folded the beds are, 0 to 1
        // Metres a block of rock stands out of the ground about it as a crag, or under it where
        // the hollows between blocks aren't filled.
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
    // Gullies narrower than twice `lod` are left out.
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
    // With it, which way water runs off it, and how far over the valley floor nearest it stands.
    struct Carved {
        formation::Smooth::Read top;
        DVec3 fall{0.0}; // per radian, as the top's gradient
        double above_floor = 1e30;
    };
    [[nodiscard]] Carved carved(DVec3 direction, const formation::Smooth::Weights& weights) const;
    // The course the river from `point` cuts down to the point it runs to, worked out the first
    // time it's read: directions from one to the other.
    [[nodiscard]] const std::vector<DVec3>& course_of(std::uint32_t point) const;
    // The ground carved, then cut by gullies no narrower than twice `lod`.
    [[nodiscard]] Dissected surface(DVec3 direction, const formation::Smooth::Weights& weights,
                                    double lod) const;
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
    void lay_beach(DVec3 direction, const formation::Smooth::Weights& weights, Noise& top,
                   Strata& layers) const;
    // The lake with its floor at the point `floor`, its points `reach` radians round it, standing
    // at `most` at the highest.
    [[nodiscard]] Lake fill_lake(std::uint32_t floor, double reach, double most,
                                 formation::Smooth::Weights& weights) const;
    // Lists the lake at the points by its cells, unless `listed` already has it there.
    void list_lake(std::uint32_t lake, std::uint32_t floor, std::vector<std::uint32_t>& listed);

    formation::World world_;
    formation::Smooth smooth_;
    double radius_;
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
    // Rivers' courses, each made once, and the seeds of the rock they cut through.
    mutable std::vector<std::vector<DVec3>> courses_;
    std::unique_ptr<std::once_flag[]> course_made_;
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
    // The lakes whose water may stand about each point.
    std::vector<std::vector<std::uint32_t>> lakes_near_;
    // The nearest point to each cell of a cube's faces, for searches to start near.
    static constexpr std::size_t starts_across = 64;
    std::vector<std::uint32_t> starts_;
};

} // namespace hog::globe
