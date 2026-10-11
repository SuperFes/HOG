#include "scenario.hpp"

#include <hog/jobs/job_system.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <entt/entity/registry.hpp>

#include <cstdio>
#include <utility>
#include <vector>

using namespace spike;

namespace {

entt::entity spawn(entt::registry& registry, int index) {
    const entt::entity e = registry.create();
    registry.emplace<Position>(e, position_of(index));
    registry.emplace<Cell>(e, cell_of(position_of(index)));
    switch (kind_of(index)) {
    case Kind::Creature:
        registry.emplace<Velocity>(e, velocity_of(index));
        registry.emplace<Health>(e, 100.0F);
        registry.emplace<Hunger>(e, 0.0F);
        registry.emplace<Creature>(e);
        break;
    case Kind::Prop:
        break;
    case Kind::Player:
        registry.emplace<Velocity>(e, velocity_of(index));
        registry.emplace<Health>(e, 100.0F);
        registry.emplace<Player>(e);
        break;
    }
    return e;
}

struct World {
    entt::registry registry;
    std::vector<entt::entity> entities;

    explicit World(bool grouped = false) {
        if (grouped) {
            static_cast<void>(registry.group<Position, Velocity>());
        }
        entities.reserve(entity_count);
        for (int i = 0; i < entity_count; ++i) {
            entities.push_back(spawn(registry, i));
        }
        for (int i = 0; i < entity_count; i += 100) {
            registry.emplace<InCombat>(entities[static_cast<std::size_t>(i)]);
        }
    }
};

} // namespace

TEST_CASE("EnTT memory", "[entt]") {
    const std::size_t before = heap_in_use();
    {
        const World world;
        std::printf("EnTT: %zu bytes for %d entities\n", heap_in_use() - before, entity_count);
    }
}

TEST_CASE("EnTT", "[entt]") {
    BENCHMARK_ADVANCED("create 100k")(Catch::Benchmark::Chronometer meter) {
        meter.measure([] { return World{}.entities.size(); });
    };

    World world;
    auto& registry = world.registry;

    BENCHMARK("move 75k (view)") {
        registry.view<Position, const Velocity>().each([&](Position& p, const Velocity& v) {
            p.x += v.x * dt;
            p.y += v.y * dt;
            p.z += v.z * dt;
        });
        return dt;
    };

    BENCHMARK("needs 70k (Health + Hunger)") {
        int sum = 0;
        registry.view<Health, Hunger>().each([&](Health& h, Hunger& g) {
            g.value += needs_dt;
            if (g.value > 50.0F) {
                h.value -= needs_dt;
            }
            sum += h.value < 0.0F ? 1 : 0;
        });
        return sum;
    };

    BENCHMARK("cell update 100k") {
        int moved = 0;
        registry.view<const Position, Cell>().each([&](const Position& p, Cell& c) {
            const std::uint64_t key = cell_of(p);
            moved += key != c.key ? 1 : 0;
            c.key = key;
        });
        return moved;
    };

    BENCHMARK("combat 1% (tag + Position + Health)") {
        int sum = 0;
        registry.view<InCombat, const Position, Health>().each([&](const Position& p, Health& h) {
            h.value -= 0.01F;
            sum += h.value < static_cast<float>(p.z) ? 1 : 0;
        });
        return sum;
    };

    BENCHMARK_ADVANCED("random get 10k")(Catch::Benchmark::Chronometer meter) {
        Random random{1};
        std::vector<entt::entity> picks;
        for (int i = 0; i < lookups; ++i) {
            picks.push_back(world.entities[random() % world.entities.size()]);
        }
        meter.measure([&] {
            double sum = 0.0;
            for (const entt::entity e : picks) {
                if (const auto* p = registry.try_get<Position>(e)) {
                    sum += p->x;
                }
            }
            return sum;
        });
    };

    BENCHMARK_ADVANCED("toggle tag 1%")(Catch::Benchmark::Chronometer meter) {
        Random random{2};
        meter.measure([&] {
            for (int i = 0; i < churn; ++i) {
                const entt::entity e = world.entities[random() % world.entities.size()];
                if (registry.all_of<InCombat>(e)) {
                    registry.remove<InCombat>(e);
                } else {
                    registry.emplace<InCombat>(e);
                }
            }
            return registry.storage<InCombat>().size();
        });
    };

    BENCHMARK_ADVANCED("churn 1% (destroy + spawn)")(Catch::Benchmark::Chronometer meter) {
        Random random{3};
        meter.measure([&] {
            for (int i = 0; i < churn; ++i) {
                const std::size_t pick = random() % world.entities.size();
                registry.destroy(world.entities[pick]);
                world.entities[pick] = spawn(registry, static_cast<int>(random() % entity_count));
            }
            return world.entities.size();
        });
    };
}

TEST_CASE("EnTT grouped", "[entt]") {
    World world{true};
    auto group = world.registry.group<Position, Velocity>();

    BENCHMARK("move 75k (owning group)") {
        group.each([&](Position& p, const Velocity& v) {
            p.x += v.x * dt;
            p.y += v.y * dt;
            p.z += v.z * dt;
        });
        return dt;
    };

    // Created after the single-threaded runs, whose timings idle workers would disturb.
    hog::jobs::JobSystem jobs{threads};
    BENCHMARK("move 75k (group, 8 threads)") {
        const auto count = static_cast<std::uint32_t>(group.size());
        const auto* first = group.handle().data();
        jobs.parallel_for(count, 1'024, [&](hog::jobs::Range range, std::uint32_t) {
            for (std::uint32_t i = range.begin; i < range.end; ++i) {
                auto [p, v] = group.get<Position, Velocity>(first[i]);
                p.x += v.x * dt;
                p.y += v.y * dt;
                p.z += v.z * dt;
            }
        });
        return count;
    };
}
