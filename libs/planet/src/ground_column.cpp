#include <hog/core/assert.hpp>
#include <hog/planet/ground_column.hpp>

#include <algorithm>
#include <optional>
#include <utility>

namespace hog::planet {

namespace {

// Runs laid upward, joining one to the run under it where they're alike and dropping any with
// nothing in them.
class Stack {
public:
    Stack(std::int64_t low, std::int64_t high) : low_{low}, high_{high} {}

    void lay(Material material, std::int64_t top) {
        top = std::min(top, high_);
        if (top <= this->top()) {
            return;
        }
        if (!runs_.empty() && runs_.back().material == material) {
            runs_.back().top = top;
        } else {
            runs_.push_back({.material = material, .top = top});
        }
    }

    [[nodiscard]] std::int64_t top() const { return runs_.empty() ? low_ : runs_.back().top; }
    [[nodiscard]] std::vector<Run> take() && { return std::move(runs_); }

private:
    std::int64_t low_;
    std::int64_t high_;
    std::vector<Run> runs_;
};

// The rock up to `top`, read in the beds' own heights, which are `risen` lower.
void lay_rock(Stack& stack, const Beds& beds, const Layering& layering, std::int64_t top) {
    const BedColumn& column = layering.column;
    const std::int64_t risen = layering.risen;
    while (stack.top() < top) {
        const std::int64_t at = stack.top() - risen;
        const Rock rock = beds.rock_at(column, at);
        // Granite and lava lie in no beds, and run to the basement's top or the ground.
        std::int64_t end = top;
        if (const std::optional<Bed> bed = beds.bed_at(column, at)) {
            end = bed->top + risen;
        } else if (column.continental && at < column.basement) {
            end = column.basement + risen;
        }
        // Lava buries whatever it flowed over.
        if (column.continental && at < column.lava) {
            end = std::min(end, std::int64_t{column.lava} + risen);
        }
        // A bed always ends above where it was read.
        stack.lay(material_of(rock), std::clamp(end, stack.top() + 1, top));
    }
}

} // namespace

Material material_of(Rock rock) {
    switch (rock) {
    case Rock::Soil:
        return Material::Soil;
    case Rock::Sand:
        return Material::Sediment;
    case Rock::Sandstone:
        return Material::Sandstone;
    case Rock::Shale:
        return Material::Mudstone;
    case Rock::Limestone:
        return Material::Limestone;
    case Rock::Granite:
        return Material::Granite;
    case Rock::Basalt:
        return Material::Basalt;
    case Rock::Peridotite:
        return Material::Mantle;
    case Rock::Magma:
        return Material::Magma;
    }
    return Material::Granite;
}

ColumnTop column_top(const Elevation& terrain, CellId cell) {
    const std::int64_t ground = terrain.elevation(cell);
    ColumnTop top{.ground = ground, .water = ground};
    if (const auto standing = terrain.rivers().standing(cell, ground)) {
        top.water = std::max(top.water, standing->level);
    }
    if (const auto river = terrain.river(cell)) {
        top.water = std::max(top.water, river->surface);
    }
    return top;
}

std::vector<Run> ground_column(const Elevation& terrain, CellId cell, const ColumnTop& top,
                               std::int64_t low, std::int64_t high) {
    HOG_ASSERT(low < high, "an empty column");
    const std::int64_t ground = top.ground;
    const std::int64_t bedrock = ground - terrain.soil(cell);
    const std::int64_t water = top.water;
    Stack stack{low, high};
    lay_rock(stack, terrain.beds(), terrain.layering(cell), std::min(bedrock, high));
    stack.lay(Material::Soil, ground);
    stack.lay(Material::Water, water);
    stack.lay(Material::Air, high);
    return std::move(stack).take();
}

} // namespace hog::planet
