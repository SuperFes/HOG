#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>
#include <hog/flow/lava.hpp>
#include <hog/flow/region.hpp>
#include <hog/globe/planet.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

namespace hog::flow {
namespace {

using globe::BrickSite;
using globe::Material;
using globe::Site;
using globe::Voxel;

constexpr std::int64_t full = Voxel::full;
// A head's difference is closed by this share a step across each of a section's sides, so one
// with all four sides lower gives no more than half its excess at once. Differences under it
// stay: a few units a column, a tenth of a millimetre.
constexpr std::int64_t closing = 8;

// Air the lava gives its heat to, kelvin.
constexpr double ambient = 288.0;

// Rock that flows: any liquid that isn't water.
bool molten(Material liquid) noexcept {
    return liquid != Material::Air && liquid != Material::Water;
}

constexpr std::array<std::pair<int, int>, 4> sides{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};

std::uint64_t column_key(std::uint8_t face, std::uint32_t i, std::uint32_t j) noexcept {
    return (std::uint64_t{face} << 42U) | (std::uint64_t{i} << 21U) | j;
}

std::uint64_t stack_key(BrickSite site) noexcept {
    return column_key(site.face, site.i, site.j);
}

// Open to liquid: not shaped matter, nor a voxel grain fills.
bool open(const Voxel& voxel) noexcept {
    return voxel.solid == Material::Air && voxel.grain_amount < full;
}

// What ground a closed voxel is: its shaped matter, or the grain filling it.
Material ground_of(const Voxel& voxel) noexcept {
    return voxel.solid != Material::Air ? voxel.solid : voxel.grain;
}

std::int64_t head(int bottom, std::int64_t amount) noexcept {
    return (std::int64_t{bottom} * full) + amount;
}

std::int64_t capacity(int bottom, int top) noexcept {
    return std::int64_t{top - bottom} * full;
}

// What a ground's pores can hold, and what they keep against gravity: half of it, as soil
// drains to about half its porosity and holds the rest.
std::int64_t pores(const Voxel& voxel) noexcept {
    return globe::amount_of(globe::properties(ground_of(voxel)).porosity);
}

std::int64_t kept(const Voxel& voxel) noexcept {
    return pores(voxel) / 2;
}

// The grain left in a voxel: what it holds, or none of any kind.
void set_grain(Voxel& voxel, std::int64_t amount) noexcept {
    voxel.grain_amount = static_cast<std::uint16_t>(amount);
    if (amount == 0) {
        voxel.grain = Material::Air;
    }
}

} // namespace

Region::Region(globe::World& world, bool hold_sea)
    : world_{&world}, hold_sea_{hold_sea},
      liquid_{world.generator().planet().volatiles == globe::Volatile::Water ? Material::Water
                                                                             : Material::Air} {}

void Region::load(BrickSite site) {
    const auto [found, added] = held_.try_emplace(globe::brick_key(site));
    if (!added) {
        return;
    }
    found->second.voxels = world_->brick(site);
    fresh_.push_back({.site = site});
    std::vector<std::uint32_t>& stack = stacks_[stack_key(site)];
    stack.insert(std::ranges::lower_bound(stack, site.layer), site.layer);
}

void Region::unload(BrickSite site) {
    if (held_.erase(globe::brick_key(site)) == 0) {
        return;
    }
    for (std::uint32_t b = 0; b < globe::brick_side; ++b) {
        for (std::uint32_t a = 0; a < globe::brick_side; ++a) {
            const auto found = loads_.find(column_key(site.face, (site.i * globe::brick_side) + a,
                                                      (site.j * globe::brick_side) + b));
            if (found != loads_.end() && found->second.layer / globe::brick_side == site.layer) {
                loads_.erase(found);
            }
        }
    }
    const auto stack = stacks_.find(stack_key(site));
    std::erase(stack->second, site.layer);
    if (stack->second.empty()) {
        stacks_.erase(stack);
    }
}

bool Region::holds(BrickSite site) const {
    return held_.contains(globe::brick_key(site));
}

Region::Held* Region::held(BrickSite site) {
    const auto found = held_.find(globe::brick_key(site));
    return found == held_.end() ? nullptr : &found->second;
}

const Region::Held* Region::held(BrickSite site) const {
    const auto found = held_.find(globe::brick_key(site));
    return found == held_.end() ? nullptr : &found->second;
}

Voxel* Region::voxel(Site site) {
    Held* brick = held(globe::brick_of(site));
    return brick == nullptr ? nullptr
                            : &brick->voxels[static_cast<std::size_t>(globe::index_in_brick(site))];
}

std::optional<Site> Region::beside(Site site, int di, int dj) const {
    const std::int64_t side = std::int64_t{1} << world_->layers().level;
    const std::int64_t i = std::int64_t{site.i} + di;
    const std::int64_t j = std::int64_t{site.j} + dj;
    // The cube's edges count as the edge of what's held until the flow crosses faces.
    if (i < 0 || j < 0 || i >= side || j >= side) {
        return std::nullopt;
    }
    return Site{.face = site.face,
                .i = static_cast<std::uint32_t>(i),
                .j = static_cast<std::uint32_t>(j),
                .layer = site.layer};
}

Voxel Region::at(Site site) const {
    const Held* brick = held(globe::brick_of(site));
    HOG_ASSERT(brick != nullptr, "the voxel's brick isn't held");
    return brick->voxels[static_cast<std::size_t>(globe::index_in_brick(site))];
}

void Region::pour(Site site, std::uint16_t amount) {
    Voxel* at = voxel(site);
    HOG_ASSERT(at != nullptr, "the voxel's brick isn't held");
    at->liquid = amount > 0 ? liquid_ : Material::Air;
    at->liquid_amount = amount;
    held(globe::brick_of(site))->dirty = true;
}

void Region::pour_lava(Site site, Material rock, std::uint16_t amount, double kelvin) {
    Voxel* at = voxel(site);
    HOG_ASSERT(at != nullptr, "the voxel's brick isn't held");
    at->liquid = amount > 0 ? rock : Material::Air;
    at->liquid_amount = amount;
    at->temperature = globe::temperature_in_eighths(kelvin);
    held(globe::brick_of(site))->dirty = true;
}

std::int64_t Region::add_lava(Site site, Material rock, std::int64_t units, double kelvin) {
    Voxel* at = voxel(site);
    HOG_ASSERT(at != nullptr, "the voxel's brick isn't held");
    if (!open(*at) || (at->liquid_amount > 0 && at->liquid != rock)) {
        return 0;
    }
    const std::int64_t room = full - at->grain_amount - at->liquid_amount;
    const std::int64_t added = std::clamp(units, std::int64_t{0}, room);
    if (added == 0) {
        return 0;
    }
    // Mixed by the heat each part brings.
    const double heat = (globe::kelvin(at->temperature) * at->liquid_amount) +
                        (kelvin * static_cast<double>(added));
    at->liquid = rock;
    at->liquid_amount = static_cast<std::uint16_t>(at->liquid_amount + added);
    at->temperature = globe::temperature_in_eighths(heat / at->liquid_amount);
    held(globe::brick_of(site))->dirty = true;
    return added;
}

bool Region::red_hot(const Voxel& voxel) const {
    // Not where the whole planet is hotter than that: its rock has no cooling to do.
    return voxel.solid != Material::Air &&
           globe::kelvin(voxel.temperature) > red_hot_kelvin &&
           world_->generator().planet().equator_temperature < red_hot_kelvin - 100.0;
}

bool Region::hot(BrickSite site) const {
    const Held* brick = held(site);
    return brick != nullptr &&
           std::ranges::any_of(brick->voxels, [this](const Voxel& voxel) {
               return (voxel.liquid_amount > 0 && molten(voxel.liquid)) || red_hot(voxel);
           });
}

void Region::pile(Site site, Material material, std::uint16_t amount) {
    Voxel* at = voxel(site);
    HOG_ASSERT(at != nullptr, "the voxel's brick isn't held");
    at->grain = material;
    set_grain(*at, amount);
    held(globe::brick_of(site))->dirty = true;
}

void Region::suspend(Site site, Material material, std::int64_t amount) {
    Load& load = loads_[column_key(site.face, site.i, site.j)];
    if (amount > load.amount) {
        load.material = material;
    }
    load.amount += amount;
    load.layer = site.layer;
}

std::int64_t Region::liquid() const {
    std::int64_t total = 0;
    for (const auto& [key, brick] : held_) {
        for (const Voxel& voxel : brick.voxels) {
            total += voxel.liquid_amount;
        }
    }
    return total;
}

std::int64_t Region::grain() const {
    std::int64_t total = 0;
    for (const auto& [key, brick] : held_) {
        for (const Voxel& voxel : brick.voxels) {
            total += voxel.grain_amount;
        }
    }
    for (const auto& [key, load] : loads_) {
        total += load.amount;
    }
    for (const Parcel& parcel : parcels_) {
        total += parcel.amount;
    }
    return total;
}

Region::Carried Region::carried(std::uint8_t face, std::uint32_t i, std::uint32_t j) const {
    const auto found = loads_.find(column_key(face, i, j));
    return found == loads_.end()
               ? Carried{}
               : Carried{.material = found->second.material, .amount = found->second.amount};
}

Region::Flux Region::flux(std::uint8_t face, std::uint32_t i, std::uint32_t j) const {
    const auto found = flux_.find(column_key(face, i, j));
    return found == flux_.end() ? Flux{} : found->second;
}

std::vector<Region::Column> Region::columns() const {
    std::vector<std::uint64_t> stacks;
    stacks.reserve(stacks_.size());
    for (const auto& [key, layers] : stacks_) {
        stacks.push_back(key);
    }
    std::ranges::sort(stacks);
    std::vector<Column> columns;
    columns.reserve(stacks.size() * globe::brick_side * globe::brick_side);
    for (const std::uint64_t key : stacks) {
        const auto face = static_cast<std::uint8_t>(key >> 42U);
        const auto bi = static_cast<std::uint32_t>((key >> 21U) & ((1U << 21U) - 1));
        const auto bj = static_cast<std::uint32_t>(key & ((1U << 21U) - 1));
        for (std::uint32_t b = 0; b < globe::brick_side; ++b) {
            for (std::uint32_t a = 0; a < globe::brick_side; ++a) {
                columns.push_back({.face = face,
                                   .i = (bi * globe::brick_side) + a,
                                   .j = (bj * globe::brick_side) + b});
            }
        }
    }
    return columns;
}

template <typename Each, typename Gap>
void Region::walk(const Column& column, const Each& each, const Gap& gap) {
    const auto stack = stacks_.find(
        column_key(column.face, column.i / globe::brick_side, column.j / globe::brick_side));
    HOG_ASSERT(stack != stacks_.end(), "a column of no brick");
    std::uint32_t last = 0;
    bool first = true;
    for (const std::uint32_t brick_layer : stack->second) {
        // Layers not held between two bricks are walls, like the floor and the ceiling.
        if (!first && brick_layer != last + 1) {
            gap();
        }
        first = false;
        last = brick_layer;
        Held& brick = *held({.face = column.face,
                             .i = column.i / globe::brick_side,
                             .j = column.j / globe::brick_side,
                             .layer = brick_layer});
        for (int k = 0; k < globe::brick_side; ++k) {
            const Site site{.face = column.face,
                            .i = column.i,
                            .j = column.j,
                            .layer =
                                (brick_layer * globe::brick_side) + static_cast<std::uint32_t>(k)};
            Voxel& voxel = brick.voxels[static_cast<std::size_t>(globe::index_in_brick(site))];
            if (open(voxel)) {
                each(site, brick, voxel);
            } else {
                gap();
            }
        }
    }
    gap();
}

template <typename Each>
void Region::walk_all(const Column& column, const Each& each) {
    const auto stack = stacks_.find(
        column_key(column.face, column.i / globe::brick_side, column.j / globe::brick_side));
    HOG_ASSERT(stack != stacks_.end(), "a column of no brick");
    std::uint32_t last = 0;
    bool first = true;
    for (const std::uint32_t brick_layer : stack->second) {
        bool joined = !first && brick_layer == last + 1;
        first = false;
        last = brick_layer;
        Held& brick = *held({.face = column.face,
                             .i = column.i / globe::brick_side,
                             .j = column.j / globe::brick_side,
                             .layer = brick_layer});
        for (int k = 0; k < globe::brick_side; ++k) {
            const Site site{.face = column.face,
                            .i = column.i,
                            .j = column.j,
                            .layer =
                                (brick_layer * globe::brick_side) + static_cast<std::uint32_t>(k)};
            each(site, brick, brick.voxels[static_cast<std::size_t>(globe::index_in_brick(site))],
                 joined);
            joined = true;
        }
    }
}

bool Region::open_to_sea(Site site) const {
    const std::optional<double> sea = world_->generator().sea_level();
    return sea && world_->layers().middle(static_cast<int>(site.layer)) < *sea;
}

bool Region::at_edge(Site site) {
    // Beside, or over it: under the sea's level the sea stands above what's held too.
    const Site above{.face = site.face, .i = site.i, .j = site.j, .layer = site.layer + 1};
    return voxel(above) == nullptr || std::ranges::any_of(sides, [&](const auto& step) {
               const std::optional<Site> next = beside(site, step.first, step.second);
               return !next || voxel(*next) == nullptr;
           });
}

void Region::hold_sea(const std::vector<Column>& columns, Stepped& stepped) {
    if (!hold_sea_ || !world_->generator().sea_level()) {
        return;
    }
    for (const Column& column : columns) {
        walk(
            column,
            [&](Site site, Held& brick, Voxel& here) {
                // At the edge of what's held, the sea beyond stands at its level.
                const std::int64_t room = full - here.grain_amount;
                if (std::int64_t{here.liquid_amount} >= room || !open_to_sea(site) ||
                    !at_edge(site)) {
                    return;
                }
                stepped.from_sea += room - here.liquid_amount;
                here.liquid = liquid_;
                here.liquid_amount = static_cast<std::uint16_t>(room);
                brick.dirty = true;
            },
            [] {});
    }
}

void Region::gather(std::vector<Column>& columns) {
    sections_.clear();
    column_at_.clear();
    for (std::size_t c = 0; c < columns.size(); ++c) {
        Column& column = columns[c];
        column.first = sections_.size();
        std::optional<Section> run;
        walk(
            column,
            [&](Site site, Held& /*brick*/, const Voxel& voxel) {
                const auto layer = static_cast<int>(site.layer);
                if (!run) {
                    run = Section{.bottom = layer, .top = layer};
                }
                run->top = layer + 1;
                run->amount += voxel.liquid_amount;
                run->bed += voxel.grain_amount;
                if (voxel.liquid_amount > 0 && run->liquid == Material::Air) {
                    run->liquid = voxel.liquid;
                }
                if (molten(voxel.liquid)) {
                    run->heat += std::int64_t{voxel.liquid_amount} * voxel.temperature;
                }
            },
            [&] {
                if (run) {
                    sections_.push_back(*run);
                    run.reset();
                }
            });
        column.count = sections_.size() - column.first;
        column_at_[column_key(column.face, column.i, column.j)] = c;
    }
    take_up_loads(columns);
}

void Region::take_up_loads(const std::vector<Column>& columns) {
    for (const Column& column : columns) {
        const auto found = loads_.find(column_key(column.face, column.i, column.j));
        if (found == loads_.end()) {
            continue;
        }
        // The section it was carried in, else the lowest with liquid, else the lowest, where
        // it lies down dry.
        std::optional<std::size_t> into;
        for (std::size_t s = column.first; s < column.first + column.count; ++s) {
            const Section& section = sections_[s];
            if (std::cmp_less_equal(section.bottom, found->second.layer) &&
                std::cmp_less(found->second.layer, section.top)) {
                into = s;
                break;
            }
            if (!into || (section.amount > 0 && sections_[*into].amount == 0)) {
                into = s;
            }
        }
        if (!into) {
            continue;
        }
        Section& section = sections_[*into];
        if (found->second.amount > section.load) {
            section.carried = found->second.material;
        }
        section.load += found->second.amount;
        loads_.erase(found);
    }
}

void Region::keep_loads(const std::vector<Column>& columns) {
    for (const Column& column : columns) {
        for (std::size_t s = column.first; s < column.first + column.count; ++s) {
            const Section& section = sections_[s];
            if (section.load <= 0) {
                continue;
            }
            Load& load = loads_[column_key(column.face, column.i, column.j)];
            if (section.load > load.amount) {
                load.material = section.carried;
                load.layer = static_cast<std::uint32_t>(section.bottom);
            }
            load.amount += section.load;
        }
    }
}

std::vector<Region::Meeting> Region::meetings(const std::vector<Column>& columns) const {
    std::vector<Meeting> meetings;
    for (std::size_t c = 0; c < columns.size(); ++c) {
        const Column& column = columns[c];
        for (const bool along_i : {true, false}) {
            const std::optional<Site> next =
                beside({.face = column.face, .i = column.i, .j = column.j}, along_i ? 1 : 0,
                       along_i ? 0 : 1);
            if (!next) {
                continue;
            }
            const auto found = column_at_.find(column_key(next->face, next->i, next->j));
            if (found == column_at_.end()) {
                continue;
            }
            meet(c, column, columns[found->second], along_i, meetings);
        }
    }
    return meetings;
}

void Region::meet(std::size_t c, const Column& here, const Column& there, bool along_i,
                  std::vector<Meeting>& meetings) const {
    for (std::size_t s = here.first; s < here.first + here.count; ++s) {
        for (std::size_t t = there.first; t < there.first + there.count; ++t) {
            const int window = std::max(sections_[s].bottom, sections_[t].bottom);
            if (window < std::min(sections_[s].top, sections_[t].top)) {
                meetings.push_back(
                    {.here = s, .there = t, .window = window, .column = c, .along_i = along_i});
            }
        }
    }
}

std::vector<std::int64_t> Region::heads(const std::vector<Meeting>& meetings) const {
    std::vector<std::int64_t> heads(sections_.size());
    std::vector<bool> is_full(sections_.size());
    for (std::size_t k = 0; k < sections_.size(); ++k) {
        const Section& section = sections_[k];
        heads[k] = head(section.bottom, section.bed + section.amount);
        is_full[k] = section.amount >= capacity(section.bottom, section.top) - section.bed;
    }
    // A full section's head is the highest surface pushing on it, through other full sections:
    // so a head passes along a tunnel while the tunnel holds only what fills it.
    for (bool raised = true; raised;) {
        raised = false;
        for (const Meeting& meeting : meetings) {
            for (const auto& [a, b] :
                 {std::pair{meeting.here, meeting.there}, std::pair{meeting.there, meeting.here}}) {
                if (is_full[a] && heads[b] > heads[a] && heads[b] > head(meeting.window, 0)) {
                    heads[a] = heads[b];
                    raised = true;
                }
            }
        }
    }
    return heads;
}

std::vector<Region::Transfer> Region::transfers(const std::vector<Column>& columns) const {
    const std::vector<Meeting> met = meetings(columns);
    const std::vector<std::int64_t> pushing = heads(met);
    std::vector<Transfer> transfers;
    for (const Meeting& meeting : met) {
        const std::int64_t here_head = pushing[meeting.here];
        const std::int64_t there_head = pushing[meeting.there];
        if (here_head == there_head) {
            continue;
        }
        const bool outward = here_head > there_head;
        const std::size_t from = outward ? meeting.here : meeting.there;
        const std::size_t to = outward ? meeting.there : meeting.here;
        // Two liquids meeting is a contact rule to come; until then they stand.
        if (sections_[from].liquid != Material::Air && sections_[to].liquid != Material::Air &&
            sections_[from].liquid != sections_[to].liquid) {
            continue;
        }
        // Only liquid standing up to the opening can run out through it.
        const std::int64_t above_sill =
            head(sections_[from].bottom, sections_[from].bed + sections_[from].amount) -
            head(meeting.window, 0);
        const std::int64_t fall = std::abs(here_head - there_head);
        std::int64_t amount = std::min(fall / closing, above_sill);
        // Lava thickens as it cools and runs the slower for it.
        if (molten(sections_[from].liquid) && sections_[from].amount > 0) {
            const double kelvin = globe::kelvin(static_cast<std::uint16_t>(
                sections_[from].heat / sections_[from].amount));
            amount = static_cast<std::int64_t>(
                static_cast<double>(amount) * lava_mobility(sections_[from].liquid, kelvin));
        }
        if (amount > 0) {
            transfers.push_back({.from = from,
                                 .to = to,
                                 .amount = amount,
                                 .fall = fall,
                                 .column = meeting.column,
                                 .along_i = meeting.along_i,
                                 .outward = outward});
        }
    }
    // No section gives more than it holds, nor takes more than fills it; what's asked of it is
    // scaled to that, so every amount moved is exactly taken from one and given to another.
    std::vector<std::int64_t> out(sections_.size(), 0);
    for (const Transfer& transfer : transfers) {
        out[transfer.from] += transfer.amount;
    }
    scale(transfers, &Transfer::from, out, [&](std::size_t k) { return sections_[k].amount; });
    std::vector<std::int64_t> in(sections_.size(), 0);
    for (const Transfer& transfer : transfers) {
        in[transfer.to] += transfer.amount;
    }
    scale(transfers, &Transfer::to, in, [&](std::size_t k) {
        return capacity(sections_[k].bottom, sections_[k].top) - sections_[k].bed -
               sections_[k].amount;
    });
    std::erase_if(transfers, [](const Transfer& transfer) { return transfer.amount <= 0; });
    return transfers;
}

void Region::scale(std::vector<Transfer>& transfers, std::size_t Transfer::* end,
                   const std::vector<std::int64_t>& asked, const auto& limit) {
    // Shared in proportion, rounded down, then the units rounding left are given back one by
    // one, so the limit is reached exactly where more was asked: a voxel a unit short of full
    // still fills.
    std::vector<std::int64_t> given(asked.size(), 0);
    for (Transfer& transfer : transfers) {
        const std::size_t k = transfer.*end;
        const std::int64_t most = std::max(limit(k), std::int64_t{0});
        if (asked[k] > most) {
            transfer.amount = transfer.amount * most / asked[k];
        }
        given[k] += transfer.amount;
    }
    for (Transfer& transfer : transfers) {
        const std::size_t k = transfer.*end;
        if (asked[k] > given[k] && given[k] < limit(k)) {
            ++transfer.amount;
            ++given[k];
        }
    }
}

void Region::want_around(Site site, std::set<std::uint64_t>& wanted) {
    // Liquid against bricks not held would move into them; under the sea's level what's beyond
    // is the sea, already where it would go.
    if (open_to_sea(site)) {
        return;
    }
    std::vector<Site> around;
    if (site.layer > 0) {
        around.push_back({.face = site.face, .i = site.i, .j = site.j, .layer = site.layer - 1});
    }
    for (const auto& step : sides) {
        if (const std::optional<Site> next = beside(site, step.first, step.second)) {
            around.push_back(*next);
        }
    }
    for (const Site next : around) {
        if (voxel(next) == nullptr) {
            wanted.insert(globe::brick_key(globe::brick_of(next)));
        }
    }
}

void Region::settle(const std::vector<Column>& columns, double seconds, Stepped& stepped) {
    const double thickness = world_->layers().thickness;
    std::set<std::uint64_t> wanted;
    for (const Column& column : columns) {
        std::size_t next = column.first;
        std::vector<std::tuple<Site, Held*, Voxel*>> run;
        walk(
            column,
            [&](Site site, Held& brick, Voxel& voxel) { run.emplace_back(site, &brick, &voxel); },
            [&] {
                if (run.empty()) {
                    return;
                }
                const Section& section = sections_[next++];
                const auto count = static_cast<std::int64_t>(run.size());
                HOG_ASSERT(section.amount <= (count * full) - section.bed,
                           "a section holding more than fills it");
                // Full from the bottom up, over the grain piled there, the rest in the voxel
                // over them.
                std::int64_t left = section.amount;
                // Each voxel of lava keeps its own heat; one newly reached takes the section's
                // mean.
                struct Hot {
                    Site site;
                    Held* brick;
                    Voxel* voxel;
                    double kelvin;
                    std::int64_t amount;
                };
                std::vector<Hot> hot;
                const double mean =
                    section.amount > 0 && molten(section.liquid)
                        ? globe::kelvin(static_cast<std::uint16_t>(section.heat / section.amount))
                        : 0.0;
                for (std::int64_t k = 0; k < count; ++k) {
                    auto [site, brick, voxel] = run[static_cast<std::size_t>(k)];
                    const std::int64_t amount =
                        std::clamp(left, std::int64_t{0}, full - voxel->grain_amount);
                    left -= amount;
                    const Material liquid = amount > 0 ? section.liquid : Material::Air;
                    const auto held_amount = static_cast<std::uint16_t>(amount);
                    const std::int64_t before = voxel->liquid_amount;
                    stepped.changed += std::abs(amount - before);
                    if (amount > 0 && molten(liquid)) {
                        hot.push_back({.site = site,
                                       .brick = brick,
                                       .voxel = voxel,
                                       .kelvin = before > 0 ? globe::kelvin(voxel->temperature)
                                                            : mean,
                                       .amount = amount});
                    }
                    if (voxel->liquid_amount != held_amount || voxel->liquid != liquid) {
                        voxel->liquid_amount = held_amount;
                        voxel->liquid = liquid;
                        brick->dirty = true;
                    }
                    // Only liquid on the move would run on; what stands still needs nothing.
                    if (amount > 0 && (section.moving || amount != before)) {
                        want_around(site, wanted);
                    }
                }
                // The top of the lava gives its heat to the air over it and sets into a crust; the
                // rest is warmed or cooled only through what lies over it.
                if (!hot.empty()) {
                    const bool exposed = section.amount + section.bed < count * full;
                    const double depth =
                        std::max(static_cast<double>(hot.back().amount) / full * thickness, 0.01);
                    hot.back().kelvin = cooled(section.liquid, hot.back().kelvin, ambient,
                                               exposed ? 1.0 : 0.0, depth, seconds);
                    for (std::size_t h = hot.size() - 1; h-- > 0;) {
                        conduct(section.liquid, hot[h].kelvin, hot[h + 1].kelvin, thickness,
                                seconds);
                    }
                    for (const Hot& each : hot) {
                        if (lava_mobility(section.liquid, each.kelvin) == 0.0) {
                            setting_.emplace_back(each.site, each.brick);
                            continue;
                        }
                        // Rounded by chance, since a step's cooling is far under a step of the
                        // stored temperature.
                        const auto temperature = static_cast<std::uint16_t>(
                            dithered(each.kelvin * 8.0, each.site, 2));
                        if (each.voxel->temperature != temperature) {
                            each.voxel->temperature = temperature;
                            each.brick->dirty = true;
                        }
                    }
                }
                run.clear();
            });
        HOG_ASSERT(next == column.first + column.count, "the column's sections don't match");
        report_wet(column, stepped);
    }
    stepped.wanted.reserve(wanted.size());
    for (const std::uint64_t key : wanted) {
        stepped.wanted.push_back(globe::brick_at_key(key));
    }
}

void Region::freeze(Site site, Held& brick) {
    Voxel& here = brick.voxels[static_cast<std::size_t>(globe::index_in_brick(site))];
    here.solid = here.liquid;
    here.liquid = Material::Air;
    here.liquid_amount = 0;
    here.grain = Material::Air;
    here.grain_amount = 0;
    here.condition = Voxel::sound;
    here.solid_amount = Voxel::full;
    brick.dirty = true;
}

void Region::report_wet(const Column& column, Stepped& stepped) const {
    std::int64_t standing = 0;
    for (std::size_t s = column.first; s < column.first + column.count; ++s) {
        if (!molten(sections_[s].liquid)) {
            standing += sections_[s].amount;
        }
    }
    if (standing > 0) {
        stepped.wet.push_back(
            {.face = column.face, .i = column.i, .j = column.j, .standing = standing});
    }
}

std::int64_t Region::dithered(double value, Site site, std::uint64_t salt) const {
    const double whole = std::floor(value);
    const std::uint64_t draw =
        hash_values(steps_, salt, std::uint64_t{site.face}, std::uint64_t{site.i},
                    std::uint64_t{site.j}, std::uint64_t{site.layer});
    const double unit = static_cast<double>(draw >> 11U) * 0x1.0p-53;
    return static_cast<std::int64_t>(whole) + (unit < value - whole ? 1 : 0);
}

std::int64_t Region::seeped(double rate, double seconds, Site site) const {
    return dithered(rate * seconds / world_->layers().thickness * full, site, 0);
}

// Liquid moves `seconds` of seepage from `over` into `under`: down into the ground at what it
// lets through; out of the ground only what it doesn't keep, dripping where it's open underneath.
std::int64_t Region::seep_down(Site site, Voxel& over, Voxel& under, double seconds) const {
    if ((open(over) && open(under)) || molten(over.liquid)) {
        return 0;
    }
    const double over_rate = open(over) ? 1.0 : globe::properties(ground_of(over)).conductivity;
    const double under_rate = open(under) ? 1.0 : globe::properties(ground_of(under)).conductivity;
    const std::int64_t give = open(over) ? over.liquid_amount : over.liquid_amount - kept(over);
    const std::int64_t room = (open(under) ? full : pores(under)) - under.liquid_amount;
    const std::int64_t moved =
        std::min({give, room, seeped(std::min(over_rate, under_rate), seconds, site)});
    if (moved <= 0) {
        return 0;
    }
    over.liquid_amount = static_cast<std::uint16_t>(over.liquid_amount - moved);
    under.liquid_amount = static_cast<std::uint16_t>(under.liquid_amount + moved);
    under.liquid = over.liquid;
    if (over.liquid_amount == 0) {
        over.liquid = Material::Air;
    }
    return moved;
}

void Region::seep(const std::vector<Column>& columns, double seconds, Stepped& stepped) {
    for (const Column& column : columns) {
        Held* under_brick = nullptr;
        Voxel* under = nullptr;
        walk_all(column, [&](Site site, Held& brick, Voxel& over, bool joined) {
            if (joined && under != nullptr) {
                if (const std::int64_t moved = seep_down(site, over, *under, seconds); moved > 0) {
                    stepped.changed += moved;
                    brick.dirty = true;
                    under_brick->dirty = true;
                }
            }
            under_brick = &brick;
            under = &over;
        });
    }
}

Stepped Region::step(double seconds) {
    Stepped stepped;
    ++steps_;
    loosen_fresh();
    inspect_support(seconds, stepped);
    std::vector<Column> all = columns();
    quench(stepped);
    cool_rock(seconds);
    hold_sea(all, stepped);
    gather(all);
    flux_.clear();
    for (const Transfer& transfer : transfers(all)) {
        Section& from = sections_[transfer.from];
        Section& to = sections_[transfer.to];
        // The load goes with the liquid, as thick as it's carried.
        const std::int64_t load = from.load * transfer.amount / from.amount;
        if (load > to.load) {
            to.carried = from.carried;
        }
        from.load -= load;
        to.load += load;
        const std::int64_t heat = from.heat * transfer.amount / from.amount;
        from.heat -= heat;
        to.heat += heat;
        from.amount -= transfer.amount;
        to.amount += transfer.amount;
        from.moving = true;
        to.moving = true;
        from.fall = std::max(from.fall, transfer.fall);
        to.fall = std::max(to.fall, transfer.fall);
        if (to.liquid == Material::Air) {
            to.liquid = from.liquid;
        }
        const Column& column = all[transfer.column];
        Flux& flux = flux_[column_key(column.face, column.i, column.j)];
        (transfer.along_i ? flux.along_i : flux.along_j) +=
            transfer.outward ? transfer.amount : -transfer.amount;
    }
    setting_.clear();
    settle(all, seconds, stepped);
    seep(all, seconds, stepped);
    carry(all, seconds, stepped);
    keep_loads(all);
    // Lava sets only once the sections that were gathered are done with.
    for (const auto& [site, brick] : setting_) {
        freeze(site, *brick);
        ++stepped.frozen;
    }
    slump(stepped);
    if (ice_years_ > 0.0) {
        glaciate(stepped, ice_years_ * seconds);
    }
    fly(seconds, stepped);
    std::ranges::sort(stepped.wanted, {}, [](const globe::BrickSite& site) {
        return globe::brick_key(site);
    });
    const auto same = std::ranges::unique(stepped.wanted, {}, [](const globe::BrickSite& site) {
        return globe::brick_key(site);
    });
    stepped.wanted.erase(same.begin(), same.end());
    // What settling and seeping took from the edge, the sea gives back before anyone sees it
    // short.
    hold_sea(all, stepped);
    return stepped;
}

void Region::carry(const std::vector<Column>& columns, double seconds, Stepped& stepped) {
    const double thickness = world_->layers().thickness;
    const double gravity = world_->generator().planet().gravity;
    for (const Column& column : columns) {
        std::size_t next = column.first;
        std::optional<Bed> bed;
        walk(
            column,
            [&](Site site, Held& brick, Voxel& here) {
                if (!bed) {
                    bed = Bed{.site = site, .brick = &brick, .bottom = &here};
                    if (site.layer > 0) {
                        const Site below{
                            .face = site.face, .i = site.i, .j = site.j, .layer = site.layer - 1};
                        bed->under = voxel(below);
                        bed->under_brick = held(globe::brick_of(below));
                    }
                }
            },
            [&] {
                if (!bed) {
                    return;
                }
                Section& section = sections_[next++];
                const double depth = static_cast<double>(section.amount) / full * thickness;
                if (section.fall > 0 && section.liquid != Material::Air &&
                    !molten(section.liquid)) {
                    // Drag on the bed under uniform flow: the liquid's weight down the slope of
                    // its surface, the columns taken as wide as the voxels are thick.
                    const double slope = static_cast<double>(section.fall) / full;
                    const double shear =
                        globe::properties(section.liquid).density * gravity * depth * slope;
                    wear(section, *bed, shear, seconds, stepped);
                }
                drop(section, *bed, depth, seconds, stepped);
                bed.reset();
            });
    }
}

void Region::wear(Section& section, const Bed& bed, double shear, double seconds,
                  Stepped& stepped) {
    // Grain on the bed goes first, then a voxel full of grain under it, then the shaped ground.
    Voxel* loose = nullptr;
    Held* loose_brick = nullptr;
    if (bed.bottom->grain_amount > 0) {
        loose = bed.bottom;
        loose_brick = bed.brick;
    } else if (bed.under != nullptr && bed.under->solid == Material::Air &&
               bed.under->grain_amount > 0) {
        loose = bed.under;
        loose_brick = bed.under_brick;
    } else if (bed.under == nullptr || open(*bed.under)) {
        return;
    }
    const Material material = loose != nullptr ? loose->grain : bed.under->solid;
    const globe::Properties& properties = globe::properties(material);
    if (shear <= properties.critical_shear) {
        return;
    }
    const double rate = properties.erodibility * (shear - properties.critical_shear);
    // A flow carries no more than its own bulk.
    const std::int64_t room = section.amount - section.load;
    if (room <= 0) {
        return;
    }
    const double thickness = world_->layers().thickness;
    std::int64_t units = 0;
    if (loose != nullptr) {
        units = std::min({dithered(rate * seconds / thickness * full, bed.site, 1),
                          std::int64_t{loose->grain_amount}, room});
        if (units <= 0) {
            return;
        }
        set_grain(*loose, loose->grain_amount - units);
        if (loose == bed.bottom) {
            section.bed -= units;
        }
        loose_brick->dirty = true;
    } else {
        const double thickness_mm = thickness * 1000.0;
        const std::int64_t mm =
            std::min({dithered(rate * seconds * 1000.0, bed.site, 1),
                      static_cast<std::int64_t>(static_cast<double>(room) * thickness_mm / full),
                      static_cast<std::int64_t>(thickness_mm)});
        if (mm <= 0) {
            return;
        }
        units = std::llround(static_cast<double>(mm) * full / thickness_mm);
        scour(bed.site, static_cast<int>(mm));
        stepped.eroded += units;
    }
    if (units > section.load) {
        section.carried = material;
    }
    section.load += units;
    stepped.changed += units;
}

void Region::drop(Section& section, const Bed& bed, double depth, double seconds,
                  Stepped& stepped) {
    if (section.load <= 0) {
        return;
    }
    const double sunk = globe::properties(section.carried).settling * seconds;
    // What settles out: the share of the depth the load sinks through, all of it if dry.
    std::int64_t dropped =
        depth <= 0.0 || sunk >= depth
            ? section.load
            : dithered(static_cast<double>(section.load) * sunk / depth, bed.site, 2);
    dropped = std::min({dropped, full - bed.bottom->grain_amount,
                        capacity(section.bottom, section.top) - section.bed - section.amount});
    if (dropped <= 0) {
        return;
    }
    if (dropped > std::int64_t{bed.bottom->grain_amount}) {
        bed.bottom->grain = section.carried;
    }
    set_grain(*bed.bottom, bed.bottom->grain_amount + dropped);
    bed.brick->dirty = true;
    section.bed += dropped;
    section.load -= dropped;
    if (section.load == 0) {
        section.carried = Material::Air;
    }
    stepped.changed += dropped;
}

void Region::scour(Site bed, int mm) {
    const double thickness = world_->layers().thickness;
    dig_units(bed, std::llround(static_cast<double>(mm) / (thickness * 1000.0) *
                                static_cast<double>(full)));
}

void Region::dig_units(Site bed, std::int64_t units) {
    const auto half = static_cast<std::uint16_t>(full / 2);
    // Down from the lowest open voxel, which holds the top of the shaped matter, taking what each
    // holds of it until the depth is taken; a voxel left less than half full is open.
    for (Site site = bed; units > 0;) {
        Voxel* here = voxel(site);
        if (here == nullptr) {
            break;
        }
        const std::int64_t take = std::min<std::int64_t>(here->solid_amount, units);
        if (take > 0) {
            here->solid_amount = static_cast<std::uint16_t>(here->solid_amount - take);
            if (here->solid_amount < half) {
                here->solid = Material::Air;
            }
            held(globe::brick_of(site))->dirty = true;
            units -= take;
        }
        if (site.layer == 0) {
            break;
        }
        --site.layer;
    }
}

std::vector<BrickSite> Region::flush() {
    std::vector<std::uint64_t> keys;
    for (auto& [key, brick] : held_) {
        if (brick.dirty) {
            keys.push_back(key);
            brick.dirty = false;
        }
    }
    std::ranges::sort(keys);
    std::vector<BrickSite> written;
    written.reserve(keys.size());
    for (const std::uint64_t key : keys) {
        const BrickSite site = globe::brick_at_key(key);
        world_->write(site, held_.at(key).voxels);
        written.push_back(site);
    }
    return written;
}

} // namespace hog::flow
