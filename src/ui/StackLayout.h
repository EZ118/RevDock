#pragma once

#include "core/Types.h"

#include <algorithm>
#include <cmath>

namespace revdock::stack {

// The reverse-engineered DOCKStackFanLayer::layout places stack items on a
// 3000 pt arc: items are stacked vertically one item apart, and each item is
// pushed to the right by `radius - sqrt(radius^2 - distance^2)`, so higher
// items curve to the right.  Each item is rotated by asin(distance / radius).
inline constexpr float kFanRadius = 3000.0f;
inline constexpr int kFanMaxItems = 17;
inline constexpr float kFanItemSize = 56.0f;
inline constexpr float kFanSpacing = 62.0f;
inline constexpr float kFanLabelHeight = 20.0f;
inline constexpr float kFanLabelWidth = 220.0f;

enum class Mode {
    Grid,
    Fan,
};

struct FanSlot {
    float centerX = 0.0f;
    float centerY = 0.0f;
    // Clockwise rotation in degrees (SDL's screen-space convention).
    float angle = 0.0f;
    float sagitta = 0.0f;
};

// Maximum number of icons that fit in a vertical fan on this display. The
// Dock binary derives a similar limit from the available edge length.
inline int fanCapacity(float availableLength, float spacing = kFanSpacing) {
    const int byLength = static_cast<int>(std::floor((availableLength - 48.0f) / spacing));
    return std::clamp(std::min(kFanMaxItems, byLength), 1, kFanMaxItems);
}

// `showAs`: 0 automatic, 1 fan, 2 grid, 3 list (list falls back to grid).
inline Mode modeFor(int showAs, size_t count, float availableLength = 1600.0f) {
    if (showAs == 2 || showAs == 3) {
        return Mode::Grid;
    }
    if (static_cast<int>(count) > fanCapacity(availableLength)) {
        return Mode::Grid;
    }
    return Mode::Fan;
}

// Slot for item `index` (0 = closest to the Dock) of `count` icons.
// `anchorX` is the centre of the bottom icon, `baseY` is the bottom edge of the
// icon column (window-local coordinates). Higher items move right along the arc.
inline FanSlot fanSlot(int index, int count, float anchorX, float baseY,
                       float itemSize = kFanItemSize, float spacing = kFanSpacing,
                       float radius = kFanRadius) {
    FanSlot slot;
    if (count <= 0) {
        slot.centerX = anchorX;
        slot.centerY = baseY - itemSize * 0.5f;
        return slot;
    }
    const float distance = (static_cast<float>(index) + 0.5f) * spacing;
    const float radiusSq = radius * radius;
    const float distanceSq = std::min(distance * distance, radiusSq);
    slot.sagitta = radius - std::sqrt(std::max(0.0f, radiusSq - distanceSq));
    slot.centerX = anchorX + slot.sagitta;
    slot.centerY = baseY - distance;
    slot.angle = std::asin(clampf(distance / radius, -1.0f, 1.0f)) * 180.0f / kPi;
    return slot;
}

// Rightward displacement of the topmost icon; useful for sizing the window.
inline float fanSagitta(int count, float spacing = kFanSpacing, float radius = kFanRadius) {
    if (count <= 0) {
        return 0.0f;
    }
    return fanSlot(count - 1, count, 0.0f, 0.0f, kFanItemSize, spacing, radius).sagitta;
}

// Nearest fan icon under (x, y), in stack-window local coordinates. The hit
// box is deliberately a little larger than the icon so the whole rounded plate
// is clickable even when the icon is rotated.
inline int fanIndexAt(float x, float y, int count, float anchorX, float baseY,
                      float itemSize = kFanItemSize, float spacing = kFanSpacing,
                      float radius = kFanRadius) {
    int best = -1;
    const float reach = itemSize * 0.5f + 6.0f;
    float bestDistance = reach;
    for (int i = 0; i < count; ++i) {
        const FanSlot slot = fanSlot(i, count, anchorX, baseY, itemSize, spacing, radius);
        const float dx = x - slot.centerX;
        const float dy = y - slot.centerY;
        if (std::abs(dx) > reach || std::abs(dy) > reach) {
            continue;
        }
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (distance <= bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}

} // namespace revdock::stack
