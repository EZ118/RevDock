#pragma once

#include <chrono>

namespace revdock {

class Clock {
public:
    Clock() : start_(std::chrono::steady_clock::now()) {}

    double now() const {
        const auto elapsed = std::chrono::steady_clock::now() - start_;
        return std::chrono::duration<double>(elapsed).count();
    }

private:
    std::chrono::steady_clock::time_point start_;
};

} // namespace revdock
