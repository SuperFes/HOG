#include <hog/flow/region.hpp>
#include <hog/flow/support.hpp>
#include <hog/flow/wear.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <set>
#include <tuple>
#include <utility>

namespace hog::flow {
namespace {

// A brick waits this many steps for the bricks about it to be taken up.
constexpr int most_tries = 40;
// Bricks across the box looked at: the brick and one either side.
constexpr int box_bricks = 3;
// Pieces of fallen ground between these volumes, in cubic metres, are boulders; smaller ones
// are rubble already, larger break up as they fall.
constexpr double least_boulder = 0.5;
constexpr double most_boulder = 150.0;
// The share of a voxel's width its removal takes out around its middle: a ball that covers it.
constexpr double carve_ball = 0.75;

// Numbers the pieces of `sites`, voxels touching by a face, into `piece`; how many there are.
int find_pieces(const std::vector<globe::Site>& sites, std::vector<int>& piece) {
    std::map<std::tuple<std::uint8_t, std::uint32_t, std::uint32_t, std::uint32_t>, std::size_t>
        index;
    for (std::size_t k = 0; k < sites.size(); ++k) {
        index.emplace(std::tuple{sites[k].face, sites[k].i, sites[k].j, sites[k].layer}, k);
    }
    int pieces = 0;
    for (std::size_t k = 0; k < sites.size(); ++k) {
        if (piece[k] >= 0) {
            continue;
        }
        std::vector<std::size_t> open{k};
        piece[k] = pieces;
        while (!open.empty()) {
            const std::size_t at = open.back();
            open.pop_back();
            const globe::Site s = sites[at];
            for (const auto& [di, dj, dl] :
                 {std::tuple{1, 0, 0}, std::tuple{-1, 0, 0}, std::tuple{0, 1, 0},
                  std::tuple{0, -1, 0}, std::tuple{0, 0, 1}, std::tuple{0, 0, -1}}) {
                const auto found = index.find({s.face, s.i + static_cast<std::uint32_t>(di),
                                               s.j + static_cast<std::uint32_t>(dj),
                                               s.layer + static_cast<std::uint32_t>(dl)});
                if (found != index.end() && piece[found->second] < 0) {
                    piece[found->second] = pieces;
                    open.push_back(found->second);
                }
            }
        }
        ++pieces;
    }
    return pieces;
}

} // namespace

void Region::disturb(globe::BrickSite site, double weathered) {
    const auto queued = std::ranges::find_if(inspect_, [&](const Fresh& waiting) {
        return globe::brick_key(waiting.site) == globe::brick_key(site);
    });
    if (queued == inspect_.end()) {
        inspect_.push_back({.site = site, .weathered = weathered});
    } else {
        queued->weathered += weathered;
    }
}

std::vector<globe::Site> Region::weather(globe::BrickSite site, double seconds) {
    std::vector<globe::Site> gone;
    const std::uint64_t round = ++weathering_;
    for (std::uint32_t l = 0; l < globe::brick_side; ++l) {
        for (std::uint32_t j = 0; j < globe::brick_side; ++j) {
            for (std::uint32_t i = 0; i < globe::brick_side; ++i) {
                const globe::Site where{.face = site.face,
                                        .i = (site.i * globe::brick_side) + i,
                                        .j = (site.j * globe::brick_side) + j,
                                        .layer = (site.layer * globe::brick_side) + l};
                if (weather_voxel(site, where, round, seconds)) {
                    gone.push_back(where);
                }
            }
        }
    }
    return gone;
}

bool Region::weather_voxel(globe::BrickSite site, globe::Site where, std::uint64_t round,
                           double seconds) {
    const double thickness = world_->layers().thickness;
    globe::Voxel* here = voxel(where);
    if (here == nullptr || here->solid == globe::Material::Air || here->condition == 0) {
        return false;
    }
    Exposure exposure;
    exposure.damp = globe::dampness(here->solid, here->liquid_amount, thickness);
    int open = 0;
    for (const auto& [di, dj, dl] :
         {std::tuple{1, 0, 0}, std::tuple{-1, 0, 0}, std::tuple{0, 1, 0}, std::tuple{0, -1, 0},
          std::tuple{0, 0, 1}, std::tuple{0, 0, -1}}) {
        const globe::Voxel* next = voxel({.face = where.face,
                                          .i = where.i + static_cast<std::uint32_t>(di),
                                          .j = where.j + static_cast<std::uint32_t>(dj),
                                          .layer = where.layer + static_cast<std::uint32_t>(dl)});
        if (next != nullptr && next->solid == globe::Material::Air) {
            ++open;
            exposure.damp =
                std::max(exposure.damp,
                         globe::dampness(globe::Material::Air, next->liquid_amount, thickness));
        }
    }
    exposure.open = open / 6.0;
    // Thaw and frost alternate within a few degrees of freezing.
    exposure.freeze =
        std::max(0.0, 1.0 - (std::abs(globe::kelvin(here->temperature) - 273.15) / 8.0));
    const double rate = wearing_rate(here->solid, exposure);
    if (rate <= 0.0) {
        return false;
    }
    const std::uint64_t key =
        (std::uint64_t{where.face} << 60U) ^ (std::uint64_t{where.i} * 73'856'093U) ^
        (std::uint64_t{where.j} * 19'349'663U) ^ (std::uint64_t{where.layer} * 83'492'791U);
    const double chance = softness(key ^ (round * 0x9E3779B97F4A7C15ULL)) / 2.6;
    const std::uint8_t after = worn(here->condition, rate * softness(key), seconds, chance);
    if (after == here->condition) {
        return false;
    }
    here->condition = after;
    held(site)->dirty = true;
    return after == 0;
}

void Region::inspect_support(double seconds, Stepped& stepped) {
    const std::uint32_t across = globe::bricks_across(world_->layers());
    const auto up = static_cast<std::uint32_t>((world_->layers().count + globe::brick_side - 1) /
                                               globe::brick_side);
    std::vector<Fresh> waiting;
    std::size_t looked = 0;
    for (Fresh& queued : inspect_) {
        const globe::BrickSite at = queued.site;
        // Matter at the edge of the world has no box to be judged in.
        if (at.i == 0 || at.j == 0 || at.layer == 0 || at.i + 1 >= across || at.j + 1 >= across ||
            at.layer + 1 >= up) {
            continue;
        }
        if (looked >= most_inspected) {
            waiting.push_back(queued);
            continue;
        }
        if (!box_held(at, stepped)) {
            if (++queued.tries < most_tries) {
                waiting.push_back(queued);
            }
            continue;
        }
        ++looked;
        if (queued.weathered > 0.0) {
            const std::vector<globe::Site> gone = weather(at, queued.weathered);
            queued.weathered = 0.0;
            if (!gone.empty()) {
                fall(gone, stepped);
            }
        }
        if (check_support(at, seconds, stepped)) {
            waiting.push_back(queued);
        }
    }
    inspect_ = std::move(waiting);
}

bool Region::box_held(globe::BrickSite at, Stepped& stepped) const {
    bool all_held = true;
    for (std::uint32_t l = at.layer - 1; l <= at.layer + 1; ++l) {
        for (std::uint32_t j = at.j - 1; j <= at.j + 1; ++j) {
            for (std::uint32_t i = at.i - 1; i <= at.i + 1; ++i) {
                const globe::BrickSite brick{.face = at.face, .i = i, .j = j, .layer = l};
                if (!holds(brick)) {
                    all_held = false;
                    stepped.wanted.push_back(brick);
                }
            }
        }
    }
    return all_held;
}

bool Region::check_support(globe::BrickSite site, double seconds, Stepped& stepped) {
    constexpr int side = box_bricks * globe::brick_side;
    const globe::Layers& layers = world_->layers();
    const std::uint32_t i0 = (site.i - 1) * globe::brick_side;
    const std::uint32_t j0 = (site.j - 1) * globe::brick_side;
    const std::uint32_t l0 = (site.layer - 1) * globe::brick_side;
    Support support{side, side, side, layers.thickness, world_->planet().body.gravity};
    for (int l = 0; l < side; ++l) {
        for (int j = 0; j < side; ++j) {
            for (int i = 0; i < side; ++i) {
                const globe::Voxel* here = voxel({.face = site.face,
                                                  .i = i0 + static_cast<std::uint32_t>(i),
                                                  .j = j0 + static_cast<std::uint32_t>(j),
                                                  .layer = l0 + static_cast<std::uint32_t>(l)});
                if (here != nullptr) {
                    support.at(i, j, l) = {.solid = here->solid, .condition = here->condition};
                }
            }
        }
    }

    const int drop = std::max(1, static_cast<int>(std::ceil(255.0 * seconds / cracks_in)));
    std::vector<globe::Site> falling;
    bool cracking = false;
    for (const Support::Run& run : support.failing()) {
        for (int l = run.bottom; l <= run.top; ++l) {
            const globe::Site where{.face = site.face,
                                    .i = i0 + static_cast<std::uint32_t>(run.i),
                                    .j = j0 + static_cast<std::uint32_t>(run.j),
                                    .layer = l0 + static_cast<std::uint32_t>(l)};
            globe::Voxel* here = voxel(where);
            if (std::cmp_less_equal(here->condition, drop)) {
                falling.push_back(where);
            } else {
                here->condition = static_cast<std::uint8_t>(here->condition - drop);
                held(globe::brick_of(where))->dirty = true;
                cracking = true;
            }
        }
    }
    if (!falling.empty()) {
        fall(falling, stepped);
        // What stood on it may hang now.
        cracking = true;
    }
    return cracking;
}

void Region::fall(const std::vector<globe::Site>& sites, Stepped& stepped) {
    std::vector<int> piece(sites.size(), -1);
    const int pieces = find_pieces(sites, piece);
    for (int p = 0; p < pieces; ++p) {
        fall_piece(sites, piece, p, stepped);
    }
}

void Region::fall_piece(const std::vector<globe::Site>& sites, const std::vector<int>& piece, int p,
                        Stepped& stepped) {
    const globe::Layers& layers = world_->layers();
    const double thickness = layers.thickness;
    std::vector<DVec3> middles;
    std::map<globe::Material, int> made_of;
    // Worn to nothing throughout, it crumbles rather than falling whole.
    bool crumbled = true;
    for (std::size_t k = 0; k < sites.size(); ++k) {
        if (piece[k] == p) {
            middles.push_back(globe::middle_of(sites[k], layers));
            ++made_of[voxel(sites[k])->solid];
            crumbled = crumbled && voxel(sites[k])->condition == 0;
        }
    }
    DVec3 centre{0.0};
    for (const DVec3& middle : middles) {
        centre += middle;
    }
    centre /= static_cast<double>(middles.size());
    double reach = 0.0;
    for (const DVec3& middle : middles) {
        reach = std::max(reach, glm::length(middle - centre));
    }
    const globe::Material material =
        std::ranges::max_element(made_of, {}, [](const auto& m) { return m.second; })->first;

    // The ground they stood in is taken out, a ball about each voxel's middle.
    const double ball = carve_ball * thickness;
    reshape(
        centre, reach + ball,
        [&](DVec3 at) {
            double nearest = 1e30;
            for (const DVec3& middle : middles) {
                nearest = std::min(nearest, glm::length(at - middle) - ball);
            }
            return nearest;
        },
        [&](globe::Voxel& voxel, double to) { globe::dig_voxel(voxel, to, thickness); });
    stepped.collapsed += static_cast<std::int64_t>(middles.size());

    const double volume = static_cast<double>(middles.size()) * thickness * thickness * thickness;
    const auto amount =
        static_cast<std::int64_t>(static_cast<double>(middles.size()) * globe::Voxel::full);
    if (!crumbled && volume >= least_boulder && volume <= most_boulder) {
        parcels_.push_back({.id = ++parcel_ids_,
                            .at = centre,
                            .material = material,
                            .amount = amount,
                            .radius = std::cbrt(3.0 * volume / (4.0 * std::numbers::pi))});
    } else {
        throw_rubble(sites, piece, p, material);
    }
}

void Region::throw_rubble(const std::vector<globe::Site>& sites, const std::vector<int>& piece,
                          int p, globe::Material material) {
    const globe::Layers& layers = world_->layers();
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::pair<DVec3, std::int64_t>> lumps;
    for (std::size_t k = 0; k < sites.size(); ++k) {
        if (piece[k] == p) {
            auto& [at, grain] = lumps[{sites[k].i, sites[k].j}];
            at = globe::middle_of(sites[k], layers);
            grain += globe::Voxel::full;
        }
    }
    for (const auto& [column, lump] : lumps) {
        if (parcels_.size() < 2048) {
            throw_grain(lump.first, {}, material, lump.second);
        }
    }
}

} // namespace hog::flow
