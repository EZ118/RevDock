#include "layout/DockLayout.h"

#include "core/Types.h"

#include <algorithm>
#include <cmath>

namespace revdock {
namespace {

constexpr float kIndicatorArea = 6.0f;
constexpr float kLabelHeight = 18.0f;
constexpr float kSeparatorWidth = 5.0f;

DockEdge edgeFromSettings(const Settings &settings) {
    const std::string value = settings.orientation();
    if (value == "left") {
        return DockEdge::Left;
    }
    if (value == "right") {
        return DockEdge::Right;
    }
    return DockEdge::Bottom;
}

float paddingFor(float baseSize) {
    return std::max(5.0f, baseSize * 0.12f);
}

} // namespace

float DockLayout::tileUnits(const DockItem &item) {
    return item.type == ItemType::Separator ? 0.0f : 1.0f;
}

// Minimized windows are shown as mini window screenshots, not as square icons:
// their slot is as wide as the window's aspect ratio (bounded so a very wide
// window cannot dominate the Dock). Everything else uses one square slot.
float DockLayout::slotWidth(const DockItem &item, float baseSize) {
    return item.type == ItemType::Separator ? kSeparatorWidth : baseSize;
}

float DockLayout::measureBaseLength(const std::vector<DockItem> &items,
                                    const Settings &settings) const {
    const float base = settings.tileSize();
    const float padding = paddingFor(base);
    if (items.empty()) {
        return padding * 2.0f;
    }
    const bool horizontal = settings.orientation() == "bottom";
    float total = 0.0f;
    for (const auto &item : items) {
        total += slotWidth(item, base);
    }
    total += padding * 0.75f * static_cast<float>(items.size() - 1);
    return total + padding * 2.0f;
}

LayoutResult DockLayout::compute(const std::vector<DockItem> &items, const Settings &settings,
                                 float windowWidth, float windowHeight, float mouseX, float mouseY,
                                 bool mouseInside, float magEnvelope) const {
    LayoutResult result;
    result.edge = edgeFromSettings(settings);
    result.baseSize = settings.tileSize();
    result.iconSize = settings.effectiveIconSize();
    const float large = settings.effectiveLargeSize();
    const bool horizontal = result.edge == DockEdge::Bottom;
    const float axisLength = horizontal ? windowWidth : windowHeight;
    const float padding = paddingFor(result.baseSize);
    // Outer gutters keep `padding`; the space between neighbouring tiles is a
    // quarter tighter than in the reference layout.
    float gap = padding * 0.75f;

    // Base positions. Widths are icon widths; gaps are laid out between items so
    // the icon-to-icon gap and the outer edge inset are both `padding`.
    std::vector<float> widths(items.size(), 0.0f);
    std::vector<float> centers(items.size(), 0.0f);
    float baseTotal = 0.0f;
    for (size_t i = 0; i < items.size(); ++i) {
        widths[i] = slotWidth(items[i], result.baseSize);
        baseTotal += widths[i];
    }
    if (items.size() > 1) {
        baseTotal += gap * static_cast<float>(items.size() - 1);
    }
    {
        float cursor = (axisLength - baseTotal) * 0.5f;
        for (size_t i = 0; i < items.size(); ++i) {
            centers[i] = cursor + widths[i] * 0.5f;
            cursor += widths[i] + gap;
        }
    }

    std::vector<float> scales(items.size(), 1.0f);
    if (mouseInside && settings.magnification() && large > result.baseSize) {
        const float range = result.baseSize * 3.0f;
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].type == ItemType::Separator) {
                continue;
            }
            const float distance = std::abs(centers[i] - mouseX);
            if (distance < range) {
                const float influence = 0.5f + 0.5f * std::cos(distance * kPi / range);
                scales[i] =
                    1.0f + (large / result.baseSize - 1.0f) * influence * magEnvelope;
            }
        }
    }

    for (size_t i = 0; i < items.size(); ++i) {
        widths[i] = slotWidth(items[i], result.baseSize * scales[i]);
    }
    auto computeTotal = [&]() {
        float total = 0.0f;
        for (float width : widths) {
            total += width;
        }
        if (items.size() > 1) {
            total += gap * static_cast<float>(items.size() - 1);
        }
        return total;
    };
    float total = computeTotal();
    for (int attempt = 0; attempt < 2 && total > axisLength - 12.0f && total > 0.0f; ++attempt) {
        const float factor = (axisLength - 12.0f) / total;
        gap = std::max(2.0f, gap * factor);
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].type == ItemType::Separator) {
                continue;
            }
            scales[i] *= factor;
            widths[i] = slotWidth(items[i], result.baseSize * scales[i]);
        }
        total = computeTotal();
    }

    const float start = (axisLength - total) * 0.5f;
    result.totalWidth = horizontal ? total : 0.0f;
    result.totalHeight = horizontal ? 0.0f : total;

    const float barBottom = windowHeight - 3.0f;
    const float barThickness = result.baseSize + padding * 2.0f + kIndicatorArea;
    const float barTop = barBottom - barThickness;
    result.barThickness = barThickness;

    if (horizontal) {
        result.bar = SDL_FRect{start - padding, barTop, total + padding * 2.0f, barThickness};
    } else {
        const float barLeft = (result.edge == DockEdge::Left ? 4.0f : windowWidth - barThickness - 4.0f);
        result.bar = SDL_FRect{barLeft, start - padding, barThickness, total + padding * 2.0f};
    }

    float cursor = start;
    result.tiles.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        const float width = widths[i];
        const float scale = scales[i];
        const float cellSize = result.baseSize * scale;
        TileGeometry geo;
        geo.scale = scale;
        geo.separator = items[i].type == ItemType::Separator;

        if (horizontal) {
            const float cellBottom = barBottom - padding - kIndicatorArea;
            const float cellTop = cellBottom - cellSize;
            if (geo.separator) {
                const float inset = padding + 5.0f;
                geo.icon = SDL_FRect{cursor + width * 0.5f - 0.5f, barTop + inset,
                                     1.0f, barThickness - 2.0f * inset};
                geo.cell = SDL_FRect{cursor, barTop, width, barThickness};
            } else {
                // `width` already folds in the minimized-window aspect ratio.
                geo.icon = SDL_FRect{cursor, cellTop, width, cellSize};
                geo.cell = geo.icon;
            }
            const float center = cursor + width * 0.5f;
            geo.label = SDL_FRect{center - 80.0f * scale, cellTop - kLabelHeight - 4.0f,
                                  160.0f * scale, kLabelHeight};
            geo.indicator = SDL_FRect{center - 2.5f, cellBottom + 2.0f, 5.0f, 5.0f};
        } else {
            const bool left = result.edge == DockEdge::Left;
            const float cellLeft = left ? (result.bar.x + padding)
                                        : (result.bar.x + result.bar.w - padding - cellSize);
            const float cellTop = cursor;
            if (geo.separator) {
                const float inset = padding + 5.0f;
                geo.icon = SDL_FRect{result.bar.x + inset, cursor + width * 0.5f - 0.5f,
                                     barThickness - 2.0f * inset, 1.0f};
                geo.cell = SDL_FRect{result.bar.x, cursor, barThickness, width};
            } else {
                geo.icon = SDL_FRect{cellLeft, cellTop, cellSize, cellSize};
                geo.cell = geo.icon;
            }
            const float center = cursor + width * 0.5f;
            geo.label = SDL_FRect{left ? cellLeft + cellSize + 8.0f : cellLeft - 168.0f,
                                  center - kLabelHeight * 0.5f, 160.0f, kLabelHeight};
            geo.indicator = SDL_FRect{left ? result.bar.x + result.bar.w + 2.0f
                                           : result.bar.x - 7.0f,
                                      center - 2.5f, 5.0f, 5.0f};
        }

        geo.hovered = mouseInside && contains(geo.cell, mouseX, mouseY);
        result.tiles.push_back(geo);
        cursor += width + gap;
    }

    return result;
}

} // namespace revdock
