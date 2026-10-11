#include <hog/jobs/job_system.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <numeric>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

constexpr std::uint32_t pool_threads = 4;

} // namespace

TEST_CASE("the pool has the requested number of threads", "[jobs]") {
    const hog::jobs::JobSystem jobs{pool_threads};
    CHECK(jobs.thread_count() == pool_threads);

    const hog::jobs::JobSystem defaulted;
    CHECK(defaulted.thread_count() >= 1);
}

TEST_CASE("parallel_for visits every item exactly once", "[jobs]") {
    hog::jobs::JobSystem jobs{pool_threads};
    const std::uint32_t count = GENERATE(1U, 7U, 1000U, 100'003U);
    const std::uint32_t min_chunk = GENERATE(1U, 64U);

    // Catch2 assertions aren't thread-safe, so workers only record what they saw.
    std::vector<std::atomic<std::uint32_t>> visits(count);
    std::atomic<bool> empty_range{false};
    jobs.parallel_for(count, min_chunk, [&](hog::jobs::Range range, std::uint32_t) {
        if (range.begin >= range.end) {
            empty_range = true;
        }
        for (std::uint32_t i = range.begin; i < range.end; ++i) {
            visits[i].fetch_add(1, std::memory_order_relaxed);
        }
    });

    CHECK_FALSE(empty_range.load());
    CHECK(std::ranges::all_of(visits, [](const auto& v) { return v.load() == 1; }));
}

TEST_CASE("results written per item are visible after parallel_for returns", "[jobs]") {
    hog::jobs::JobSystem jobs{pool_threads};
    constexpr std::uint32_t count = 50'000;

    std::vector<std::uint64_t> squares(count);
    jobs.parallel_for(count, 256, [&](hog::jobs::Range range, std::uint32_t) {
        for (std::uint32_t i = range.begin; i < range.end; ++i) {
            squares[i] = std::uint64_t{i} * i;
        }
    });

    std::uint64_t expected = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        expected += i * i;
    }
    CHECK(std::accumulate(squares.begin(), squares.end(), std::uint64_t{0}) == expected);
}

TEST_CASE("thread indices stay below thread_count", "[jobs]") {
    hog::jobs::JobSystem jobs{pool_threads};

    std::mutex mutex;
    std::set<std::uint32_t> seen;
    jobs.parallel_for(10'000, 1, [&](hog::jobs::Range, std::uint32_t thread) {
        const std::scoped_lock lock{mutex};
        seen.insert(thread);
    });

    REQUIRE_FALSE(seen.empty());
    CHECK(*seen.rbegin() < jobs.thread_count());
}

TEST_CASE("parallel_for can be nested inside a job", "[jobs]") {
    hog::jobs::JobSystem jobs{pool_threads};
    constexpr std::uint32_t outer = 16;
    constexpr std::uint32_t inner = 1'000;

    std::atomic<std::uint32_t> total{0};
    jobs.parallel_for(outer, 1, [&](hog::jobs::Range range, std::uint32_t) {
        for (std::uint32_t i = range.begin; i < range.end; ++i) {
            jobs.parallel_for(inner, 16, [&](hog::jobs::Range sub, std::uint32_t) {
                total.fetch_add(sub.end - sub.begin, std::memory_order_relaxed);
            });
        }
    });

    CHECK(total.load() == outer * inner);
}

TEST_CASE("an empty range runs nothing", "[jobs]") {
    hog::jobs::JobSystem jobs{pool_threads};
    bool ran = false;
    jobs.parallel_for(0, 1, [&](hog::jobs::Range, std::uint32_t) { ran = true; });
    CHECK_FALSE(ran);
}

TEST_CASE("an exception thrown by a job reaches the caller", "[jobs]") {
    hog::jobs::JobSystem jobs{pool_threads};

    CHECK_THROWS_AS(jobs.parallel_for(10'000, 1,
                                      [](hog::jobs::Range range, std::uint32_t) {
                                          if (range.begin <= 5'000 && 5'000 < range.end) {
                                              throw std::runtime_error{"job failed"};
                                          }
                                      }),
                    std::runtime_error);

    // The pool stays usable afterwards.
    std::atomic<std::uint32_t> total{0};
    jobs.parallel_for(100, 1, [&](hog::jobs::Range range, std::uint32_t) {
        total.fetch_add(range.end - range.begin, std::memory_order_relaxed);
    });
    CHECK(total.load() == 100);
}

TEST_CASE("parallel_for checks its preconditions", "[jobs]") {
    hog::jobs::JobSystem jobs{pool_threads};
    const hog::testing::AssertionGuard guard;
    const auto noop = [](hog::jobs::Range, std::uint32_t) {};

    CHECK_THROWS_AS(jobs.parallel_for(10, 0, noop), hog::testing::AssertionFailed);

    bool rejected = false;
    std::thread outsider{[&] {
        try {
            jobs.parallel_for(10, 1, noop);
        } catch (const hog::testing::AssertionFailed&) {
            rejected = true;
        }
    }};
    outsider.join();
    CHECK(rejected);
}
