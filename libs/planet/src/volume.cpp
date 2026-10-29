#include <hog/core/assert.hpp>
#include <hog/core/bytes.hpp>
#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/planet/materials.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/volume.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <vector>

namespace hog::planet {

namespace {

[[nodiscard]] bool overlaps(VolumeCell a, VolumeCell b) {
    const CellId column = a.column();
    const bool across = column.contains(b.column()) || b.column().contains(column);
    const VolumeCell a_shells = VolumeCell::from(column, a.shell_level(), a.shell());
    const VolumeCell b_shells = VolumeCell::from(column, b.shell_level(), b.shell());
    return across && (a_shells.contains(b_shells) || b_shells.contains(a_shells));
}

} // namespace

Volume::Volume(Contents everywhere, int region_level)
    : nodes_(face_count, Node{.split = Split::Leaf, .children = 0, .contents = everywhere}),
      region_level_(region_level) {
    HOG_ASSERT(region_level >= 0 && region_level <= max_level, "region level out of range");
}

VolumeCell Volume::child(VolumeCell at, Split split, int index) {
    return split == Split::Across ? at.across_child(index) : at.shell_child(index);
}

std::optional<int> Volume::child_holding(VolumeCell at, Split split, VolumeCell cell) {
    for (int index = 0; index < child_count(split); ++index) {
        if (child(at, split, index).contains(cell)) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<std::uint32_t> Volume::find(VolumeCell cell) const {
    HOG_ASSERT(cell.is_valid(), "invalid cell");
    auto node = static_cast<std::uint32_t>(cell.column().face());
    VolumeCell at = VolumeCell::from(CellId::from_face(cell.column().face()));
    while (at != cell) {
        const Node& here = nodes_[node];
        if (here.split == Split::Leaf) {
            return std::nullopt;
        }
        const auto index = child_holding(at, here.split, cell);
        if (!index) {
            return std::nullopt;
        }
        node = here.children + static_cast<std::uint32_t>(*index);
        at = child(at, here.split, *index);
    }
    return node;
}

std::optional<Volume::Leaf> Volume::leaf_holding(VolumeCell cell) const {
    HOG_ASSERT(cell.is_valid(), "invalid cell");
    auto node = static_cast<std::uint32_t>(cell.column().face());
    VolumeCell at = VolumeCell::from(CellId::from_face(cell.column().face()));
    while (nodes_[node].split != Split::Leaf) {
        const Node& here = nodes_[node];
        const auto index = child_holding(at, here.split, cell);
        if (!index) {
            return std::nullopt;
        }
        node = here.children + static_cast<std::uint32_t>(*index);
        at = child(at, here.split, *index);
    }
    return Leaf{.cell = at, .contents = nodes_[node].contents};
}

bool Volume::splits_across(VolumeCell cell) const {
    HOG_ASSERT(cell.is_valid(), "invalid cell");
    auto node = static_cast<std::uint32_t>(cell.column().face());
    VolumeCell at = VolumeCell::from(CellId::from_face(cell.column().face()));
    while (nodes_[node].split != Split::Leaf) {
        const Node& here = nodes_[node];
        const auto index = child_holding(at, here.split, cell);
        if (!index) {
            return here.split == Split::Across;
        }
        node = here.children + static_cast<std::uint32_t>(*index);
        at = child(at, here.split, *index);
    }
    HOG_ASSERT(false, "a leaf holds the cell");
    return false;
}

void Volume::set(VolumeCell cell, Contents contents) {
    HOG_ASSERT(cell.is_valid(), "invalid cell");
    const int face = cell.column().face();
    fill(static_cast<std::uint32_t>(face), VolumeCell::from(CellId::from_face(face)), cell,
         contents);
}

void Volume::fill(std::uint32_t node, VolumeCell at, VolumeCell cell, Contents contents) {
    std::vector<Pending> pending{{.node = node, .at = at}};
    while (!pending.empty()) {
        const Pending next = pending.back();
        pending.pop_back();
        if (cell.contains(next.at)) {
            prune(next.node);
            nodes_[next.node].contents = contents;
            continue;
        }
        if (nodes_[next.node].split == Split::Leaf) {
            if (nodes_[next.node].contents == contents) {
                continue;
            }
            const int across = std::max(cell.column().level(), region_level_);
            split(next.node, next.at.column().level() < across ? Split::Across : Split::Outward);
        }
        const Node& here = nodes_[next.node];
        for (int index = 0; index < child_count(here.split); ++index) {
            const VolumeCell under = child(next.at, here.split, index);
            if (overlaps(under, cell)) {
                pending.push_back(
                    {.node = here.children + static_cast<std::uint32_t>(index), .at = under});
            }
        }
    }
}

void Volume::split(std::uint32_t node, Split split) {
    const int count = child_count(split);
    auto& free = split == Split::Across ? free_quads_ : free_pairs_;
    std::uint32_t first = 0;
    if (free.empty()) {
        first = static_cast<std::uint32_t>(nodes_.size());
        nodes_.resize(nodes_.size() + static_cast<std::size_t>(count));
    } else {
        first = free.back();
        free.pop_back();
    }
    const Contents contents = nodes_[node].contents;
    for (int index = 0; index < count; ++index) {
        nodes_[first + static_cast<std::uint32_t>(index)] =
            Node{.split = Split::Leaf, .children = 0, .contents = contents};
    }
    nodes_[node].split = split;
    nodes_[node].children = first;
    leaves_ += static_cast<std::size_t>(count) - 1;
}

void Volume::prune(std::uint32_t node) {
    std::vector<std::uint32_t> pending{node};
    while (!pending.empty()) {
        const std::uint32_t next = pending.back();
        pending.pop_back();
        const Node here = nodes_[next];
        if (here.split == Split::Leaf) {
            continue;
        }
        const int count = child_count(here.split);
        for (int index = 0; index < count; ++index) {
            pending.push_back(here.children + static_cast<std::uint32_t>(index));
        }
        (here.split == Split::Across ? free_quads_ : free_pairs_).push_back(here.children);
        nodes_[next].split = Split::Leaf;
        leaves_ -= static_cast<std::size_t>(count) - 1;
    }
}

bool Volume::merge(VolumeCell cell) {
    const auto node = find(cell);
    return node && join(*node);
}

bool Volume::join(std::uint32_t node) {
    const Node here = nodes_[node];
    if (here.split == Split::Leaf) {
        return false;
    }
    const Node& first = nodes_[here.children];
    for (int index = 0; index < child_count(here.split); ++index) {
        const Node& under = nodes_[here.children + static_cast<std::uint32_t>(index)];
        if (under.split != Split::Leaf || under.contents != first.contents) {
            return false;
        }
    }
    const Contents contents = first.contents;
    prune(node);
    nodes_[node].contents = contents;
    return true;
}

void Volume::settle() {
    // Parents come before their children, so joining from the back joins the finest first.
    std::vector<std::uint32_t> split;
    std::vector<std::uint32_t> pending{0, 1, 2, 3, 4, 5};
    while (!pending.empty()) {
        const std::uint32_t next = pending.back();
        pending.pop_back();
        const Node& here = nodes_[next];
        if (here.split == Split::Leaf) {
            continue;
        }
        split.push_back(next);
        for (int index = 0; index < child_count(here.split); ++index) {
            pending.push_back(here.children + static_cast<std::uint32_t>(index));
        }
    }
    for (const std::uint32_t node : std::views::reverse(split)) {
        join(node);
    }
}

namespace {

// Cubic metres between a column's sides and two spheres.
[[nodiscard]] double cell_volume(VolumeCell cell, double radius) {
    const ShellBounds bounds = shell_bounds(cell, radius);
    const double thick = bounds.outer - bounds.inner;
    const double spread = (bounds.outer * bounds.outer) + (bounds.outer * bounds.inner) +
                          (bounds.inner * bounds.inner);
    return cell_area(cell.column()) * thick * spread / 3.0;
}

void add(Budget& budget, std::size_t material, double volume, float temperature) {
    const MaterialProperties held = properties(static_cast<Material>(material));
    budget.volume[material] += volume;
    budget.heat[material] += volume * static_cast<double>(held.density) *
                             static_cast<double>(held.specific_heat) *
                             static_cast<double>(temperature);
}

// Adds what of `leaf` lies in `cell`, which it holds.
void add(Budget& budget, const Volume::Leaf& leaf, VolumeCell cell, double radius) {
    const double volume = cell_volume(cell, radius);
    const float temperature = leaf.contents.temperature;
    if (!leaf.contents.surface.has_value()) {
        add(budget, static_cast<std::size_t>(leaf.contents.material), volume, temperature);
        return;
    }
    // Points spread evenly through the cell, each weighted by the volume about it.
    constexpr int samples = 4;
    const FaceIJ at = cell.column().to_face_ij();
    const double side = std::ldexp(1.0, -at.level);
    const ShellBounds bounds = shell_bounds(cell, radius);
    std::array<double, material_count> weights{};
    double total = 0.0;
    for (int a = 0; a < samples; ++a) {
        for (int b = 0; b < samples; ++b) {
            // Even in u and v, where a patch of the cube stands for 1 / |cube|^3 of the sphere.
            const auto along = [&](std::uint32_t index, int step) {
                const double low = st_to_uv(static_cast<double>(index) * side);
                const double high = st_to_uv(static_cast<double>(index + 1) * side);
                return low + ((high - low) * ((step + 0.5) / samples));
            };
            const DVec3 cube = face_uv_to_xyz(at.face, along(at.i, a), along(at.j, b));
            const DVec3 direction = glm::normalize(cube);
            const double patch = 1.0 / std::pow(glm::length(cube), 3.0);
            for (int c = 0; c < samples; ++c) {
                const double reach =
                    bounds.inner + ((bounds.outer - bounds.inner) * ((c + 0.5) / samples));
                const double weight = patch * reach * reach;
                const Material material =
                    material_at(leaf, radius, WorldPosition{direction * reach});
                weights[static_cast<std::size_t>(material)] += weight;
                total += weight;
            }
        }
    }
    for (std::size_t material = 0; material < material_count; ++material) {
        if (weights[material] > 0.0) {
            add(budget, material, volume * weights[material] / total, temperature);
        }
    }
}

} // namespace

Budget budget(const Volume& volume, double radius) {
    Budget counted;
    volume.for_each_leaf([&](VolumeCell cell, const Contents& contents) {
        add(counted, Volume::Leaf{.cell = cell, .contents = contents}, cell, radius);
    });
    return counted;
}

Budget budget(const Volume& volume, double radius, CellId region) {
    HOG_ASSERT(region.level() == volume.region_level(), "a region is a column at the region level");
    Budget counted;
    volume.for_each_leaf_in(region, [&](const Volume::Leaf& leaf, VolumeCell part) {
        add(counted, leaf, part, radius);
    });
    return counted;
}

std::optional<Volume::Leaf> leaf_at(const Volume& volume, double radius, WorldPosition position) {
    const VolumeCell finest = volume_cell_at(position, radius, max_level, max_shell_level);
    if (!finest.is_valid()) {
        return std::nullopt;
    }
    return volume.leaf_holding(finest);
}

namespace {

constexpr std::array<std::byte, 8> region_magic{std::byte{'H'}, std::byte{'O'}, std::byte{'G'},
                                                std::byte{'V'}, std::byte{'O'}, std::byte{'L'},
                                                std::byte{'0'}, std::byte{'1'}};

void write_contents(ByteWriter& out, const Contents& contents) {
    out.write(static_cast<std::uint8_t>(contents.material));
    out.write(std::bit_cast<std::uint32_t>(contents.temperature));
    out.write(static_cast<std::uint8_t>(contents.surface.has_value() ? 1 : 0));
    if (contents.surface.has_value()) {
        const Surface surface = contents.surface.value_or(Surface{});
        out.write(static_cast<std::uint8_t>(surface.under));
        for (glm::length_t axis = 0; axis < 3; ++axis) {
            out.write(std::bit_cast<std::uint32_t>(surface.normal[axis]));
        }
        out.write(std::bit_cast<std::uint32_t>(surface.offset));
    }
}

[[nodiscard]] Material read_material(ValueReader& in) {
    const auto material = in.integer<std::uint8_t>();
    if (material >= material_count) {
        in.fail(Error{.code = ErrorCode::Parse, .message = "unknown material"});
    }
    return static_cast<Material>(material);
}

[[nodiscard]] Contents read_contents(ValueReader& in) {
    Contents contents{
        .material = read_material(in), .temperature = in.single(), .surface = std::nullopt};
    const auto surfaced = in.integer<std::uint8_t>();
    if (surfaced > 1) {
        in.fail(Error{.code = ErrorCode::Parse, .message = "bad surface flag"});
    }
    if (surfaced == 1) {
        Surface surface{.under = read_material(in), .normal = {}, .offset = 0.0F};
        for (glm::length_t axis = 0; axis < 3; ++axis) {
            surface.normal[axis] = in.single();
        }
        surface.offset = in.single();
        contents.surface = surface;
    }
    return contents;
}

} // namespace

std::uint32_t Volume::reach(CellId region) {
    auto node = static_cast<std::uint32_t>(region.face());
    for (int level = 1; level <= region.level(); ++level) {
        if (nodes_[node].split == Split::Leaf) {
            split(node, Split::Across);
        }
        const FaceIJ at = region.parent(level).to_face_ij();
        node = nodes_[node].children + (((at.i & 1U) << 1U) | (at.j & 1U));
    }
    return node;
}

std::vector<std::byte> Volume::bake(CellId region, std::uint64_t code) const {
    HOG_ASSERT(region.is_valid() && region.level() == region_level_,
               "a region is a column at the region level");
    ByteWriter out;
    out.write_bytes(region_magic);
    out.write(code);
    out.write(region.raw());
    // Down to the region, or to the leaf it's in.
    auto node = static_cast<std::uint32_t>(region.face());
    for (int level = 1; level <= region.level() && nodes_[node].split != Split::Leaf; ++level) {
        const FaceIJ at = region.parent(level).to_face_ij();
        node = nodes_[node].children + (((at.i & 1U) << 1U) | (at.j & 1U));
    }
    std::vector<std::uint32_t> pending{node};
    while (!pending.empty()) {
        const Node& here = nodes_[pending.back()];
        pending.pop_back();
        out.write(static_cast<std::uint8_t>(here.split));
        if (here.split == Split::Leaf) {
            write_contents(out, here.contents);
            continue;
        }
        for (int index = child_count(here.split) - 1; index >= 0; --index) {
            pending.push_back(here.children + static_cast<std::uint32_t>(index));
        }
    }
    return std::move(out).take();
}

std::vector<Volume::BakedNode> Volume::read_tree(ValueReader& in, CellId region) {
    std::vector<BakedNode> read;
    std::vector<VolumeCell> pending{VolumeCell::from(region)};
    while (!pending.empty() && in.ok()) {
        const VolumeCell at = pending.back();
        pending.pop_back();
        const auto split = in.integer<std::uint8_t>();
        const bool across =
            split == static_cast<std::uint8_t>(Split::Across) && at.column().level() < max_level;
        const bool outward = split == static_cast<std::uint8_t>(Split::Outward) &&
                             at.shell_level() < max_shell_level;
        if (split == static_cast<std::uint8_t>(Split::Leaf)) {
            read.push_back({.split = Split::Leaf, .contents = read_contents(in)});
        } else if (across || outward) {
            const Split kind = across ? Split::Across : Split::Outward;
            read.push_back({.split = kind, .contents = {}});
            for (int index = child_count(kind) - 1; index >= 0; --index) {
                pending.push_back(child(at, kind, index));
            }
        } else {
            in.fail(Error{.code = ErrorCode::Parse, .message = "bad split"});
        }
    }
    return read;
}

std::optional<Volume::Baked> Volume::load(std::span<const std::byte> bytes) {
    ValueReader in{bytes};
    for (const std::byte expected : region_magic) {
        if (in.integer<std::uint8_t>() != std::to_integer<std::uint8_t>(expected)) {
            return std::nullopt;
        }
    }
    const auto code = in.integer<std::uint64_t>();
    const CellId region = CellId::from_raw(in.integer<std::uint64_t>());
    if (!in.ok() || !region.is_valid() || region.level() != region_level_) {
        return std::nullopt;
    }
    const std::vector<BakedNode> read = read_tree(in, region);
    if (!in.finish()) {
        return std::nullopt;
    }
    const std::uint32_t root = reach(region);
    prune(root);
    std::vector<std::uint32_t> filling{root};
    for (const BakedNode& next : read) {
        const std::uint32_t node = filling.back();
        filling.pop_back();
        if (next.split == Split::Leaf) {
            nodes_[node].contents = next.contents;
            continue;
        }
        split(node, next.split);
        for (int index = child_count(next.split) - 1; index >= 0; --index) {
            filling.push_back(nodes_[node].children + static_cast<std::uint32_t>(index));
        }
    }
    return Baked{.region = region, .code = code};
}

namespace {

// How far over a leaf's surface a position is; below zero, it's under it.
[[nodiscard]] double over(const Volume::Leaf& leaf, const Surface& surface, double radius,
                          WorldPosition position) {
    const DVec3 from_center = position - volume_center(leaf.cell, radius);
    return glm::dot(from_center, DVec3{surface.normal}) - static_cast<double>(surface.offset);
}

} // namespace

Material material_at(const Volume::Leaf& leaf, double radius, WorldPosition position) {
    if (!leaf.contents.surface.has_value()) {
        return leaf.contents.material;
    }
    const Surface surface = leaf.contents.surface.value_or(Surface{});
    return over(leaf, surface, radius, position) >= 0.0 ? leaf.contents.material : surface.under;
}

LeafCrossing cross_leaf(const Volume::Leaf& leaf, double radius, WorldPosition from,
                        DVec3 direction) {
    LeafCrossing crossing{.first = material_at(leaf, radius, from),
                          .then = std::nullopt,
                          .surface = 0.0,
                          .exit = exit_distance(leaf.cell, radius, from, direction)};
    if (!leaf.contents.surface.has_value()) {
        return crossing;
    }
    const Surface surface = leaf.contents.surface.value_or(Surface{});
    const double toward = glm::dot(direction, DVec3{surface.normal});
    const double height = over(leaf, surface, radius, from);
    // Only heading toward the plane from either side does the ray reach it.
    if (toward == 0.0 || (height >= 0.0) == (toward > 0.0)) {
        return crossing;
    }
    const double along = -height / toward;
    if (along < crossing.exit) {
        crossing.then = height >= 0.0 ? surface.under : leaf.contents.material;
        crossing.surface = along;
    }
    return crossing;
}

} // namespace hog::planet
