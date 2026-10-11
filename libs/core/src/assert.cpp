#include <hog/core/assert.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <print>

namespace hog {

namespace {

void default_handler(const AssertionFailure& failure) {
    std::println(stderr, "{}:{}: assertion failed: {} ({}) in {}", failure.location.file_name(),
                 failure.location.line(), failure.expression, failure.message,
                 failure.location.function_name());
}

// Process-wide by design: one policy for every assertion in every thread.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<AssertionHandler> current_handler{&default_handler};

} // namespace

AssertionHandler set_assertion_handler(AssertionHandler handler) noexcept {
    return current_handler.exchange(handler != nullptr ? handler : &default_handler);
}

void assertion_failed(std::string_view expression, std::string_view message,
                      std::source_location location) {
    current_handler.load()(
        AssertionFailure{.expression = expression, .message = message, .location = location});
    std::abort();
}

} // namespace hog
