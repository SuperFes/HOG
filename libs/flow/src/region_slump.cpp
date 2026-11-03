#include <hog/flow/region.hpp>
#include <hog/flow/slump.hpp>
#include <hog/globe/surface.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

namespace hog::flow {
namespace {

using globe::BrickSite;
using globe::Material;
using globe::Site;
using globe::Voxel;

constexpr std::int64_t full = Voxel::full;
// Passes of the rule a step makes, and the most voxels up a column grain is given room in.
constexpr int passes = 4;
constexpr std::uint32_t tallest = 64;
// Grain lands in a column however far down it must look for the ground, to this many layers.
constexpr std::uint32_t deepest = 1024;
// Grain asked across a longer drop than this falls instead of arriving at once.
constexpr std::int64_t cliff = 2 * full;
// Lumps of grain in flight at most; past it grain slumps across any drop as it did before.
constexpr std::size_t most_parcels = 2048;
// Metres a second grain leaves a lip at.
constexpr double departure = 1.0;
constexpr std::array<std::pair<int, int>, Slump::sides> around{
    {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}}};

struct Key {
    std::uint8_t face = 0;
    std::uint32_t i = 0;
    std::uint32_t j = 0;
    friend constexpr auto operator<=>(const Key&, const Key&) noexcept = default;
};

// A column's top ground and the grain lying on it.
struct Profile {
    std::uint32_t first = 0; // the lowest voxel the grain may fill
    std::int64_t ground = 0;
    std::int64_t grain = 0;
    std::int64_t room = 0;
    std::int64_t rise = 0;
    Material material = Material::Air;
    std::uint32_t voxels = 0; // how many up from `first` hold room
};

} // namespace

void Region::slump(Stepped& stepped) {
    const globe::Layers& layers = world_->store().layers();
    const double thickness = layers.thickness;
    const double gravity = world_->generator().planet().gravity;

    // The columns with grain in the open.
    std::map<Key, int> columns;
    for (const auto& [brick_key, brick] : held_) {
        const BrickSite at = globe::brick_at_key(brick_key);
        for (int k = 0; k < globe::brick_voxels; ++k) {
            const Voxel& voxel = brick.voxels[static_cast<std::size_t>(k)];
            if (voxel.grain_amount > 0 && voxel.solid == Material::Air) {
                const Site site = globe::site_in(at, k);
                columns.try_emplace(Key{site.face, site.i, site.j}, 0);
            }
        }
    }
    if (columns.empty()) {
        return;
    }
    // And those beside them, which the grain may slide into.
    const std::vector<Key> grained = [&] {
        std::vector<Key> keys;
        for (const auto& [key, unused] : columns) {
            keys.push_back(key);
        }
        return keys;
    }();
    for (const Key& key : grained) {
        for (const auto& [di, dj] : around) {
            if (const auto next = beside({key.face, key.i, key.j, 0}, di, dj)) {
                columns.try_emplace(Key{next->face, next->i, next->j}, 0);
            }
        }
    }

    // Each column's ground and grain, where it has a top surface to lie on.
    std::vector<Key> keys;
    std::vector<Profile> profiles;
    for (auto& [key, node] : columns) {
        const auto stack = stacks_.find(
            (std::uint64_t{key.face} << 42U) | (std::uint64_t{key.i / globe::brick_side} << 21U) |
            (key.j / globe::brick_side));
        if (stack == stacks_.end()) {
            node = Slump::none;
            continue;
        }
        // Down from the top brick, through bricks held without a gap, to the highest shaped
        // matter with open voxel over it.
        const std::optional<Scan> found =
            scan(key.face, key.i, key.j, ((stack->second.back() + 1) * globe::brick_side) - 1,
                 globe::any_depth);
        if (!found) {
            // Grain beside ground that isn't held yet has nowhere known to go: take it up.
            const std::uint32_t top = ((stack->second.back() + 1) * globe::brick_side) - 1;
            if (const auto missing = unheld_below(
                    globe::middle_of({key.face, key.i, key.j, top}, layers))) {
                stepped.wanted.push_back(*missing);
            }
            node = Slump::none;
            continue;
        }
        const globe::Surface& surface = found->surface;
        Profile profile{.first = surface.first,
                        .ground = found->ground,
                        .grain = surface.grain_amount,
                        .room = std::int64_t{surface.open} * full,
                        .material = surface.grain,
                        .voxels = surface.open};
        if (surface.grain != Material::Air) {
            const Site base{.face = key.face, .i = key.i, .j = key.j, .layer = surface.first};
            const double width = std::sqrt(layers.volume(globe::column_of(base, layers),
                                                         static_cast<int>(surface.first)) /
                                           thickness);
            profile.rise = static_cast<std::int64_t>(
                std::llround(surface.holds(thickness, gravity) * width / thickness *
                             static_cast<double>(full)));
        }
        node = static_cast<int>(keys.size());
        keys.push_back(key);
        profiles.push_back(profile);
    }

    Slump rule{keys.size()};
    for (std::size_t n = 0; n < keys.size(); ++n) {
        Slump::Node& node = rule[n];
        node.ground = profiles[n].ground;
        node.grain = profiles[n].grain;
        node.rise = profiles[n].rise;
        node.room = profiles[n].room;
        if (parcels_.size() < most_parcels) {
            node.reach = cliff;
        }
        for (std::size_t s = 0; s < Slump::sides; ++s) {
            const auto next = columns.find(Key{.face = keys[n].face,
                                               .i = keys[n].i + static_cast<std::uint32_t>(
                                                                    around[s].first),
                                               .j = keys[n].j + static_cast<std::uint32_t>(
                                                                    around[s].second)});
            // A neighbour beside the face's edge or past what's held is no column to slide to.
            // Any drop is one, however deep: grain pours into pits and over cliffs.
            if (next != columns.end() && next->second != Slump::none) {
                node.next[s] = next->second;
            }
        }
    }
    std::int64_t moved = 0;
    for (int pass = 0; pass < passes; ++pass) {
        const std::int64_t now = rule.step();
        if (now == 0) {
            break;
        }
        moved += now;
    }
    if (moved == 0) {
        return;
    }
    stepped.slumped += moved;

    // What a column's grain is made of: its own, else that of the heaviest neighbour that had some.
    const auto material_of = [&](std::size_t n) {
        Material material = profiles[n].material;
        if (material != Material::Air) {
            return material;
        }
        std::int64_t most = 0;
        for (std::size_t s = 0; s < Slump::sides; ++s) {
            const int next = rule[n].next[s];
            if (next != Slump::none && profiles[static_cast<std::size_t>(next)].grain > most) {
                most = profiles[static_cast<std::size_t>(next)].grain;
                material = profiles[static_cast<std::size_t>(next)].material;
            }
        }
        return material;
    };

    // Grain that went over a cliff leaves flying, a lump for each side it left by, from the lip.
    std::map<std::pair<std::size_t, std::size_t>, std::int64_t> left;
    for (const Slump::Fall& fall : rule.falls()) {
        left[{fall.from, fall.side}] += fall.amount;
    }
    for (const auto& [from_side, amount] : left) {
        const std::size_t n = from_side.first;
        const auto to = static_cast<std::size_t>(rule[n].next[from_side.second]);
        const Site here{keys[n].face, keys[n].i, keys[n].j, profiles[n].first};
        const Site there{keys[to].face, keys[to].i, keys[to].j, profiles[n].first};
        const DVec3 from_middle = globe::middle_of(here, layers);
        const DVec3 to_middle = globe::middle_of(there, layers);
        const double top = layers.floor +
                           (static_cast<double>(profiles[n].ground + profiles[n].grain) /
                            static_cast<double>(full) * thickness);
        const DVec3 lip =
            glm::normalize(from_middle + ((to_middle - from_middle) * 0.5)) * (top + 0.05);
        throw_grain(lip, glm::normalize(to_middle - from_middle) * departure, material_of(n),
                    amount);
    }

    // Write each column's grain back, filling its voxels from the ground up. A column that gains
    // grain where it had none takes the material of the heaviest neighbour that had some.
    for (std::size_t n = 0; n < keys.size(); ++n) {
        const std::int64_t depth = rule[n].grain;
        if (depth == profiles[n].grain) {
            continue;
        }
        const Material material = material_of(n);
        fill_grain({.face = keys[n].face, .i = keys[n].i, .j = keys[n].j, .layer = profiles[n].first},
                   profiles[n].voxels, depth, material);
    }
}

std::optional<Region::Scan> Region::scan(std::uint8_t face, std::uint32_t i, std::uint32_t j,
                                         std::uint32_t start, std::uint32_t reach) {
    const globe::Layers& layers = world_->store().layers();
    const auto lookup = [&](std::uint32_t layer) { return voxel({face, i, j, layer}); };
    std::optional<globe::Surface> surface = globe::surface_below(start, reach, tallest, lookup);
    if (!surface) {
        return std::nullopt;
    }
    // The shaped surface lies below the voxel's middle by its distance, along the normal.
    double offset = 0.0;
    if (surface->bottom.distance != Voxel::far) {
        const Site base{.face = face, .i = i, .j = j, .layer = surface->first};
        const DVec3 normal = globe::unpacked_normal(surface->bottom.normal);
        const DVec3 up = glm::normalize(globe::middle_of(base, layers));
        const double lean = std::max(glm::dot(normal, up), 0.3);
        offset = globe::metres(surface->bottom.distance) / layers.thickness / lean;
    }
    const std::int64_t ground = std::llround(
        (static_cast<double>(surface->first) + 0.5 - offset) * static_cast<double>(full));
    return Scan{.surface = *surface, .ground = ground};
}

void Region::fill_grain(Site column, std::uint32_t voxels, std::int64_t depth, Material material) {
    std::int64_t left = depth;
    for (std::uint32_t up = 0; up < voxels; ++up) {
        const Site site{.face = column.face,
                        .i = column.i,
                        .j = column.j,
                        .layer = column.layer + up};
        Voxel* here = voxel(site);
        const std::int64_t amount = std::min(left, full);
        left -= amount;
        if (here->grain_amount != amount || (amount > 0 && here->grain != material)) {
            here->grain = amount > 0 ? material : Material::Air;
            here->grain_amount = static_cast<std::uint16_t>(amount);
            held(globe::brick_of(site))->dirty = true;
        }
    }
}

void Region::throw_grain(DVec3 at, DVec3 velocity, Material material, std::int64_t amount) {
    if (amount > 0) {
        parcels_.push_back({.id = ++parcel_ids_,
                            .at = at,
                            .velocity = velocity,
                            .material = material,
                            .amount = amount});
    }
}

std::optional<Ground> Region::ground_under(DVec3 at) {
    const globe::Layers& layers = world_->store().layers();
    const double radius = glm::length(at);
    const int layer = layers.layer_at(radius);
    if (layer < 0 || layer >= layers.count) {
        return std::nullopt;
    }
    const globe::Cell column = globe::cell_under(at / radius, layers.level);
    const std::optional<Scan> found =
        scan(column.face, column.i, column.j, static_cast<std::uint32_t>(layer), deepest);
    if (!found) {
        return std::nullopt;
    }
    const globe::Surface& surface = found->surface;
    DVec3 normal = at / radius;
    if (surface.bottom.distance != Voxel::far && surface.bottom.normal != Voxel::no_normal) {
        normal = globe::unpacked_normal(surface.bottom.normal);
    }
    return Ground{.radius = layers.floor + (static_cast<double>(found->ground + surface.grain_amount) /
                                            static_cast<double>(full) * layers.thickness),
                  .normal = normal,
                  .holds = surface.holds(layers.thickness, world_->generator().planet().gravity)};
}

bool Region::land(const Parcel& parcel, Stepped& stepped) {
    const globe::Layers& layers = world_->store().layers();
    const double radius = glm::length(parcel.at);
    const int layer = layers.layer_at(radius);
    if (layer < 0 || layer >= layers.count) {
        return false;
    }
    const globe::Cell column = globe::cell_under(parcel.at / radius, layers.level);
    const std::optional<Scan> found =
        scan(column.face, column.i, column.j, static_cast<std::uint32_t>(layer), deepest);
    if (!found) {
        return false;
    }
    const globe::Surface& surface = found->surface;
    const std::int64_t room = (std::int64_t{surface.open} * full) - surface.grain_amount;
    const std::int64_t put = std::min(parcel.amount, std::max<std::int64_t>(room, 0));
    stepped.scattered += parcel.amount - put;
    if (put > 0) {
        fill_grain({.face = column.face, .i = column.i, .j = column.j, .layer = surface.first},
                   surface.open, surface.grain_amount + put,
                   surface.grain != Material::Air ? surface.grain : parcel.material);
    }
    return true;
}

std::optional<globe::BrickSite> Region::unheld_below(DVec3 at) {
    const globe::Layers& layers = world_->store().layers();
    const double radius = glm::length(at);
    const int start = layers.layer_at(radius);
    if (start < 0 || start >= layers.count) {
        return std::nullopt;
    }
    const globe::Cell column = globe::cell_under(at / radius, layers.level);
    for (std::uint32_t down = 0; down <= deepest && down <= static_cast<std::uint32_t>(start);
         ++down) {
        const Site site{.face = column.face,
                        .i = column.i,
                        .j = column.j,
                        .layer = static_cast<std::uint32_t>(start) - down};
        const Voxel* here = voxel(site);
        if (here == nullptr) {
            return globe::brick_of(site);
        }
        if (here->solid != Material::Air) {
            break;
        }
    }
    return std::nullopt;
}

void Region::fly(double seconds, Stepped& stepped) {
    const globe::Layers& layers = world_->store().layers();
    const double gravity = world_->generator().planet().gravity;
    const auto ground = [&](DVec3 at) { return ground_under(at); };
    std::vector<Parcel> flying;
    flying.reserve(parcels_.size());
    for (Parcel& parcel : parcels_) {
        const Parcel before = parcel;
        if (Flight::advance(parcel, seconds, gravity, 0.5 * layers.thickness, ground) ==
            Flight::Fate::Flying) {
            flying.push_back(parcel);
            continue;
        }
        // Over ground not held yet it waits, hanging, for the bricks to be taken up.
        if (const auto missing = unheld_below(parcel.at);
            missing && before.age + seconds < Flight::longest) {
            stepped.wanted.push_back(*missing);
            flying.push_back(before);
            flying.back().age += seconds;
            flying.back().velocity = {};
            continue;
        }
        if (!land(parcel, stepped)) {
            stepped.scattered += parcel.amount;
        }
    }
    parcels_ = std::move(flying);
    stepped.flying = parcels_.size();
}

} // namespace hog::flow
