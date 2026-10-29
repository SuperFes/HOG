#include <hog/planet/projection.hpp>
#include <hog/terrain/river_water.hpp>
#include <hog/terrain/shore.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>

namespace hog::terrain {

ShoreFrame shore_frame(DVec3 eye, double radius) {
    const int level = planet::level_for(shore_anchoring, radius);
    const DVec3 anchor = planet::cell_center(planet::cell_at(eye, level));
    // Any way across will do at the poles, where east has none.
    DVec3 across = glm::cross(DVec3{0.0, 0.0, 1.0}, anchor);
    if (glm::length(across) < 1e-6) {
        across = glm::cross(DVec3{1.0, 0.0, 0.0}, anchor);
    }
    const DVec3 east = glm::normalize(across);
    return {.anchor = anchor, .east = east, .north = glm::cross(anchor, east)};
}

DVec3 shore_direction(const ShoreFrame& frame, double radius, DVec2 along) {
    return glm::normalize((frame.anchor * radius) + (frame.east * along.x) +
                          (frame.north * along.y));
}

DVec2 shore_along(const ShoreFrame& frame, double radius, DVec3 direction) {
    const double scale = radius / glm::dot(direction, frame.anchor);
    return DVec2{glm::dot(direction, frame.east), glm::dot(direction, frame.north)} * scale;
}

ShoreGround shore_ground(const planet::World& world, const ShoreFrame& frame, double radius,
                         double spacing) {
    const planet::Elevation& terrain = world.geology().terrain();
    ShoreGround ground;
    ground.elevations.reserve(static_cast<std::size_t>(shore_size) * shore_size);
    ground.lakes.reserve(ground.elevations.capacity());
    ground.sea.reserve(ground.elevations.capacity());
    const double first = (0.5 - (shore_size / 2.0)) * spacing;
    for (int row = 0; row < shore_size; ++row) {
        for (int column = 0; column < shore_size; ++column) {
            const planet::CellId cell = planet::cell_at(shore_direction(
                frame, radius, {first + (column * spacing), first + (row * spacing)}));
            const std::int64_t generated = terrain.elevation(cell);
            const auto water = terrain.rivers().standing(cell, generated);
            const bool lake = water && !water->sea;
            ground.elevations.push_back(
                static_cast<float>(planet::fixed_to_double(world.elevation(cell, generated))));
            ground.lakes.push_back(lake ? static_cast<float>(planet::fixed_to_double(water->level))
                                        : 0.0F);
            ground.sea.push_back(water && water->sea ? 1 : 0);
        }
    }
    return ground;
}

std::vector<float> shore_depths(const ShoreGround& ground) {
    std::vector<float> depths;
    depths.reserve(ground.elevations.size());
    for (std::size_t at = 0; at < ground.elevations.size(); ++at) {
        depths.push_back(ground.lakes[at] > 0.0F ? shore_highest
                                                 : std::max(-ground.elevations[at], shore_highest));
    }
    return depths;
}

namespace {

constexpr auto shore_texels = static_cast<std::size_t>(shore_size) * shore_size;

// Water at least this deep is taken as the open sea.
constexpr float open_sea = 20.0F; // metres

// Spreads what the sea reaches from the texels marked to every texel lower than its reach.
void spread(std::span<const float> layer, std::vector<std::uint8_t>& reached) {
    std::vector<std::size_t> reaching;
    for (std::size_t at = 0; at < reached.size(); ++at) {
        if (reached[at] != 0) {
            reaching.push_back(at);
        }
    }
    const auto size = static_cast<std::size_t>(shore_size);
    while (!reaching.empty()) {
        const std::size_t at = reaching.back();
        reaching.pop_back();
        const std::size_t column = at % size;
        const std::size_t row = at / size;
        const auto reach = [&](bool inside, std::size_t next) {
            if (inside && reached[next] == 0 && layer[next] > -shore_reach) {
                reached[next] = 1;
                reaching.push_back(next);
            }
        };
        reach(column > 0, at - 1);
        reach(column + 1 < size, at + 1);
        reach(row > 0, at - size);
        reach(row + 1 < size, at + size);
    }
}

} // namespace

namespace {

// Whether the texel at `column` and `row` of layer `layer` is open sea: on the coarsest, deep or
// water at its edge; on any finer, water where the coarser layer's sea, `coarser`, reaches.
bool open_sea_at(std::span<const float> depths, std::span<const std::uint8_t> sea,
                 std::size_t layer, std::size_t column, std::size_t row,
                 const std::vector<std::uint8_t>& coarser) {
    const auto size = static_cast<std::size_t>(shore_size);
    const std::size_t layers = depths.size() / shore_texels;
    const std::size_t at = (layer * shore_texels) + (row * size) + column;
    const float depth = depths[at];
    if (layer + 1 == layers) {
        const bool edge = row == 0 || column == 0 || row + 1 == size || column + 1 == size;
        return sea[at] != 0 && (depth >= open_sea || (edge && depth > 0.0F));
    }
    if (depth <= 0.0F) {
        return false;
    }
    // The coarser layer's texel this one's centre lies in.
    const double ratio = shore_spacing.at(layer) / shore_spacing.at(layer + 1);
    const auto coarse = [&](std::size_t texel) {
        return std::floor(((static_cast<double>(texel) + 0.5 - (shore_size / 2.0)) * ratio) +
                          (shore_size / 2.0));
    };
    const double coarse_column = coarse(column);
    const double coarse_row = coarse(row);
    if (coarse_column < 0.0 || coarse_row < 0.0 || coarse_column >= shore_size ||
        coarse_row >= shore_size) {
        return false;
    }
    const std::size_t under =
        (static_cast<std::size_t>(coarse_row) * size) + static_cast<std::size_t>(coarse_column);
    return coarser[under] != 0 && depths[((layer + 1) * shore_texels) + under] > 0.0F;
}

} // namespace

std::vector<float> keep_to_sea_reach(std::span<float> depths, std::span<const std::uint8_t> sea) {
    const std::size_t layers = depths.size() / shore_texels;
    const auto size = static_cast<std::size_t>(shore_size);
    std::vector<float> still(depths.size(), shore_highest);
    std::vector<std::uint8_t> coarser;
    for (std::size_t layer = layers; layer-- > 0;) {
        std::vector<std::uint8_t> reached(shore_texels, 0);
        for (std::size_t row = 0; row < size; ++row) {
            for (std::size_t column = 0; column < size; ++column) {
                reached[(row * size) + column] =
                    open_sea_at(depths, sea, layer, column, row, coarser) ? 1 : 0;
            }
        }
        const std::span<float> here = depths.subspan(layer * shore_texels, shore_texels);
        spread(here, reached);
        for (std::size_t at = 0; at < shore_texels; ++at) {
            if (reached[at] == 0) {
                if (here[at] > 0.0F && sea[(layer * shore_texels) + at] != 0) {
                    still[(layer * shore_texels) + at] = here[at];
                }
                here[at] = shore_highest;
            }
        }
        coarser = std::move(reached);
    }
    return still;
}

namespace {

// Of a layer's levels, `level`, and how many texels out from a lake each is kept, `out`: the level
// the texel at `column` and `row`, over `ground`, keeps from those about it `step` - 1 out: the
// highest its ground stands over, or failing one, the lowest, so the lower of two lakes keeps the
// ground between them that's lower than the higher. 0 if none is that near.
float kept_from_around(std::span<const float> level, std::span<const int> out, float ground,
                       std::size_t column, std::size_t row, int step) {
    const auto size = static_cast<std::size_t>(shore_size);
    float over = 0.0F;
    float lowest = 0.0F;
    for (std::size_t down = std::max(row, std::size_t{1}) - 1; down <= std::min(row + 1, size - 1);
         ++down) {
        for (std::size_t across = std::max(column, std::size_t{1}) - 1;
             across <= std::min(column + 1, size - 1); ++across) {
            const std::size_t around = (down * size) + across;
            if (out[around] != step - 1) {
                continue;
            }
            if (level[around] <= ground) {
                over = std::max(over, level[around]);
            }
            lowest = lowest == 0.0F ? level[around] : std::min(lowest, level[around]);
        }
    }
    return over > 0.0F ? over : lowest;
}

// Keeps each texel of a layer not yet kept, as `out` has it, the level kept_from_around finds it,
// `step` out from a lake.
void keep_lakes_out(std::span<float> level, std::span<const float> ground, std::span<int> out,
                    int step) {
    const auto size = static_cast<std::size_t>(shore_size);
    std::vector<std::size_t> reached;
    for (std::size_t row = 0; row < size; ++row) {
        for (std::size_t column = 0; column < size; ++column) {
            const std::size_t at = (row * size) + column;
            if (out[at] >= 0) {
                continue;
            }
            const float kept = kept_from_around(level, out, ground[at], column, row, step);
            if (kept > 0.0F) {
                level[at] = kept;
                reached.push_back(at);
            }
        }
    }
    for (const std::size_t at : reached) {
        out[at] = step;
    }
}

} // namespace

std::vector<float> keep_lakes(std::span<const float> elevations, std::span<const float> lakes,
                              std::span<float> still) {
    std::vector<float> levels(lakes.begin(), lakes.end());
    const std::size_t layers = levels.size() / shore_texels;
    for (std::size_t layer = 0; layer < layers; ++layer) {
        const std::span<float> level =
            std::span{levels}.subspan(layer * shore_texels, shore_texels);
        const std::span<const float> ground =
            elevations.subspan(layer * shore_texels, shore_texels);
        std::vector<int> out(shore_texels, -1);
        for (std::size_t at = 0; at < shore_texels; ++at) {
            out[at] = level[at] > 0.0F ? 0 : -1;
        }
        for (int step = 1; step <= lake_margin; ++step) {
            keep_lakes_out(level, ground, out, step);
        }
        for (std::size_t at = 0; at < shore_texels; ++at) {
            // Kept out over lower ground, the level is drawn over it in the air, so that ground is
            // told dry for the water there to be left undrawn.
            if (out[at] == 0 || (out[at] > 0 && ground[at] >= level[at])) {
                still[(layer * shore_texels) + at] =
                    std::max(level[at] - ground[at], shore_highest);
            } else if (out[at] > 0) {
                still[(layer * shore_texels) + at] = shore_highest;
            }
        }
    }
    return levels;
}

void keep_dry(std::span<const float> elevations, std::span<const float> depths,
              std::span<const float> still, std::span<float> levels) {
    for (std::size_t at = 0; at < levels.size(); ++at) {
        if (elevations[at] < 0.0F && depths[at] <= shore_highest && still[at] <= 0.0F &&
            levels[at] == 0.0F) {
            levels[at] = elevations[at] + shore_highest;
        }
    }
}

namespace {

// The step to the texel upwind of a wind blowing towards each heading.
constexpr std::array<std::array<int, 2>, fetch_headings> upwind{
    {{-1, 0}, {-1, -1}, {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}}};

// Of a layer's fetch towards a heading, where its upwind step leaves the layer from `column` and
// `row`: the next coarser layer's at the point it leaves to, `coarser`, or the furthest kept from
// the coarsest.
float fetch_beyond(std::size_t layer, int column, int row, std::span<const float> coarser) {
    if (coarser.empty()) {
        return longest_fetch;
    }
    const double ratio = shore_spacing.at(layer) / shore_spacing.at(layer + 1);
    const auto coarse = [&](int at) {
        return static_cast<int>(
            std::floor(((at + 0.5 - (shore_size / 2.0)) * ratio) + (shore_size / 2.0)));
    };
    const int coarse_column = std::clamp(coarse(column), 0, shore_size - 1);
    const int coarse_row = std::clamp(coarse(row), 0, shore_size - 1);
    return coarser[(static_cast<std::size_t>(coarse_row) * shore_size) +
                   static_cast<std::size_t>(coarse_column)];
}

} // namespace

namespace {

// Of a layer's water, `wet`, its fetch towards `heading` into `here`, running on from `coarser`.
void sweep(std::size_t layer, int heading, const std::vector<std::uint8_t>& wet,
           std::span<float> here, std::span<const float> coarser) {
    const auto size = static_cast<std::size_t>(shore_size);
    const auto [dx, dy] = upwind.at(static_cast<std::size_t>(heading));
    const auto step = static_cast<float>(shore_spacing.at(layer) * std::hypot(dx, dy));
    const auto inside = [](int at) { return at >= 0 && at < shore_size; };
    // Upwind texels first.
    for (int across = 0; across < shore_size; ++across) {
        const int row = dy > 0 ? shore_size - 1 - across : across;
        for (int down = 0; down < shore_size; ++down) {
            const int column = dx > 0 ? shore_size - 1 - down : down;
            const std::size_t at =
                (static_cast<std::size_t>(row) * size) + static_cast<std::size_t>(column);
            if (wet[at] == 0) {
                continue;
            }
            const int from_column = column + dx;
            const int from_row = row + dy;
            const float before = inside(from_column) && inside(from_row)
                                     ? here[(static_cast<std::size_t>(from_row) * size) +
                                            static_cast<std::size_t>(from_column)]
                                     : fetch_beyond(layer, from_column, from_row, coarser);
            // From the shore halfway to the texel upwind, or on from that texel.
            here[at] = std::min(before > 0.0F ? before + step : step / 2.0F, longest_fetch);
        }
    }
}

} // namespace

std::vector<float> shore_fetches(std::span<const float> depths, std::span<const float> still) {
    const std::size_t layers = depths.size() / shore_texels;
    std::vector<float> fetches(layers * fetch_headings * shore_texels, 0.0F);
    const std::span<float> all{fetches};
    const auto plane = [&](std::size_t layer, int heading) {
        return all.subspan(((layer * fetch_headings) + static_cast<std::size_t>(heading)) *
                               shore_texels,
                           shore_texels);
    };
    for (std::size_t layer = layers; layer-- > 0;) {
        std::vector<std::uint8_t> wet(shore_texels);
        for (std::size_t at = 0; at < shore_texels; ++at) {
            const std::size_t in_layer = (layer * shore_texels) + at;
            wet[at] = depths[in_layer] > 0.0F || still[in_layer] > 0.0F ? 1 : 0;
        }
        for (int heading = 0; heading < fetch_headings; ++heading) {
            sweep(layer, heading, wet, plane(layer, heading),
                  layer + 1 < layers ? plane(layer + 1, heading) : std::span<float>{});
        }
    }
    return fetches;
}

namespace {

// Squared texels from each texel of a layer to the nearest that `seeds` marks, or none past any
// distance there: Felzenszwalb and Huttenlocher's transform, by rows and then by columns.
constexpr double no_seed = 1e30;

std::vector<double> squared_distances(const std::vector<std::uint8_t>& seeds) {
    const auto size = static_cast<std::size_t>(shore_size);
    std::vector<double> found(shore_texels);
    for (std::size_t at = 0; at < shore_texels; ++at) {
        found[at] = seeds[at] != 0 ? 0.0 : no_seed;
    }
    std::vector<double> line(size);
    std::vector<std::size_t> parabolas(size);
    std::vector<double> starts(size + 1);
    // The lower envelope of the parabolas rooted at each texel of `line` that has a seed in reach.
    const auto along = [&](const auto& texel) {
        for (std::size_t q = 0; q < size; ++q) {
            line[q] = found[texel(q)];
        }
        std::size_t k = 0;
        bool any = false;
        const auto meet = [&](std::size_t q, std::size_t v) {
            const auto qd = static_cast<double>(q);
            const auto vd = static_cast<double>(v);
            return ((line[q] + (qd * qd)) - (line[v] + (vd * vd))) / (2.0 * (qd - vd));
        };
        for (std::size_t q = 0; q < size; ++q) {
            if (line[q] >= no_seed) {
                continue;
            }
            if (!any) {
                any = true;
                parabolas[0] = q;
                starts[0] = -std::numeric_limits<double>::infinity();
                starts[1] = std::numeric_limits<double>::infinity();
                continue;
            }
            double s = meet(q, parabolas[k]);
            while (s <= starts[k]) {
                --k;
                s = meet(q, parabolas[k]);
            }
            ++k;
            parabolas[k] = q;
            starts[k] = s;
            starts[k + 1] = std::numeric_limits<double>::infinity();
        }
        if (!any) {
            return;
        }
        k = 0;
        for (std::size_t q = 0; q < size; ++q) {
            while (starts[k + 1] < static_cast<double>(q)) {
                ++k;
            }
            const double off = static_cast<double>(q) - static_cast<double>(parabolas[k]);
            found[texel(q)] = (off * off) + line[parabolas[k]];
        }
    };
    for (std::size_t row = 0; row < size; ++row) {
        along([&](std::size_t column) { return (row * size) + column; });
    }
    for (std::size_t column = 0; column < size; ++column) {
        along([&](std::size_t row) { return (row * size) + column; });
    }
    return found;
}

// Of a layer's plane, `plane`, its value at the centre of a texel of the next finer layer, blended
// from the four texels round it.
float finer_centre(std::span<const float> plane, std::size_t layer_finer, int column, int row) {
    const double ratio = shore_spacing.at(layer_finer) / shore_spacing.at(layer_finer + 1);
    const auto coordinate = [&](int texel) {
        return std::clamp(((static_cast<double>(texel) + 0.5 - (shore_size / 2.0)) * ratio) +
                              (shore_size / 2.0) - 0.5,
                          0.0, static_cast<double>(shore_size - 1));
    };
    const double x = coordinate(column);
    const double y = coordinate(row);
    const auto x0 = static_cast<std::size_t>(std::min(static_cast<int>(x), shore_size - 2));
    const auto y0 = static_cast<std::size_t>(std::min(static_cast<int>(y), shore_size - 2));
    const double fx = x - static_cast<double>(x0);
    const double fy = y - static_cast<double>(y0);
    const auto size = static_cast<std::size_t>(shore_size);
    const auto at = [&](std::size_t r, std::size_t c) {
        return static_cast<double>(plane[(r * size) + c]);
    };
    const double low = std::lerp(at(y0, x0), at(y0, x0 + 1), fx);
    const double high = std::lerp(at(y0 + 1, x0), at(y0 + 1, x0 + 1), fx);
    return static_cast<float>(std::lerp(low, high, fy));
}

} // namespace

namespace {

// Of a layer's texels, `wet` where there's water, how far each lies offshore into `metres`,
// negative inland, as shore_offshore has it; from `coarser`, the next coarser layer's, if any,
// where the nearest waterline may lie beyond it.
void offshore_layer(std::size_t layer, const std::vector<std::uint8_t>& wet,
                    std::span<float> metres, std::span<const float> coarser) {
    std::vector<std::uint8_t> dry(shore_texels);
    for (std::size_t at = 0; at < shore_texels; ++at) {
        dry[at] = wet[at] != 0 ? 0 : 1;
    }
    const std::vector<double> to_dry = squared_distances(dry);
    const std::vector<double> to_wet = squared_distances(wet);
    const double spacing = shore_spacing.at(layer);
    for (int row = 0; row < shore_size; ++row) {
        for (int column = 0; column < shore_size; ++column) {
            const std::size_t at =
                (static_cast<std::size_t>(row) * static_cast<std::size_t>(shore_size)) +
                static_cast<std::size_t>(column);
            const bool in_water = wet[at] != 0;
            const double squared = in_water ? to_dry[at] : to_wet[at];
            // The waterline lies halfway between the texels either side of it.
            const double found = squared < no_seed ? (std::sqrt(squared) - 0.5) * spacing
                                                   : std::numeric_limits<double>::infinity();
            // Any waterline beyond the layer lies at least as far as its edge.
            const double edge =
                (std::min({column, row, shore_size - 1 - column, shore_size - 1 - row}) + 0.5) *
                spacing;
            double far = found;
            if (found > edge && !coarser.empty()) {
                const auto coarse = static_cast<double>(finer_centre(coarser, layer, column, row));
                far = std::clamp(in_water ? coarse : -coarse, edge, found);
            }
            far = std::min(far, static_cast<double>(farthest_offshore));
            metres[at] = static_cast<float>(in_water ? far : -far);
        }
    }
}

// How fast a layer's `metres` grow east and north, per metre, into `east` and `north`.
void offshore_slopes(std::size_t layer, std::span<const float> metres, std::span<float> east,
                     std::span<float> north) {
    const double spacing = shore_spacing.at(layer);
    const auto value = [&](int row, int column) {
        return static_cast<double>(
            metres[(static_cast<std::size_t>(row) * static_cast<std::size_t>(shore_size)) +
                   static_cast<std::size_t>(column)]);
    };
    for (int row = 0; row < shore_size; ++row) {
        for (int column = 0; column < shore_size; ++column) {
            const std::size_t at =
                (static_cast<std::size_t>(row) * static_cast<std::size_t>(shore_size)) +
                static_cast<std::size_t>(column);
            const int left = std::max(column - 1, 0);
            const int right = std::min(column + 1, shore_size - 1);
            const int below = std::max(row - 1, 0);
            const int above = std::min(row + 1, shore_size - 1);
            east[at] = static_cast<float>((value(row, right) - value(row, left)) /
                                          (static_cast<double>(right - left) * spacing));
            north[at] = static_cast<float>((value(above, column) - value(below, column)) /
                                           (static_cast<double>(above - below) * spacing));
        }
    }
}

} // namespace

std::vector<float> shore_offshore(std::span<const float> depths, std::span<const float> still) {
    const std::size_t layers = depths.size() / shore_texels;
    std::vector<float> offshore(layers * offshore_planes * shore_texels, 0.0F);
    const std::span<float> all{offshore};
    const auto plane = [&](std::size_t layer, int which) {
        return all.subspan(((layer * offshore_planes) + static_cast<std::size_t>(which)) *
                               shore_texels,
                           shore_texels);
    };
    for (std::size_t layer = layers; layer-- > 0;) {
        std::vector<std::uint8_t> wet(shore_texels);
        for (std::size_t at = 0; at < shore_texels; ++at) {
            const std::size_t in_layer = (layer * shore_texels) + at;
            wet[at] = depths[in_layer] > 0.0F || still[in_layer] > 0.0F ? 1 : 0;
        }
        offshore_layer(layer, wet, plane(layer, 0),
                       layer + 1 < layers ? plane(layer + 1, 0) : std::span<float>{});
        offshore_slopes(layer, plane(layer, 0), plane(layer, 1), plane(layer, 2));
    }
    return offshore;
}

void ShoreStream::follow(DVec3 eye) {
    wanted_ = shore_frame(eye, radius_);
    if (!working_ && wanted_ != started_) {
        start(*wanted_);
    }
}

std::vector<float> lake_levels(const ShoreDepths& shore) {
    std::vector<float> levels;
    const std::size_t texels = std::min(shore.levels.size(), shore_texels);
    for (std::size_t at = 0; at < texels; ++at) {
        if (shore.levels[at] > 0.0F && shore.still[at] > 0.0F) {
            levels.push_back(shore.levels[at]);
        }
    }
    std::ranges::sort(levels);
    const auto [last, end] = std::ranges::unique(levels);
    levels.erase(last, end);
    return levels;
}

double water_below(std::span<const float> levels, double height) {
    const auto above = std::ranges::upper_bound(
        levels, height, std::less{}, [](float level) { return static_cast<double>(level); });
    return above == levels.begin() ? 0.0 : static_cast<double>(*std::prev(above));
}

double plume_length(double flow) {
    return plume_reach * std::sqrt(std::max(flow, 0.0));
}

double plume_at(const Plume& plume, DVec2 at) {
    const DVec2 from = at - plume.origin;
    const double out = glm::dot(from, plume.heading);
    const double across = std::abs((from.x * plume.heading.y) - (from.y * plume.heading.x));
    const double half = plume.half + (std::max(out, 0.0) * plume_spread);
    // It fills the mouth behind the shore, and mixes away out to sea.
    const double along = out < 0.0 ? glm::smoothstep(-2.0 * plume.half, 0.0, out)
                                   : std::exp(-out / std::max(plume.length, 1e-6));
    return along * (1.0 - glm::smoothstep(0.6, 1.0, across / std::max(half, 1e-6)));
}

std::vector<float> shore_plumes(const planet::World& world, const ShoreFrame& frame,
                                double radius) {
    const planet::Elevation& terrain = world.geology().terrain();
    const planet::Meander& meander = terrain.rivers().meander();
    // Out to the coarsest layer's corners.
    const double widest = shore_size * shore_spacing.back();
    std::vector<Plume> plumes;
    for (const planet::Reach& reach : terrain.rivers().reaches()) {
        if (!reach.mouth || glm::length(reach.to - frame.anchor) * radius > widest) {
            continue;
        }
        const double shore = river_shore(terrain, reach, radius);
        const auto along = [&](double t) {
            return shore_along(frame, radius, meander.unshifted(planet::course_at(reach, t)));
        };
        const DVec2 ahead = along(std::min(shore + 0.05, 1.0)) - along(std::max(shore - 0.05, 0.0));
        if (glm::length(ahead) <= 0.0) {
            continue;
        }
        plumes.push_back({.origin = along(shore),
                          .heading = glm::normalize(ahead),
                          .half = planet::width_at(reach, shore) / 2.0,
                          .length = plume_length(reach.flow)});
    }
    constexpr auto texels = static_cast<std::size_t>(shore_size) * shore_size;
    std::vector<float> found(shore_spacing.size() * texels, 0.0F);
    for (std::size_t layer = 0; layer < shore_spacing.size(); ++layer) {
        const double spacing = shore_spacing.at(layer);
        const double first = (0.5 - (shore_size / 2.0)) * spacing;
        const auto texel = [&](double metres) {
            return std::clamp(static_cast<int>(std::floor((metres - first) / spacing)), 0,
                              shore_size - 1);
        };
        for (const Plume& plume : plumes) {
            // It's all but mixed away three lengths out.
            const double out = 3.0 * plume.length;
            const double extent = out + plume.half + (out * plume_spread);
            for (int row = texel(plume.origin.y - extent); row <= texel(plume.origin.y + extent);
                 ++row) {
                for (int column = texel(plume.origin.x - extent);
                     column <= texel(plume.origin.x + extent); ++column) {
                    float& mixed =
                        found[(layer * texels) + (static_cast<std::size_t>(row) * shore_size) +
                              static_cast<std::size_t>(column)];
                    mixed = std::max(
                        mixed, static_cast<float>(plume_at(
                                   plume, {first + (column * spacing), first + (row * spacing)})));
                }
            }
        }
    }
    return found;
}

ShoreDepths work_out_shore(const planet::World& world, const ShoreFrame& frame, double radius) {
    ShoreDepths found{.frame = frame,
                      .depths = {},
                      .still = {},
                      .levels = {},
                      .fetches = {},
                      .offshore = {},
                      .plumes = {}};
    std::vector<float> elevations;
    std::vector<float> lakes;
    std::vector<std::uint8_t> sea;
    for (const double spacing : shore_spacing) {
        const ShoreGround ground = shore_ground(world, frame, radius, spacing);
        const std::vector<float> layer = shore_depths(ground);
        found.depths.insert(found.depths.end(), layer.begin(), layer.end());
        elevations.insert(elevations.end(), ground.elevations.begin(), ground.elevations.end());
        lakes.insert(lakes.end(), ground.lakes.begin(), ground.lakes.end());
        sea.insert(sea.end(), ground.sea.begin(), ground.sea.end());
    }
    found.still = keep_to_sea_reach(found.depths, sea);
    found.levels = keep_lakes(elevations, lakes, found.still);
    keep_dry(elevations, found.depths, found.still, found.levels);
    found.fetches = shore_fetches(found.depths, found.still);
    found.offshore = shore_offshore(found.depths, found.still);
    found.plumes = shore_plumes(world, frame, radius);
    return found;
}

void ShoreStream::start(const ShoreFrame& frame) {
    started_ = frame;
    working_ = std::make_unique<jobs::Async<ShoreDepths>>(
        *jobs_, [world = world_, frame, radius = radius_] {
            return work_out_shore(*world, frame, radius);
        });
}

std::optional<ShoreDepths> ShoreStream::collect() {
    if (!working_ || !working_->done()) {
        return std::nullopt;
    }
    ShoreDepths found = working_->take();
    working_.reset();
    if (wanted_ && wanted_ != started_) {
        start(*wanted_);
    }
    return found;
}

std::optional<ShoreDepths> ShoreStream::finish() {
    std::optional<ShoreDepths> last;
    while (working_) {
        last = working_->take();
        working_.reset();
        if (wanted_ && wanted_ != started_) {
            start(*wanted_);
        }
    }
    return last;
}

} // namespace hog::terrain
