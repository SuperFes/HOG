#pragma once

#include <source_location>
#include <string_view>

namespace hog {

struct AssertionFailure {
    std::string_view expression;
    std::string_view message;
    std::source_location location;
};

// Called on a failed HOG_ASSERT. If the handler returns, the process aborts; tests install one that
// throws so a failure can be observed. Functions guarded by assertions are therefore not noexcept.
using AssertionHandler = void (*)(const AssertionFailure&);

// Returns the previous handler. Passing nullptr restores the default (print and abort).
AssertionHandler set_assertion_handler(AssertionHandler handler) noexcept;

[[noreturn]] void assertion_failed(std::string_view expression, std::string_view message,
                                   std::source_location location = std::source_location::current());

} // namespace hog

// Always on, including release builds: guards invariants whose violation would corrupt state.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): needs the expression text
#define HOG_ASSERT(condition, message)                                                             \
    ((condition) ? static_cast<void>(0) : ::hog::assertion_failed(#condition, message))
