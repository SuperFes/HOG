#pragma once

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
    [[nodiscard]] Here at(DVec3 direction) const;

    [[nodiscard]] bool has_sea() const noexcept { return has_sea_; }
    [[nodiscard]] double lowest() const noexcept { return lowest_; }
    [[nodiscard]] double highest() const noexcept { return highest_; }
    [[nodiscard]] const formation::World& world() const noexcept { return world_; }

private:
    [[nodiscard]] std::uint32_t start_near(DVec3 direction) const noexcept;

    formation::World world_;
    formation::Smooth smooth_;
    double radius_;
    double vertical_;
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
    std::vector<double> lake_depth_; // metres of a lake's water over the top; 0 where none
    std::vector<double>
        lake_level_; // the level its water stands at, over the sea, at a lake's points
    std::vector<double> lake_share_; // 1 at a lake's points, else 0
    // The nearest point to each cell of a cube's faces, for searches to start near.
    static constexpr std::size_t starts_across = 64;
    std::vector<std::uint32_t> starts_;
};

} // namespace hog::globe
