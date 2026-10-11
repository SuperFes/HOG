#include <hog/testing/assertion_guard.hpp>

#include <format>

namespace hog::testing {

namespace {

[[noreturn]] void throwing_handler(const AssertionFailure& failure) {
    throw AssertionFailed(failure);
}

} // namespace

AssertionFailed::AssertionFailed(const AssertionFailure& failure)
    : std::logic_error(
          std::format("assertion failed: {} ({})", failure.expression, failure.message)),
      expression_(failure.expression), message_(failure.message) {}

AssertionGuard::AssertionGuard() noexcept : previous_(set_assertion_handler(&throwing_handler)) {}

AssertionGuard::~AssertionGuard() {
    set_assertion_handler(previous_);
}

} // namespace hog::testing
