#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/climate.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/planet_config.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace hog::planet {

enum class Rock : std::uint8_t {
    Soil,
    Sand,
    Sandstone,
    Shale,
    Limestone,
    Granite,
    Basalt,
    Peridotite,
    Magma,
};

inline constexpr int rock_count = static_cast<int>(Rock::Magma) + 1;

[[nodiscard]] std::string_view to_string(Rock rock) noexcept;

// A cell's ground from the surface down: loose cover, then sedimentary rock, then bedrock down to
// the base of the crust, then mantle rock that runs on without end, perhaps with a magma chamber
// somewhere in it. Thicknesses and depths are fixed-point metres; thicknesses may be zero.
struct Strata {
    Rock cover = Rock::Soil;
    std::int64_t cover_thickness = 0;
    Rock sediment = Rock::Shale;
    std::int64_t sediment_thickness = 0;
    Rock bedrock = Rock::Granite;
    std::int64_t mantle_depth = std::numeric_limits<std::int64_t>::max();
    std::int64_t chamber_top = 0;
    std::int64_t chamber_thickness = 0;

    // The rock at a fixed-point depth in metres below the surface.
    [[nodiscard]] constexpr Rock rock_at(std::int64_t depth) const noexcept {
        if (depth >= chamber_top && depth < chamber_top + chamber_thickness) {
            return Rock::Magma;
        }
        if (depth < cover_thickness) {
            return cover;
        }
        if (depth < cover_thickness + sediment_thickness) {
            return sediment;
        }
        if (depth >= mantle_depth) {
            return Rock::Peridotite;
        }
        return bedrock;
    }

    // Fixed-point metres below the surface to the top of the bedrock.
    [[nodiscard]] constexpr std::int64_t bedrock_depth() const noexcept {
        return cover_thickness + sediment_thickness;
    }
};

enum class Resource : std::uint8_t {
    Clay,
    Coal,
    Salt,
    Iron,
    Copper,
    Tin,
    Gold,
};

inline constexpr int resource_count = static_cast<int>(Resource::Gold) + 1;

[[nodiscard]] std::string_view to_string(Resource resource) noexcept;

inline constexpr std::array rarest_first{
    Resource::Gold, Resource::Tin,  Resource::Copper, Resource::Salt,
    Resource::Coal, Resource::Iron, Resource::Clay,
};

// A flat seam under a cell, in fixed-point metres below the surface; no seam has zero thickness.
// What a cell holds is the seam's thickness times the cell's area, so it is finite at every level.
struct Deposit {
    std::int64_t top = 0;
    std::int64_t thickness = 0;

    [[nodiscard]] constexpr bool empty() const noexcept { return thickness == 0; }
};

// Molten rock under volcanic ground: a flattened ellipsoid lying level beneath the surface.
struct Chamber {
    DVec3 center;             // unit direction over its middle
    std::int64_t depth;       // fixed-point metres below sea level to its middle
    double radius;            // metres from its middle to its rim
    std::int64_t half_height; // fixed-point metres from its middle to its roof
};

// The rarest resource whose seam takes in a fixed-point depth in metres below the surface.
[[nodiscard]] std::optional<Resource>
resource_at(const std::array<Deposit, resource_count>& deposits, std::int64_t depth) noexcept;

// Rock layers and ore deposits from terrain and climate: continents sit on granite under a cover
// of sedimentary rock that mountains strip away, carrying on under the sea as shelves; the deep
// ocean floor and the starting islands are basalt, with a magma chamber under each island.
class Geology {
public:
    explicit Geology(const PlanetConfig& config);

    [[nodiscard]] const Climate& climate() const noexcept { return climate_; }
    [[nodiscard]] const Elevation& terrain() const noexcept { return climate_.terrain(); }

    // Whether continental crust lies under the cell. It reaches out under the sea as a shelf,
    // ending partway down the slope to the ocean floor.
    [[nodiscard]] bool continental(CellId cell) const;

    [[nodiscard]] Strata strata(CellId cell) const;

    // Fixed-point metres from the surface to the mantle: thick under continents and thicker
    // still under mountains, thin under the ocean floor, and thickened under the islands.
    [[nodiscard]] std::int64_t crust_thickness(CellId cell) const;

    // Fixed-point °C of the rock at a fixed-point depth in metres below the surface: warming
    // from the surface, or the cold water on the deep sea floor, down through the crust, then
    // more slowly through the mantle, and hotter near a magma chamber.
    [[nodiscard]] std::int64_t rock_temperature(CellId cell, std::int64_t depth) const;

    // The magma chambers, one under each starting island.
    [[nodiscard]] std::span<const Chamber> chambers() const noexcept { return chambers_; }

    // Every resource's seam under the cell, indexed by resource. The deep ocean floor holds none.
    [[nodiscard]] std::array<Deposit, resource_count> deposits(CellId cell) const;

    // Cubic metres of a seam under the cell.
    [[nodiscard]] double volume(CellId cell, const Deposit& deposit) const;

private:
    [[nodiscard]] Strata strata(CellId cell, Biome biome) const;
    [[nodiscard]] NoisePoint noise_at(CellId cell, double wavelength) const;

    Climate climate_;
    double planet_radius_;
    std::uint64_t facies_seed_;
    std::uint64_t thickness_seed_;
    std::uint64_t deposit_seed_;
    std::uint64_t crust_seed_;
    std::vector<Chamber> chambers_;
};

} // namespace hog::planet
