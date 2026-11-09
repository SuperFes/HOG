#pragma once

#include "dissection.hpp"
#include "lake_fill.hpp"

#include <hog/formation/smooth.hpp>
#include <hog/formation/world.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/noise.hpp>
#include <hog/globe/planet.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace hog::globe {

// A planet's land formed stage by stage, from plates to soil, read anywhere on it. Its heights
// are as tall against the planet as the generator's own (`Derived::vertical`); what lives at a
// person's scale, the soil, keeps its true depth.
class Formed {
public:
    Formed(const Planet& planet, const Derived& derived);

    struct Here {
        Noise top;                  // metres over the sea, slope per metre at the radius
        std::optional<double> lake; // metres over the sea a lake's water stands at
        Strata layers;
        double light = 0.0;   // share of light crust
        double folding = 0.0; // how folded the beds are, 0 to 1
    };
    // Gullies narrower than twice `lod` are left out.
    [[nodiscard]] Here at(DVec3 direction, double lod = 0.0) const;

    [[nodiscard]] bool has_sea() const noexcept { return has_sea_; }
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
    // The ground carved, then cut by gullies no narrower than twice `lod`.
    [[nodiscard]] Dissected surface(DVec3 direction, const formation::Smooth::Weights& weights,
                                    double lod) const;
    // Fills each lake on the ground as it's read here, lowest first from its floor, until it
    // runs out over its rim or stands at the level formed for it.
    void fill_lakes();
    // The shores' fields, carried a couple of points either way from the waterline so they read
    // whole along it.
    void fill_shores();
    // What of the top of ground `height` metres over the sea the waves laid as beach.
    void lay_beach(double height, const formation::Smooth::Weights& weights, Strata& layers) const;
    // The lake with its floor at the point `floor`, its points `reach` radians round it, standing
    // at `most` at the highest.
    [[nodiscard]] Lake fill_lake(std::uint32_t floor, double reach, double most,
                                 formation::Smooth::Weights& weights) const;
    // Lists the lake at the points by its cells, unless `listed` already has it there.
    void list_lake(std::uint32_t lake, std::uint32_t floor, std::vector<std::uint32_t>& listed);

    formation::World world_;
    formation::Smooth smooth_;
    double radius_;
    double vertical_;
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
    // How steeply valley walls rise about each point, metres up per metre across.
    std::vector<double> wall_;
    // The steepest soil lies on about each point, the wetter the less steep, likewise.
    std::vector<double> soil_holds_;
    // What the sea's waves made of the shore, held at the points about the waterline: storms'
    // wave height, the beach a metre of coast holds and the mud, and what the beach is made of.
    std::vector<double> waves_;
    std::vector<double> beach_;
    std::vector<double> mud_;
    std::vector<double> quartz_;
    std::vector<double> volcanic_;
    std::vector<double> carbonate_;
    std::vector<double> shingle_;
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

        // The middle of the cell at `x`, `y`.
        [[nodiscard]] DVec3 toward(int x, int y) const noexcept;
    };
    std::vector<Lake> lakes_;
    // The lakes whose water may stand about each point.
    std::vector<std::vector<std::uint32_t>> lakes_near_;
    // The nearest point to each cell of a cube's faces, for searches to start near.
    static constexpr std::size_t starts_across = 64;
    std::vector<std::uint32_t> starts_;
};

} // namespace hog::globe
