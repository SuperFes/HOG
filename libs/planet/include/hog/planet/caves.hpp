#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/geology.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hog::planet {

enum class Cave : std::uint8_t {
    Karst,    // passages dissolved through wet limestone
    Sinkhole, // a karst passage's roof fallen in, open to the sky
    LavaTube,
    SeaCave,
};

inline constexpr int cave_count = static_cast<int>(Cave::SeaCave) + 1;

[[nodiscard]] std::string_view to_string(Cave cave) noexcept;

// Metres of clearance a person needs to walk upright, or to crawl; narrower is a squeeze only
// water, small animals or digging get through.
inline constexpr double walk_clearance = 1.0;
inline constexpr double crawl_clearance = 0.4;

// A stretch of passage: a tube between two points, narrowing or widening from one end to the
// other. Points are metres from the planet's middle.
struct Passage {
    DVec3 from;
    DVec3 to;
    double from_radius = 0.0; // metres
    double to_radius = 0.0;
    Cave cave = Cave::Karst;
};

// Open space found at a point: what cave it is, and the passage's radius there.
struct CaveHit {
    Cave cave = Cave::Karst;
    double clearance = 0.0; // metres

    friend bool operator==(const CaveHit&, const CaveHit&) = default;
};

// A lava tube from a magma chamber's vent down towards the sea, and a ball around all of it.
struct LavaTube {
    std::size_t chamber = 0; // in Geology::chambers()
    std::vector<Passage> passages;
    DVec3 middle{};
    double reach = 0.0; // metres from the middle that the tube stays within
};

// The passage's radius at a point, if the point lies inside it.
[[nodiscard]] std::optional<double> clearance_at(const Passage& passage, DVec3 point) noexcept;

// Natural voids under the generated ground, from the rock and the seed.
class Caves {
public:
    explicit Caves(const Geology& geology);

    [[nodiscard]] const Geology& geology() const noexcept { return *geology_; }

    // The open space at a fixed-point depth in metres below a cell's surface, if any. Where
    // passages meet, the roomiest wins.
    [[nodiscard]] std::optional<CaveHit> cave_at(CellId cell, std::int64_t depth) const;

    // Every passage that could reach a cell.
    [[nodiscard]] std::vector<Passage> passages_near(CellId cell) const;

    // The point at a fixed-point depth in metres below a cell's surface.
    [[nodiscard]] DVec3 point_at(CellId cell, std::int64_t depth) const;

    [[nodiscard]] std::span<const LavaTube> lava_tubes() const noexcept { return lava_tubes_; }

    // The level of the cells karst passages run between.
    [[nodiscard]] int karst_level() const noexcept { return karst_level_; }

    // Remembers the passages it has worked out, for many nearby queries on one thread.
    class Lookup {
    public:
        explicit Lookup(const Caves& caves) : caves_{&caves} {}

        [[nodiscard]] std::optional<CaveHit> cave_at(CellId cell, std::int64_t depth);
        [[nodiscard]] std::vector<Passage> passages_near(CellId cell);

    private:
        struct Junction {
            bool open = false;
            DVec3 point{};
            double radius = 0.0;
            std::int64_t depth = 0;
        };

        [[nodiscard]] const Junction& junction(CellId tile);
        [[nodiscard]] const std::vector<Passage>& karst(CellId tile);

        const Caves* caves_;
        std::unordered_map<std::uint64_t, Junction> junctions_;
        std::unordered_map<std::uint64_t, std::vector<Passage>> karst_;
    };

private:
    [[nodiscard]] LavaTube lava_tube(DVec3 vent, std::uint64_t seed) const;

    const Geology* geology_;
    double planet_radius_;
    int karst_level_;
    int point_level_;
    std::uint64_t karst_seed_;
    std::vector<LavaTube> lava_tubes_;
};

} // namespace hog::planet
