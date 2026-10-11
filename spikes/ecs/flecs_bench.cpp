#include "scenario.hpp"

#include <hog/jobs/job_system.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <flecs.h>

#include <cstdio>
#include <vector>

using namespace spike;

namespace {

flecs::entity spawn(flecs::world& world, int index) {
    flecs::entity e = world.entity();
    e.set<Position>(position_of(index));
    e.set<Cell>({cell_of(position_of(index))});
    switch (kind_of(index)) {
    case Kind::Creature:
        e.set<Velocity>(velocity_of(index));
        e.set<Health>({100.0F});
        e.set<Hunger>({0.0F});
        e.add<Creature>();
        break;
    case Kind::Prop:
        break;
    case Kind::Player:
        e.set<Velocity>(velocity_of(index));
        e.set<Health>({100.0F});
        e.add<Player>();
        break;
    }
    return e;
}

struct World {
    flecs::world world;
    std::vector<flecs::entity> entities;

    explicit World(bool deferred = false, bool dont_fragment = false) {
        if (dont_fragment) {
            world.component<InCombat>().add(flecs::DontFragment);
        }
        if (deferred) {
            world.defer_begin();
        }
        entities.reserve(entity_count);
        for (int i = 0; i < entity_count; ++i) {
            entities.push_back(spawn(world, i));
        }
        for (int i = 0; i < entity_count; i += 100) {
            entities[static_cast<std::size_t>(i)].add<InCombat>();
        }
        if (deferred) {
            world.defer_end();
        }
    }
};

} // namespace

TEST_CASE("flecs memory", "[flecs]") {
    const std::size_t before = heap_in_use();
    {
        const World world;
        std::printf("flecs: %zu bytes for %d entities\n", heap_in_use() - before, entity_count);
    }
}

TEST_CASE("flecs", "[flecs]") {
    BENCHMARK_ADVANCED("create 100k")(Catch::Benchmark::Chronometer meter) {
        meter.measure([] { return World{}.entities.size(); });
    };
    BENCHMARK_ADVANCED("create 100k (deferred)")(Catch::Benchmark::Chronometer meter) {
        meter.measure([] { return World{true}.entities.size(); });
    };

    World w;
    flecs::world& world = w.world;

    const auto move = world.query<Position, const Velocity>();
    BENCHMARK("move 75k (query)") {
        move.each([&](Position& p, const Velocity& v) {
            p.x += v.x * dt;
            p.y += v.y * dt;
            p.z += v.z * dt;
        });
        return dt;
    };

    BENCHMARK("move 75k (query, iter by table)") {
        move.run([&](flecs::iter& it) {
            while (it.next()) {
                auto p = it.field<Position>(0);
                auto v = it.field<const Velocity>(1);
                for (const auto i : it) {
                    p[i].x += v[i].x * dt;
                    p[i].y += v[i].y * dt;
                    p[i].z += v[i].z * dt;
                }
            }
        });
        return dt;
    };

    const auto needs = world.query<Health, Hunger>();
    BENCHMARK("needs 70k (Health + Hunger)") {
        int sum = 0;
        needs.each([&](Health& h, Hunger& g) {
            g.value += needs_dt;
            if (g.value > 50.0F) {
                h.value -= needs_dt;
            }
            sum += h.value < 0.0F ? 1 : 0;
        });
        return sum;
    };

    const auto cells = world.query<const Position, Cell>();
    BENCHMARK("cell update 100k") {
        int moved = 0;
        cells.each([&](const Position& p, Cell& c) {
            const std::uint64_t key = cell_of(p);
            moved += key != c.key ? 1 : 0;
            c.key = key;
        });
        return moved;
    };

    const auto combat = world.query_builder<const Position, Health>().with<InCombat>().build();
    BENCHMARK("combat 1% (tag + Position + Health)") {
        int sum = 0;
        combat.each([&](const Position& p, Health& h) {
            h.value -= 0.01F;
            sum += h.value < static_cast<float>(p.z) ? 1 : 0;
        });
        return sum;
    };

    BENCHMARK_ADVANCED("random get 10k")(Catch::Benchmark::Chronometer meter) {
        Random random{1};
        std::vector<flecs::entity> picks;
        for (int i = 0; i < lookups; ++i) {
            picks.push_back(w.entities[random() % w.entities.size()]);
        }
        meter.measure([&] {
            double sum = 0.0;
            for (const flecs::entity e : picks) {
                if (const auto* p = e.try_get<Position>()) {
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
                const flecs::entity e = w.entities[random() % w.entities.size()];
                if (e.has<InCombat>()) {
                    e.remove<InCombat>();
                } else {
                    e.add<InCombat>();
                }
            }
            return w.entities.size();
        });
    };

    BENCHMARK_ADVANCED("churn 1% (destroy + spawn)")(Catch::Benchmark::Chronometer meter) {
        Random random{3};
        meter.measure([&] {
            for (int i = 0; i < churn; ++i) {
                const std::size_t pick = random() % w.entities.size();
                w.entities[pick].destruct();
                w.entities[pick] = spawn(world, static_cast<int>(random() % entity_count));
            }
            return w.entities.size();
        });
    };
}

TEST_CASE("flecs non-fragmenting tag", "[flecs]") {
    World w{false, true};
    const auto combat = w.world.query_builder<const Position, Health>().with<InCombat>().build();
    BENCHMARK("combat 1% (non-fragmenting tag)") {
        int sum = 0;
        combat.each([&](const Position& p, Health& h) {
            h.value -= 0.01F;
            sum += h.value < static_cast<float>(p.z) ? 1 : 0;
        });
        return sum;
    };

    BENCHMARK_ADVANCED("toggle tag 1% (non-fragmenting)")(Catch::Benchmark::Chronometer meter) {
        Random random{2};
        meter.measure([&] {
            for (int i = 0; i < churn; ++i) {
                const flecs::entity e = w.entities[random() % w.entities.size()];
                if (e.has<InCombat>()) {
                    e.remove<InCombat>();
                } else {
                    e.add<InCombat>();
                }
            }
            return w.entities.size();
        });
    };
}

TEST_CASE("flecs threaded", "[flecs]") {
    World w;
    flecs::world& world = w.world;
    world.set_stage_count(static_cast<std::int32_t>(threads));
    hog::jobs::JobSystem jobs{threads};
    const auto move = world.query<Position, const Velocity>();

    BENCHMARK("move 75k (query, 8 threads)") {
        world.readonly_begin();
        jobs.parallel_for(threads, 1, [&](hog::jobs::Range range, std::uint32_t thread) {
            const flecs::world stage = world.get_stage(static_cast<std::int32_t>(thread));
            for (std::uint32_t worker = range.begin; worker < range.end; ++worker) {
                move.iter(stage)
                    .worker(static_cast<std::int32_t>(worker), static_cast<std::int32_t>(threads))
                    .each([](Position& p, const Velocity& v) {
                        p.x += v.x * dt;
                        p.y += v.y * dt;
                        p.z += v.z * dt;
                    });
            }
        });
        world.readonly_end();
        return threads;
    };
}
