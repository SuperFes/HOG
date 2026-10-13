#pragma once

#include <hog/planet/archipelago.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/continents.hpp>
#include <hog/planet/landform.hpp>
#include <hog/planet/planet_config.hpp>

#include <cstdint>
#include <memory>
#include <mutex>

namespace hog::planet {

// Terrain height: continents and the starting islands give the coastline, lowlands and ocean
// floor; ridges raise mountain ranges inside belts away from continental coasts. Rivers then wear
// the land over geological time while the belts keep rising, which carves valleys and drainage.
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

    // Fixed-point metres above sea level: positive on land, negative under the ocean.
    [[nodiscard]] std::int64_t elevation(CellId cell) const;

    // How strongly mountains rise here, in fixed point from 0 to 1.
    [[nodiscard]] std::int64_t mountain_strength(CellId cell) const;

    // The land as rivers left it, evolved when first needed. Elevations built from the same
    // config share one.
    [[nodiscard]] const Landform& landform() const;

private:
    [[nodiscard]] std::int64_t mountain_strength(CellId cell, std::int64_t height) const;
    // The elevation as generated, before rivers wore it.
    [[nodiscard]] std::int64_t generated(CellId cell) const;
    [[nodiscard]] std::shared_ptr<const Landform> evolve(const PlanetConfig& config) const;

    Continents continents_;
    Archipelago archipelago_;
    std::uint64_t belt_seed_;
    std::uint64_t ridge_seed_;
    struct Evolving {
        PlanetConfig config;
        std::once_flag once;
        std::shared_ptr<const Landform> landform;
    };
    std::shared_ptr<Evolving> evolving_;
};

} // namespace hog::planet
