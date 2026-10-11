#pragma once

#include <hog/core/assert.hpp>

#include <stdexcept>
#include <string>

namespace hog::testing {

// Thrown instead of aborting while an AssertionGuard is alive.
class AssertionFailed : public std::logic_error {
public:
    explicit AssertionFailed(const AssertionFailure& failure);

    [[nodiscard]] const std::string& expression() const noexcept { return expression_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

private:
    std::string expression_;
    std::string message_;
};

// Makes HOG_ASSERT throw AssertionFailed for its lifetime, so tests can check that a precondition
// is enforced without killing the test process.
class AssertionGuard {
public:
    AssertionGuard() noexcept;
    ~AssertionGuard();

    AssertionGuard(const AssertionGuard&) = delete;
    AssertionGuard& operator=(const AssertionGuard&) = delete;
    AssertionGuard(AssertionGuard&&) = delete;
    AssertionGuard& operator=(AssertionGuard&&) = delete;

private:
    AssertionHandler previous_;
};

} // namespace hog::testing
