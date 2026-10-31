#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/level_grid.hpp>
#include <hog/planet/plates.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace hog::planet {

// The crust as it stands today, made without running its history: the plates as they are now,
// continents assembled from terranes that lie across them, and what each plate boundary does where
// it runs. Thickness, the ocean floor's age and the heights the crust floats to follow from those,
// and the sea fills the lowest of them from the world's water.
class CrustMap {
public:
    // Lengths in metres along the ground and thicknesses in kilometres, so a planet of any size
    // keeps them; rates in metres a million years, as the plates have them.
    struct Settings {
        // Of the planet's area, the share that's continental crust.
        double continental = 0.4;
        // Typical width of the terranes continents are pieced together from, and how many
        // continents they gather into.
        double terrane_size = 20'000.0;
        int continents = 3;
        // Million years since the oldest terranes were joined, and the youngest.
        double oldest = 2'500.0;
        double youngest = 40.0;
        // Kilometres of crust: continent as it settles, new ocean floor, and continent at its very
        // edge, thinned over `margin_reach` metres as it was stretched when its ocean opened.
        double continental_thickness = 35.0;
        double oceanic_thickness = 7.0;
        double margin_thickness = 14.0;
        double margin_reach = 30'000.0;
        // Margins were stretched in segments, each up to this share wider or narrower, so coasts
        // don't run straight along the terranes' edges.
        double margin_variation = 0.6;
        // Each terrane settles up to this many kilometres thinner or thicker than continent does,
        // so some stand as uplands and others lie low enough for the sea to flood.
        double terrane_spread = 4.0;
        // Plates closing or parting this fast do all they can; slower, less, but not
        // proportionally less, as even slow ones build up over time.
        double full_rate = 25'000.0;
        // Continents meeting pile up this much more crust along the boundary, this far either
        // side.
        double collision_thickening = 12.0;
        double collision_reach = 25'000.0;
        // Ocean floor diving under a continent thickens it in a range this far inland and this
        // wide; diving under other ocean floor, it builds an island arc.
        double cordillera_thickening = 10.0;
        double cordillera_inland = 15'000.0;
        double cordillera_reach = 12'000.0;
        double arc_thickening = 10.0;
        double arc_inland = 12'000.0;
        double arc_reach = 8'000.0;
        // Where ocean floor dives, its trench sinks this many metres, this far out and this wide.
        double trench_depth = 3'000.0;
        double trench_out = 4'000.0;
        double trench_reach = 8'000.0;
        // Continent pulled apart thins along the boundary by this much, this far either side.
        double rift_thinning = 14.0;
        double rift_reach = 10'000.0;
        // Where two terranes were joined, the crust was piled up this much along their suture,
        // this far either side; it wears down by half over `suture_halving` million years and
        // by three quarters over three times that.
        double suture_thickening = 25.0;
        double suture_reach = 8'000.0;
        double suture_halving = 120.0;
        // Rivers wear continent down towards the sea, and it rises as it lightens, until it
        // floats at the sea's level: half the way over this many million years since its terrane
        // was joined, three quarters over three times that.
        double wearing_halving = 300.0;
        // Metres over which the plates bend under what boundaries pile up, so ranges are held
        // up partly by the plate around them, which sinks into a moat beside them and bulges
        // a little beyond; Earth's plates bend over some 50 to 100 km, and none at all is each
        // column floating alone.
        double bending_reach = 12'000.0;
        // Crust piling up or pulled apart this many kilometres is hot enough to bend over only
        // `weak_reach` metres, nearly floating alone; less, it stiffens towards the cold plate.
        double weak_at = 5.0;
        double weak_reach = 2'000.0;
        // Ocean floor's age is how far it lies from its ridge over how fast the ridge spreads,
        // as if the plates crossed this planet as slowly as Earth's cross Earth, so oceans are
        // as old and deep as Earth's on a planet of any size.
        double earth_radius = 6'371'000.0;
    };

    enum class Kind : std::uint8_t { Oceanic, Continental };

    struct Column {
        Kind kind = Kind::Oceanic;
        // Kilometres thick, all told.
        double thickness = 7.0;
        // Kilometres of it the plates' boundaries are piling up now, or taking away where
        // negative: what still rises or sinks.
        double active = 0.0;
        // Metres the plate sinks here under what boundaries pile up, here and around.
        double bent = 0.0;
        // Kilometres left of what old sutures piled up.
        double worn = 0.0;
        // Million years since ocean floor formed at its ridge, or since a terrane was joined.
        double age = 0.0;
        // How strongly volcanoes rise above a diving plate, and how strongly a trench sinks, both
        // from 0 to 1.
        double arc = 0.0;
        double trench = 0.0;
        // Unit tangent the nearest range or suture runs along; zero where there's none.
        DVec3 grain{0.0};
        // Which terrane, for continents; which plate's ridge, for ocean floor.
        std::uint32_t terrane = 0;
    };

    // Precondition: 0 < settings.continental < 1 and settings.continents >= 1.
    CrustMap(std::uint64_t seed, std::shared_ptr<const Plates> plates, Settings settings,
             double ocean_fraction);

    [[nodiscard]] const Plates& plates() const noexcept { return *plates_; }
    [[nodiscard]] const LevelGrid& grid() const noexcept { return plates_->grid(); }
    [[nodiscard]] const Settings& settings() const noexcept { return settings_; }
    [[nodiscard]] std::span<const Column> columns() const noexcept { return columns_; }
    // Fixed-point metres each grid cell's crust floats to, above the crest of a mid-ocean ridge,
    // and the same without what boundaries are piling up now.
    [[nodiscard]] std::span<const std::int64_t> heights() const noexcept { return heights_; }
    [[nodiscard]] std::span<const std::int64_t> standing() const noexcept { return standing_; }
    // The level the world's water fills the sea to, topped up to cover `ocean_fraction`.
    [[nodiscard]] std::int64_t sea_level() const noexcept { return sea_level_; }

    // Metres crust of `kind`, `thickness` kilometres thick and `age` million years old floats to
    // above a ridge's crest.
    [[nodiscard]] double floated(Kind kind, double thickness, double age) const;

private:
    void assemble(std::uint64_t seed);
    void thin_margins(std::uint64_t seed);
    void age_ocean();
    void join_terranes();
    void meet_at_boundaries();
    void bend();
    void wear_down();
    void float_crust(double ocean_fraction);

    Settings settings_;
    std::shared_ptr<const Plates> plates_;
    std::vector<Column> columns_;
    std::vector<std::int64_t> heights_;
    std::vector<std::int64_t> standing_;
    std::int64_t sea_level_ = 0;
};

} // namespace hog::planet
