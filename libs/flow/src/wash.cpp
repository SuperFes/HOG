#include <hog/flow/wash.hpp>

#include <algorithm>
#include <cmath>
#include <ranges>

namespace hog::flow {
namespace {

using globe::Material;

constexpr std::size_t fines = 0;
constexpr std::size_t sand = 1;
constexpr std::size_t stones = 2;
constexpr std::array<Material, 3> dropped_as{Material::Sediment, Material::Sand, Material::Gravel};

std::size_t kind_of(Material material) {
    switch (material) {
    case Material::Sand:
    case Material::VolcanicSand:
    case Material::ShellSand:
        return sand;
    case Material::Gravel:
    case Material::Shingle:
        return stones;
    default:
        return fines;
    }
}

// How readily running water carries a material beside topsoil: the less drag it withstands, the
// more.
double ease_of(Material material) {
    const double withstands = globe::properties(material).critical_shear;
    if (withstands <= 0.0) {
        return 0.0;
    }
    return std::min(10.0, globe::properties(Material::Soil).critical_shear / withstands);
}

} // namespace

std::vector<std::size_t> Wash::downhill_last() {
    const std::size_t count = nodes_.size();
    std::vector<std::vector<std::size_t>> upstream(count);
    for (std::size_t n = 0; n < count; ++n) {
        if (!nodes_[n].still && nodes_[n].drains != none) {
            upstream[static_cast<std::size_t>(nodes_[n].drains)].push_back(n);
        }
    }
    std::vector<std::size_t> order;
    order.reserve(count);
    std::vector<bool> placed(count, false);
    const auto gather_from = [&](std::size_t root) {
        placed[root] = true;
        std::size_t k = order.size();
        order.push_back(root);
        for (; k < order.size(); ++k) {
            for (const std::size_t up : upstream[order[k]]) {
                if (!placed[up]) {
                    placed[up] = true;
                    order.push_back(up);
                }
            }
        }
    };
    for (std::size_t n = 0; n < count; ++n) {
        if (nodes_[n].still || nodes_[n].drains == none) {
            gather_from(n);
        }
    }
    for (std::size_t n = 0; n < count; ++n) {
        if (!placed[n]) {
            nodes_[n].still = true;
            gather_from(n);
        }
    }
    return order;
}

void Wash::run(double years, int passes) {
    const std::vector<std::size_t> order = downhill_last();
    std::vector<double> gathered(nodes_.size(), 0.0);
    for (const std::size_t n : std::views::reverse(order)) {
        Node& node = nodes_[n];
        if (node.gathers) {
            node.discharge = node.runs_off + gathered[n];
        }
        if (!node.still && node.drains != none) {
            gathered[static_cast<std::size_t>(node.drains)] += node.discharge;
        }
    }
    for (int k = 0; k < passes; ++k) {
        pass(order, years / static_cast<double>(passes));
    }
}

void Wash::pass(std::span<const std::size_t> order, double years) {
    const double area = law_.cell * law_.cell;
    std::vector<Load> loads(nodes_.size(), Load{});
    for (const std::size_t n : std::views::reverse(order)) {
        Load& load = loads[n];
        if (nodes_[n].still) {
            for (std::size_t kind = 0; kind < load.size(); ++kind) {
                drop(n, kind, load.at(kind));
            }
            continue;
        }
        if (nodes_[n].drains == none) {
            continue;
        }
        const auto to = static_cast<std::size_t>(nodes_[n].drains);
        const double fall = std::max(0.0, nodes_[n].ground - nodes_[to].ground);
        const double can = law_.capacity *
                           std::pow(std::max(nodes_[n].discharge, 0.0), law_.discharge_power) *
                           std::pow(fall / law_.cell, law_.slope_power) * years;
        const double share = carry(n, can, load);
        if (share > 0.0 && can > 0.0) {
            // Never deeper than halfway to where it drains, so it doesn't dig itself a pit.
            static_cast<void>(take(n, share, can, 0.5 * fall * area, load));
        }
        for (std::size_t kind = 0; kind < load.size(); ++kind) {
            loads[to].at(kind) += load.at(kind);
        }
    }
}

double Wash::carry(std::size_t node, double can, Load& load) {
    // The fines take up the water's capacity first; what's past it drops.
    double share = 1.0;
    for (std::size_t kind = 0; kind < load.size(); ++kind) {
        const double room = can * ease_of(dropped_as.at(kind));
        const double carried = room > 0.0 ? std::min(load.at(kind), share * room) : 0.0;
        if (room > 0.0) {
            share -= carried / room;
        }
        drop(node, kind, load.at(kind) - carried);
        load.at(kind) = carried;
    }
    return share;
}

void Wash::drop(std::size_t node, std::size_t kind, double volume) {
    if (volume <= 0.0) {
        return;
    }
    Node& here = nodes_[node];
    const double depth = volume / (law_.cell * law_.cell);
    const Material material = dropped_as.at(kind);
    if (!here.loose.empty() && here.loose.back().laid && here.loose.back().material == material) {
        here.loose.back().depth += depth;
    } else {
        here.loose.push_back({.material = material, .depth = depth, .laid = true});
    }
    here.ground += depth;
}

double Wash::take(std::size_t node, double share, double can, double most, Load& load) {
    Node& here = nodes_[node];
    const double area = law_.cell * law_.cell;
    double took = 0.0;
    while (share > 0.0 && took < most && !here.loose.empty()) {
        Piece& top = here.loose.back();
        const double ease = ease_of(top.material);
        if (ease <= 0.0) {
            break;
        }
        const double volume = std::min({top.depth * area, share * can * ease, most - took});
        top.depth -= volume / area;
        share -= volume / (can * ease);
        took += volume;
        if (!top.laid) {
            taken_[node] += volume / area;
        }
        load.at(kind_of(top.material)) += volume;
        if (top.depth > 1e-9) {
            break;
        }
        here.loose.pop_back();
    }
    here.ground -= took / area;
    return took;
}

} // namespace hog::flow
