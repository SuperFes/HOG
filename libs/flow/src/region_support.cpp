#include <hog/flow/region.hpp>
#include <hog/flow/support.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <set>
#include <tuple>

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

} // namespace

void Region::disturb(globe::BrickSite site) {
    const bool queued = std::ranges::any_of(inspect_, [&](const Fresh& waiting) {
        return globe::brick_key(waiting.site) == globe::brick_key(site);
    });
    if (!queued) {
        inspect_.push_back({.site = site});
    }
}

void Region::inspect_support(double seconds, Stepped& stepped) {
    const std::uint32_t across = world_->store().bricks_across();
    const auto up = static_cast<std::uint32_t>(
        (world_->store().layers().count + globe::brick_side - 1) / globe::brick_side);
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
        if (!all_held) {
            if (++queued.tries < most_tries) {
                waiting.push_back(queued);
            }
            continue;
        }
        ++looked;
        if (check_support(at, seconds, stepped)) {
            waiting.push_back(queued);
        }
    }
    inspect_ = std::move(waiting);
}

bool Region::check_support(globe::BrickSite site, double seconds, Stepped& stepped) {
    constexpr int side = box_bricks * globe::brick_side;
    const globe::Layers& layers = world_->store().layers();
    const std::uint32_t i0 = (site.i - 1) * globe::brick_side;
    const std::uint32_t j0 = (site.j - 1) * globe::brick_side;
    const std::uint32_t l0 = (site.layer - 1) * globe::brick_side;
    Support support{side, side, side, layers.thickness, world_->generator().planet().gravity};
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
            if (here->condition <= drop) {
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
    const globe::Layers& layers = world_->store().layers();
    std::map<std::tuple<std::uint8_t, std::uint32_t, std::uint32_t, std::uint32_t>, std::size_t>
        index;
    for (std::size_t k = 0; k < sites.size(); ++k) {
        index.emplace(std::tuple{sites[k].face, sites[k].i, sites[k].j, sites[k].layer}, k);
    }
    // Pieces: voxels touching by a face.
    std::vector<int> piece(sites.size(), -1);
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

    const double thickness = layers.thickness;
    for (int p = 0; p < pieces; ++p) {
        std::vector<DVec3> middles;
        std::map<globe::Material, int> made_of;
        for (std::size_t k = 0; k < sites.size(); ++k) {
            if (piece[k] == p) {
                middles.push_back(globe::middle_of(sites[k], layers));
                ++made_of[voxel(sites[k])->solid];
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
        reshape(centre, reach + ball,
                [&](DVec3 at, DVec3& outward) {
                    double nearest = 1e30;
                    for (const DVec3& middle : middles) {
                        const double d = glm::length(at - middle) - ball;
                        if (d < nearest) {
                            nearest = d;
                            outward = glm::length(at - middle) > 0.0
                                          ? glm::normalize(at - middle)
                                          : glm::normalize(at);
                        }
                    }
                    return nearest;
                },
                [](globe::Voxel& voxel, double to, DVec3 outward) {
                    globe::dig_voxel(voxel, to, outward);
                });
        stepped.collapsed += static_cast<std::int64_t>(middles.size());

        const double volume = static_cast<double>(middles.size()) * thickness * thickness * thickness;
        const std::int64_t amount =
            static_cast<std::int64_t>(static_cast<double>(middles.size()) * globe::Voxel::full);
        if (volume >= least_boulder && volume <= most_boulder) {
            parcels_.push_back({.id = ++parcel_ids_,
                                .at = centre,
                                .material = material,
                                .amount = amount,
                                .radius = std::cbrt(3.0 * volume / (4.0 * std::numbers::pi))});
        } else {
            // Rubble: a lump of grain from each column the piece stood in.
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
    }
}

} // namespace hog::flow
