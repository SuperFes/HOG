#pragma once

#include <hog/planet/bake.hpp>
#include <hog/planet/beds.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/continents.hpp>
#include <hog/planet/hotspots.hpp>
#include <hog/planet/landform.hpp>
#include <hog/planet/planet_config.hpp>
#include <hog/planet/rivers.hpp>
#include <hog/planet/volcanoes.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>

namespace hog::planet {

// What the ground shows of the rock under it.
struct Exposure {
    Rock rock = Rock::Basalt;
    // Fixed point from 0 to 1: how much of the ground is bare rock rather than soil.
    std::int64_t bare = 0;
    // Fixed point: the bed's index plus how far up it the ground cuts; zero in rock that lies in
    // no beds.
    std::int64_t layer = 0;

    friend bool operator==(const Exposure&, const Exposure&) = default;
};

// Where the beds lie under a cell: ground at height `ground` cuts their layering at
// `ground - risen`, the land having risen that many fixed-point metres since they were laid.
struct Layering {
    BedColumn column;
    std::int64_t risen = 0;
};

// Terrain height: continents give the coastline, lowlands and ocean
// floor; ridges raise mountain ranges where the plates push them up, away from the coast, and
// volcanoes stand along the arcs above plates diving and in chains carried off plumes. Rivers
// then wear the land over geological time while the ranges keep rising, which carves valleys and
// drainage, and cut channels where they run.
class Elevation {
public:
    // Metres.
    static constexpr std::int64_t ocean_depth = 4'000;
    // From the troughs to the crests of the hills on the ocean floor, and the deepest the floor
    // reaches, in a trench.
    static constexpr std::int64_t abyssal_hill_height = 300;
    static constexpr std::int64_t deepest =
        ocean_depth + Continents::trench_depth + (abyssal_hill_height / 2);
    static constexpr std::int64_t lowland_height = 300;
    static constexpr std::int64_t mountain_height = 4'000;

    explicit Elevation(const PlanetConfig& config);

    [[nodiscard]] const Plates& plates() const noexcept { return continents_.plates(); }
    [[nodiscard]] const Continents& continents() const noexcept { return continents_; }
    [[nodiscard]] const Beds& beds() const noexcept { return beds_; }
    [[nodiscard]] const Volcanoes& volcanoes() const noexcept { return volcanoes_; }
    [[nodiscard]] const Hotspots& hotspots() const noexcept { return hotspots_; }

    // Fixed-point continent height; land is positive.
    template <int Bits = fixed_fraction_bits>
    [[nodiscard]] std::int64_t height(CellId cell) const {
        return continents_.height<Bits>(cell);
    }
    // Out of the sea: ground above its level, told by the fine elevation, or below it but cut off
    // from it.
    [[nodiscard]] bool is_land(CellId cell) const;

    // Fixed-point metres above sea level of the ground, a river's bed included: positive above
    // the sea, negative below it.
    [[nodiscard]] std::int64_t elevation(CellId cell) const;

    // What rock ground at `ground` fixed-point metres shows there, or would under its soil, and
    // how bare it is: bare where soil is thin, or stripped from steep slopes as the ledges are,
    // and at least `least_bare`; never under water.
    [[nodiscard]] Exposure exposure(CellId cell, std::int64_t ground,
                                    std::int64_t least_bare = 0) const;

    [[nodiscard]] Layering layering(CellId cell) const;

    // The unit direction of the landform cell nearest `from` whose ground stands more than `above`
    // fixed-point metres above the sea, or `from` itself if none does.
    [[nodiscard]] DVec3 nearest_land(DVec3 from, std::int64_t above = 0) const;

    // The river running through a cell, if one does.
    [[nodiscard]] std::optional<Channel> river(CellId cell) const;

    // The lake over a cell, if one is: the ground under it and the level of its water.
    [[nodiscard]] std::optional<Channel> lake(CellId cell) const;

    // Fixed-point metres of the ground above the water it drains into first, a river, a lake or
    // the sea; zero under water.
    [[nodiscard]] std::int64_t above_water(CellId cell) const;

    // The elevation of the sea floor at a height `below` sea level, all in fixed point with `Bits`
    // fractional bits: a sandy shoreface on Dean's equilibrium profile, a shelf, then the slope
    // down to the abyss this deep.
    template <int Bits = fixed_fraction_bits>
    [[nodiscard]] static std::int64_t
    sea_floor(std::int64_t below, std::int64_t abyss = ocean_depth * fixed_unit<Bits>);

    // How strongly mountains rise here, in fixed point from 0 to 1.
    [[nodiscard]] std::int64_t mountain_strength(CellId cell) const;

    // The land as rivers left it and the rivers on it, evolved when first needed. Elevations
    // built from the same config share them.
    [[nodiscard]] const Landform& landform() const { return evolved().landform; }
    [[nodiscard]] const Rivers& rivers() const { return evolved().rivers; }

private:
    struct Evolved {
        // `ground` is the worn ground at each landform cell's centre, which rivers start from.
        Evolved(Landform worn, std::span<const std::int64_t> ground, const PlanetConfig& config);
        Evolved(const Evolved&) = delete;
        Evolved& operator=(const Evolved&) = delete;
        Evolved(Evolved&&) = delete;
        Evolved& operator=(Evolved&&) = delete;
        ~Evolved() = default;

        Landform landform;
        Meander meander;
        Rivers rivers;
    };

    // From the continent's height and the plates' mountains there, all in fixed point with
    // `Bits` fractional bits: mountains rise only well inland.
    template <int Bits>
    [[nodiscard]] static std::int64_t mountain_strength(std::int64_t height,
                                                        std::int64_t mountains);
    // Ridged noise from 0 to 1 in fixed point with `Bits` fractional bits, at `frequency` per unit
    // of the sphere's radius, its ridges running with the grain.
    template <int Bits>
    [[nodiscard]] static std::int64_t ridge(std::uint64_t seed, double frequency, int octaves,
                                            CellId cell, const std::array<std::int64_t, 3>& grain);
    // The same, while the landform is still being evolved; `shifted` is the cell the meanders
    // move `cell` to.
    [[nodiscard]] Layering layering(CellId cell, CellId shifted, const Landform& landform) const;
    // The elevation as generated, before rivers wore it.
    [[nodiscard]] std::int64_t generated(CellId cell) const;
    // The same with `Bits` fractional bits.
    template <int Bits>
    [[nodiscard]] std::int64_t generated(CellId cell) const;
    // The elevation as rivers wore it, before they cut their channels, given the generated one;
    // about lakes, the landform's own ground, which holds them in.
    [[nodiscard]] std::int64_t worn(CellId cell, std::int64_t generated, const Landform& landform,
                                    const Meander& meander) const;
    // Worn `ground` stepped into ledges along the beds, where bare rock stands steep; `shifted` is
    // where the landform is sampled for it.
    [[nodiscard]] std::int64_t ledged(CellId cell, CellId shifted, std::int64_t ground,
                                      std::int64_t generated, const Landform& landform) const;
    [[nodiscard]] const Evolved& evolved() const;
    [[nodiscard]] std::shared_ptr<const Evolved> evolve(const PlanetConfig& config) const;
    // The landform on a grid, evolved from the generated ground, calling `stepped` with how many
    // steps are done after each.
    [[nodiscard]] Landform wear(LevelGrid grid, std::uint64_t seed,
                                const std::function<void(int)>& stepped) const;
    // What a bake for a grid of `level` holds, or nothing where it holds none.
    [[nodiscard]] static std::shared_ptr<const Evolved>
    unbake(std::optional<LandformBake> bake, int level, const PlanetConfig& config);

    Continents continents_;
    Volcanoes volcanoes_;
    Hotspots hotspots_;
    std::uint64_t ridge_seed_;
    double ridge_frequency_; // per unit of the sphere's radius
    std::uint64_t hill_seed_;
    double hill_frequency_;
    std::uint64_t relief_seed_;
    Beds beds_;
    struct Evolving {
        PlanetConfig config;
        std::once_flag once;
        std::shared_ptr<const Evolved> evolved;
    };
    std::shared_ptr<Evolving> evolving_;
};

extern template std::int64_t Elevation::sea_floor<fixed_fraction_bits>(std::int64_t below,
                                                                       std::int64_t abyss);
extern template std::int64_t Elevation::sea_floor<fine_fraction_bits>(std::int64_t below,
                                                                      std::int64_t abyss);

} // namespace hog::planet
