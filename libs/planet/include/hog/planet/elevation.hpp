#pragma once

#include <hog/planet/archipelago.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/continents.hpp>
#include <hog/planet/landform.hpp>
#include <hog/planet/planet_config.hpp>
#include <hog/planet/rivers.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace hog::planet {

// Terrain height: continents and the starting islands give the coastline, lowlands and ocean
// floor; ridges raise mountain ranges inside belts away from continental coasts. Rivers then wear
// the land over geological time while the belts keep rising, which carves valleys and drainage,
// and cut channels where they run.
class Elevation {
public:
    // Metres.
    static constexpr std::int64_t ocean_depth = 4'000;
    static constexpr std::int64_t lowland_height = 300;
    static constexpr std::int64_t mountain_height = 4'000;

    explicit Elevation(const PlanetConfig& config);

    [[nodiscard]] const Continents& continents() const noexcept { return continents_; }
    [[nodiscard]] const Archipelago& archipelago() const noexcept { return archipelago_; }

    // Fixed-point continent height with the islands raised on it; land is positive.
    [[nodiscard]] std::int64_t height(CellId cell) const {
        return archipelago_.raise(cell, continents_.height(cell));
    }
    [[nodiscard]] bool is_land(CellId cell) const { return height(cell) > 0; }

    // Fixed-point metres above sea level of the ground, a river's bed included: positive on
    // land, negative under the ocean.
    [[nodiscard]] std::int64_t elevation(CellId cell) const;

    // The river running through a cell, if one does.
    [[nodiscard]] std::optional<Channel> river(CellId cell) const;

    // How strongly mountains rise here, in fixed point from 0 to 1.
    [[nodiscard]] std::int64_t mountain_strength(CellId cell) const;

    // The land as rivers left it and the rivers on it, evolved when first needed. Elevations
    // built from the same config share them.
    [[nodiscard]] const Landform& landform() const { return evolved().landform; }
    [[nodiscard]] const Rivers& rivers() const { return evolved().rivers; }

private:
    struct Evolved {
        Evolved(Landform worn, const Elevation& terrain, const PlanetConfig& config);
        Evolved(const Evolved&) = delete;
        Evolved& operator=(const Evolved&) = delete;
        Evolved(Evolved&&) = delete;
        Evolved& operator=(Evolved&&) = delete;
        ~Evolved() = default;

        Landform landform;
        Meander meander;
        Rivers rivers;
    };

    [[nodiscard]] std::int64_t mountain_strength(CellId cell, std::int64_t height) const;
    // The elevation as generated, before rivers wore it.
    [[nodiscard]] std::int64_t generated(CellId cell) const;
    // The elevation as rivers wore it, before they cut their channels, given the generated one.
    [[nodiscard]] static std::int64_t worn(CellId cell, std::int64_t generated,
                                           const Landform& landform, const Meander& meander);
    [[nodiscard]] const Evolved& evolved() const;
    [[nodiscard]] std::shared_ptr<const Evolved> evolve(const PlanetConfig& config) const;

    Continents continents_;
    Archipelago archipelago_;
    std::uint64_t belt_seed_;
    std::uint64_t ridge_seed_;
    struct Evolving {
        PlanetConfig config;
        std::once_flag once;
        std::shared_ptr<const Evolved> evolved;
    };
    std::shared_ptr<Evolving> evolving_;
};

} // namespace hog::planet
