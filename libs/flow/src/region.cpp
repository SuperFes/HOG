#include <hog/core/assert.hpp>
#include <hog/flow/region.hpp>
#include <hog/globe/planet.hpp>

#include <algorithm>
#include <array>
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

constexpr std::array<std::pair<int, int>, 4> sides{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};

std::uint64_t column_key(std::uint8_t face, std::uint32_t i, std::uint32_t j) noexcept {
    return (std::uint64_t{face} << 42U) | (std::uint64_t{i} << 21U) | j;
}

std::uint64_t stack_key(BrickSite site) noexcept {
    return column_key(site.face, site.i, site.j);
}

bool open(const Voxel& voxel) noexcept {
    return voxel.solid == Material::Air;
}

std::int64_t head(int bottom, std::int64_t amount) noexcept {
    return (std::int64_t{bottom} * full) + amount;
}

std::int64_t capacity(int bottom, int top) noexcept {
    return std::int64_t{top - bottom} * full;
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
    std::vector<std::uint32_t>& stack = stacks_[stack_key(site)];
    stack.insert(std::ranges::lower_bound(stack, site.layer), site.layer);
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
    const std::int64_t side = std::int64_t{1} << world_->store().layers().level;
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

std::int64_t Region::liquid() const {
    std::int64_t total = 0;
    for (const auto& [key, brick] : held_) {
        for (const Voxel& voxel : brick.voxels) {
            if (open(voxel)) {
                total += voxel.liquid_amount;
            }
        }
    }
    return total;
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

bool Region::open_to_sea(Site site) const {
    const std::optional<double> sea = world_->generator().sea_level();
    return sea && world_->store().layers().middle(static_cast<int>(site.layer)) < *sea;
}

bool Region::at_edge(Site site) {
    return std::ranges::any_of(sides, [&](const auto& step) {
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
                if (here.liquid_amount >= full || !open_to_sea(site) || !at_edge(site)) {
                    return;
                }
                stepped.from_sea += full - here.liquid_amount;
                here.liquid = liquid_;
                here.liquid_amount = static_cast<std::uint16_t>(full);
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
                if (voxel.liquid_amount > 0 && run->liquid == Material::Air) {
                    run->liquid = voxel.liquid;
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
        heads[k] = head(section.bottom, section.amount);
        is_full[k] = section.amount >= capacity(section.bottom, section.top);
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
            head(sections_[from].bottom, sections_[from].amount) - head(meeting.window, 0);
        const std::int64_t amount =
            std::min(std::abs(here_head - there_head) / closing, above_sill);
        if (amount > 0) {
            transfers.push_back({.from = from,
                                 .to = to,
                                 .amount = amount,
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
        return capacity(sections_[k].bottom, sections_[k].top) - sections_[k].amount;
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

void Region::settle(const std::vector<Column>& columns, Stepped& stepped) {
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
                HOG_ASSERT(section.amount <= count * full, "a section holding more than fills it");
                // Full from the bottom up, the rest in the voxel over them.
                for (std::int64_t k = 0; k < count; ++k) {
                    auto [site, brick, voxel] = run[static_cast<std::size_t>(k)];
                    const std::int64_t amount =
                        std::clamp(section.amount - (k * full), std::int64_t{0}, full);
                    const Material liquid = amount > 0 ? section.liquid : Material::Air;
                    const auto held_amount = static_cast<std::uint16_t>(amount);
                    stepped.changed += std::abs(amount - voxel->liquid_amount);
                    if (voxel->liquid_amount != held_amount || voxel->liquid != liquid) {
                        voxel->liquid_amount = held_amount;
                        voxel->liquid = liquid;
                        brick->dirty = true;
                    }
                    if (amount > 0) {
                        want_around(site, wanted);
                    }
                }
                run.clear();
            });
        HOG_ASSERT(next == column.first + column.count, "the column's sections don't match");
    }
    stepped.wanted.reserve(wanted.size());
    for (const std::uint64_t key : wanted) {
        stepped.wanted.push_back(globe::brick_at_key(key));
    }
}

Stepped Region::step() {
    Stepped stepped;
    std::vector<Column> all = columns();
    hold_sea(all, stepped);
    gather(all);
    flux_.clear();
    for (const Transfer& transfer : transfers(all)) {
        Section& from = sections_[transfer.from];
        Section& to = sections_[transfer.to];
        from.amount -= transfer.amount;
        to.amount += transfer.amount;
        if (to.liquid == Material::Air) {
            to.liquid = from.liquid;
        }
        const Column& column = all[transfer.column];
        Flux& flux = flux_[column_key(column.face, column.i, column.j)];
        (transfer.along_i ? flux.along_i : flux.along_j) +=
            transfer.outward ? transfer.amount : -transfer.amount;
    }
    settle(all, stepped);
    return stepped;
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
