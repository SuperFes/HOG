#pragma once

#include <hog/globe/brick.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/world.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace hog::flow {

// What a step did: the voxels' amounts' changes summed, how much the sea beyond the edge gave,
// and the bricks not held yet that liquid stands against and would move into.
struct Stepped {
    std::int64_t changed = 0;
    std::int64_t from_sea = 0;
    std::vector<globe::BrickSite> wanted;
};

// Liquid moving through a world's voxels, over as many of its bricks as are held here. Each
// column's open voxels between the shaped matter make its sections; a section's liquid settles
// to its bottom, and where two neighbouring sections share a layer and one stands higher, liquid
// runs across into the other by the difference in their heads. Liquid pushed into a section
// already full presses in beyond full, and the head that gives pushes on in turn, so liquid finds
// its level through a tunnel too. The columns are square and so is the flow: across the four
// sides alike. Liquid in the pores of shaped matter is left where it is.
class Region {
public:
    // With `hold_sea`, open voxels under the sea's level at the edge of what is held are kept
    // full, the sea beyond being endless.
    explicit Region(globe::World& world, bool hold_sea = true);

    // Reads the brick as the world now has it, unless held already.
    void load(globe::BrickSite site);
    [[nodiscard]] bool holds(globe::BrickSite site) const;
    [[nodiscard]] std::size_t size() const noexcept { return held_.size(); }

    // Precondition: its brick is held.
    [[nodiscard]] globe::Voxel at(globe::Site site) const;
    // Sets the voxel's liquid to `amount` of the world's own.
    void pour(globe::Site site, std::uint16_t amount);
    // All the liquid in the open voxels held.
    [[nodiscard]] std::int64_t liquid() const;

    Stepped step();
    // Writes the bricks changed since they were loaded or last flushed, and says which.
    std::vector<globe::BrickSite> flush();

    // The liquid the last step carried out of a column across its +i and +j sides, less what
    // came in across them: the current, for wheels, boats and what the water carries.
    struct Flux {
        std::int64_t along_i = 0;
        std::int64_t along_j = 0;
    };
    [[nodiscard]] Flux flux(std::uint8_t face, std::uint32_t i, std::uint32_t j) const;

private:
    struct Held {
        globe::BrickVoxels voxels{};
        bool dirty = false;
    };
    // A run of open voxels up a column, `bottom` up to but not `top`, and the liquid in it.
    struct Section {
        int bottom = 0;
        int top = 0;
        std::int64_t amount = 0;
        globe::Material liquid = globe::Material::Air;
    };
    struct Column {
        std::uint8_t face = 0;
        std::uint32_t i = 0;
        std::uint32_t j = 0;
        std::size_t first = 0; // of its sections
        std::size_t count = 0;
    };
    // Where two sections meet: open at the same layers, in columns side by side.
    struct Meeting {
        std::size_t here = 0;
        std::size_t there = 0;
        int window = 0; // the lowest layer both are open at
        std::size_t column = 0;
        bool along_i = true; // `there` is across its +i side, else its +j side
    };
    struct Transfer {
        std::size_t from = 0;
        std::size_t to = 0;
        std::int64_t amount = 0;
        std::size_t column = 0; // whose +i or +j side it crosses
        bool along_i = true;
        bool outward = true; // out of `column`
    };

    [[nodiscard]] Held* held(globe::BrickSite site);
    [[nodiscard]] const Held* held(globe::BrickSite site) const;
    [[nodiscard]] globe::Voxel* voxel(globe::Site site);
    // The site `di` and `dj` across from it on its face, or none past the face's edge.
    [[nodiscard]] std::optional<globe::Site> beside(globe::Site site, int di, int dj) const;
    [[nodiscard]] bool open_to_sea(globe::Site site) const;
    // Whether any of the four voxels beside it isn't held.
    [[nodiscard]] bool at_edge(globe::Site site);

    [[nodiscard]] std::vector<Column> columns() const;
    // Calls `each(site, brick, voxel)` up the column, open voxel after open voxel, and `gap()`
    // where a run ends.
    template <typename Each, typename Gap>
    void walk(const Column& column, const Each& each, const Gap& gap);
    void hold_sea(const std::vector<Column>& columns, Stepped& stepped);
    void gather(std::vector<Column>& columns);
    [[nodiscard]] std::vector<Meeting> meetings(const std::vector<Column>& columns) const;
    // Adds where the sections of two columns side by side meet, `there` across `here`'s +i or
    // +j side.
    void meet(std::size_t c, const Column& here, const Column& there, bool along_i,
              std::vector<Meeting>& meetings) const;
    // Each section's head: its surface, or what pushes on it when full.
    [[nodiscard]] std::vector<std::int64_t> heads(const std::vector<Meeting>& meetings) const;
    [[nodiscard]] std::vector<Transfer> transfers(const std::vector<Column>& columns) const;
    void want_around(globe::Site site, std::set<std::uint64_t>& wanted);
    // Scales the transfers at each section's `end` down to `limit(section)` where `asked` of it
    // exceeds that.
    static void scale(std::vector<Transfer>& transfers, std::size_t Transfer::* end,
                      const std::vector<std::int64_t>& asked, const auto& limit);
    void settle(const std::vector<Column>& columns, Stepped& stepped);

    globe::World* world_;
    bool hold_sea_;
    globe::Material liquid_;
    std::unordered_map<std::uint64_t, Held> held_;
    // The brick layers held up each column of bricks, in order.
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> stacks_;
    std::vector<Section> sections_;
    std::unordered_map<std::uint64_t, std::size_t> column_at_; // into columns(), by its key
    std::unordered_map<std::uint64_t, Flux> flux_;             // the last step's, by column
};

} // namespace hog::flow
