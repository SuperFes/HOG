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

// The column `di` along i and `dj` along j, over the cube's edges onto the next face.
std::optional<Key> across(int level, const Key& key, int di, int dj) {
    const std::optional<globe::Cell> next = globe::next_to(
        {.level = static_cast<std::uint8_t>(level), .face = key.face, .i = key.i, .j = key.j}, di,
        dj);
    return next ? std::optional<Key>{Key{next->face, next->i, next->j}} : std::nullopt;
}

} // namespace

void Region::slump(Stepped& stepped) {
    const globe::Layers& layers = world_->layers();
    const double thickness = layers.thickness;
    const double gravity = world_->planet().body.gravity;

    // The columns with grain in the open.
    std::map<Key, int> columns;
    for (const auto& [brick_key, brick] : held_) {
        const BrickSite at = globe::brick_at_key(brick_key);
        for (int k = 0; k < globe::brick_voxels; ++k) {
            const Voxel& voxel = brick.voxels[static_cast<std::size_t>(k)];
            if (voxel.grain_amount > 0 && voxel.solid == Material::Air &&
                voxel.grain != Material::Ice) {
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
            if (const auto next = across(layers.level, key, di, dj)) {
                columns.try_emplace(*next, 0);
            }
        }
    }

    // Each column's floors, each a run of open voxels over shaped matter with the grain lying on
    // it: the top ground, and the floors of caves under their roofs.
    std::vector<Key> keys;
    std::vector<Profile> profiles;
    std::map<Key, std::vector<std::size_t>> floors_of;
    for (const auto& [key, unused] : columns) {
        const auto stack = stacks_.find((std::uint64_t{key.face} << 42U) |
                                        (std::uint64_t{key.i / globe::brick_side} << 21U) |
                                        (key.j / globe::brick_side));
        if (stack == stacks_.end()) {
            continue;
        }
        const std::uint32_t low = stack->second.front() * globe::brick_side;
        const std::uint32_t high = ((stack->second.back() + 1) * globe::brick_side) - 1;
        // Open to the bottom of what's held: the ground under it isn't, so take it up.
        const Voxel* bottom = voxel({key.face, key.i, key.j, low});
        if (bottom != nullptr && bottom->solid == Material::Air) {
            if (const auto missing =
                    unheld_below(globe::middle_of({key.face, key.i, key.j, low}, layers))) {
                stepped.wanted.push_back(*missing);
            }
        }
        for (const Scan& found : floors(key.face, key.i, key.j, low, high)) {
            const globe::Surface& surface = found.surface;
            Profile profile{.first = surface.first,
                            .ground = found.ground,
                            .grain = surface.grain_amount,
                            .room = std::int64_t{surface.open} * full,
                            .material = surface.grain,
                            .voxels = surface.open};
            if (surface.grain != Material::Air) {
                const Site base{.face = key.face, .i = key.i, .j = key.j, .layer = surface.first};
                const double width = std::sqrt(
                    layers.volume(globe::column_of(base, layers), static_cast<int>(surface.first)) /
                    thickness);
                profile.rise = static_cast<std::int64_t>(
                    std::llround(surface.holds(thickness, gravity) * width / thickness *
                                 static_cast<double>(full)));
            }
            floors_of[key].push_back(keys.size());
            keys.push_back(key);
            profiles.push_back(profile);
        }
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
        // The layer the heap's surface is in.
        const std::int64_t top = std::max<std::int64_t>(
            (profiles[n].ground + profiles[n].grain) / full, profiles[n].first);
        for (std::size_t s = 0; s < Slump::sides; ++s) {
            const auto over = across(layers.level, keys[n], around[s].first, around[s].second);
            const auto beside_it = over ? floors_of.find(*over) : floors_of.end();
            // A neighbour past the cube's corner or what's held is no column to slide to.
            // Any drop is one, however deep: grain pours into pits and over cliffs. It goes to
            // the floor beside it open at the heap's surface, not one through a wall or roof.
            if (beside_it == floors_of.end()) {
                continue;
            }
            for (const std::size_t m : beside_it->second) {
                const std::int64_t first = profiles[m].first;
                if (first <= top &&
                    (top < first + profiles[m].voxels || profiles[m].voxels >= tallest)) {
                    node.next[s] = static_cast<int>(m);
                    break;
                }
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
        const double top =
            layers.floor + (static_cast<double>(profiles[n].ground + profiles[n].grain) /
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
        fill_grain(
            {.face = keys[n].face, .i = keys[n].i, .j = keys[n].j, .layer = profiles[n].first},
            profiles[n].voxels, depth, material);
    }
}

std::optional<Region::Scan> Region::scan(std::uint8_t face, std::uint32_t i, std::uint32_t j,
                                         std::uint32_t start, std::uint32_t reach) {
    const globe::Layers& layers = world_->layers();
    const auto lookup = [&](std::uint32_t layer) { return voxel({face, i, j, layer}); };
    std::optional<globe::Surface> surface =
        globe::surface_below(start, reach, tallest, layers.thickness, lookup);
    if (!surface) {
        return std::nullopt;
    }
    // The ground stands as high in the first open voxel as the shaped matter fills it.
    const std::int64_t ground =
        (std::int64_t{surface->first} * full) + surface->bottom.solid_amount;
    return Scan{.surface = *surface, .ground = ground};
}

void Region::fill_grain(Site column, std::uint32_t voxels, std::int64_t depth, Material material) {
    std::int64_t left = depth;
    for (std::uint32_t up = 0; up < voxels; ++up) {
        const Site site{
            .face = column.face, .i = column.i, .j = column.j, .layer = column.layer + up};
        Voxel* here = voxel(site);
        // Grain lies over the shaped matter the voxel holds.
        const std::int64_t room =
            full - (here->solid == Material::Air ? std::int64_t{here->solid_amount} : full);
        const std::int64_t amount = std::clamp<std::int64_t>(left, 0, room);
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

// The lean of the grain's own surface at a column, from the heights of the columns about it, for
// where it has no neighbours to read it from, `fallback`.
DVec3 Region::heap_normal(const globe::Cell& column, std::uint32_t layer, DVec3 fallback) {
    const globe::Layers& layers = world_->layers();
    const std::uint32_t across = globe::bricks_across(world_->layers()) * globe::brick_side;
    if (column.i == 0 || column.j == 0 || column.i + 1 >= across || column.j + 1 >= across) {
        return fallback;
    }
    const std::uint32_t start =
        std::min<std::uint32_t>(layer + 2, static_cast<std::uint32_t>(layers.count) - 1);
    const auto top = [&](std::uint32_t i, std::uint32_t j) -> std::optional<DVec3> {
        const std::optional<Scan> found = scan(column.face, i, j, start, deepest);
        if (!found) {
            return std::nullopt;
        }
        const double height =
            layers.floor + (static_cast<double>(found->ground + found->surface.grain_amount) /
                            static_cast<double>(full) * layers.thickness);
        const DVec3 middle = globe::middle_of({column.face, i, j, start}, layers);
        return glm::normalize(middle) * height;
    };
    const auto ahead = top(column.i + 1, column.j);
    const auto behind = top(column.i - 1, column.j);
    const auto right = top(column.i, column.j + 1);
    const auto left = top(column.i, column.j - 1);
    if (!ahead || !behind || !right || !left) {
        return fallback;
    }
    DVec3 normal = glm::cross(*ahead - *behind, *right - *left);
    if (glm::dot(normal, fallback) < 0.0) {
        normal = -normal;
    }
    const double length = glm::length(normal);
    return length > 0.0 ? normal / length : fallback;
}

std::optional<Ground> Region::ground_under(DVec3 at) {
    const globe::Layers& layers = world_->layers();
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
    const DVec3 normal = heap_normal(column, static_cast<std::uint32_t>(layer), at / radius);
    return Ground{.radius =
                      layers.floor + (static_cast<double>(found->ground + surface.grain_amount) /
                                      static_cast<double>(full) * layers.thickness),
                  .normal = normal,
                  .holds = surface.holds(layers.thickness, world_->planet().body.gravity)};
}

bool Region::land(const Parcel& parcel, Stepped& stepped) {
    const globe::Layers& layers = world_->layers();
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
    const globe::Layers& layers = world_->layers();
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
    const globe::Layers& layers = world_->layers();
    const double gravity = world_->planet().body.gravity;
    std::vector<Parcel> flying;
    flying.reserve(parcels_.size());
    for (Parcel& parcel : parcels_) {
        const Parcel before = parcel;
        // A boulder's middle stays its radius above the ground, and it rolls on gentler slopes.
        const auto ground = [&](DVec3 at) {
            std::optional<Ground> found = ground_under(at);
            if (found && parcel.radius > 0.0) {
                found->radius += parcel.radius;
                found->holds = std::min(found->holds, rolls_down);
            }
            return found;
        };
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
        if (parcel.radius > 0.0) {
            // A block comes to rest as ground, if its surroundings are held to build it in.
            if (const auto absent = settle_boulder(parcel);
                absent && before.age + seconds < Flight::longest) {
                stepped.wanted.push_back(*absent);
                flying.push_back(before);
                flying.back().age += seconds;
                flying.back().velocity = {};
            }
            continue;
        }
        if (!land(parcel, stepped)) {
            stepped.scattered += parcel.amount;
        }
    }
    parcels_ = std::move(flying);
    stepped.flying = parcels_.size();
}

bool Region::drop_hanging(std::uint8_t face, std::uint32_t i, std::uint32_t j, const Scan& floor) {
    const globe::Layers& layers = world_->layers();
    bool dropped = false;
    bool gap = false;
    for (std::uint32_t up = 0; up < floor.surface.open; ++up) {
        const Site site{.face = face, .i = i, .j = j, .layer = floor.surface.first + up};
        Voxel* here = voxel(site);
        const std::int64_t amount = here->grain_amount;
        // Grain over a voxel that isn't full has nothing under it to lie on.
        if (gap && amount > 0) {
            throw_grain(globe::middle_of(site, layers), {}, here->grain, amount);
            here->grain = Material::Air;
            here->grain_amount = 0;
            held(globe::brick_of(site))->dirty = true;
            dropped = true;
        }
        gap = gap || amount < full;
    }
    return dropped;
}

std::vector<Region::Scan> Region::floors(std::uint8_t face, std::uint32_t i, std::uint32_t j,
                                         std::uint32_t low, std::uint32_t high) {
    std::vector<Scan> found;
    for (std::uint32_t layer = low; layer < high; ++layer) {
        const Voxel* bed = voxel({face, i, j, layer});
        const Voxel* over = voxel({face, i, j, layer + 1});
        if (bed == nullptr || bed->solid == Material::Air || over == nullptr ||
            over->solid != Material::Air) {
            continue;
        }
        std::optional<Scan> floor = scan(face, i, j, layer + 1, 1);
        if (floor && parcels_.size() < most_parcels && drop_hanging(face, i, j, *floor)) {
            floor = scan(face, i, j, layer + 1, 1);
        }
        if (floor) {
            found.push_back(*floor);
        }
    }
    return found;
}

} // namespace hog::flow
