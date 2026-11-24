#pragma once

#include "core/Types.h"

namespace revdock {

class AnimatedValue {
public:
    explicit AnimatedValue(float value = 0.0f) : value_(value), from_(value), to_(value) {}

    void setImmediate(float value) {
        value_ = from_ = to_ = value;
        start_ = 0.0;
        duration_ = 0.0;
    }

    void setTarget(double now, float target, double duration) {
        if (duration <= 0.0) {
            setImmediate(target);
            return;
        }
        if (target == to_ && (active() || value_ == target)) {
            return;
        }
        from_ = value_;
        to_ = target;
        start_ = now;
        duration_ = duration;
    }

    float update(double now) {
        if (duration_ <= 0.0) {
            value_ = to_;
            return value_;
        }
        const float t = static_cast<float>(clampd((now - start_) / duration_, 0.0, 1.0));
        value_ = lerpf(from_, to_, easeInOut(t));
        if (t >= 1.0f) {
            duration_ = 0.0;
        }
        return value_;
    }

    float value() const { return value_; }
    float target() const { return to_; }
    bool active() const { return duration_ > 0.0; }

private:
    float value_ = 0.0f;
    float from_ = 0.0f;
    float to_ = 0.0f;
    double start_ = 0.0;
    double duration_ = 0.0;
};

} // namespace revdock
