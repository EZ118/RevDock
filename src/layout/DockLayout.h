#pragma once

#include "config/Settings.h"
#include "model/DockModel.h"

#include <SDL3/SDL_rect.h>

#include <vector>

namespace revdock {

enum class DockEdge { Bottom, Left, Right };

struct TileGeometry {
    SDL_FRect cell;
    SDL_FRect icon;
    SDL_FRect label;
    SDL_FRect indicator;
    float scale = 1.0f;
    bool separator = false;
    bool hovered = false;
};

struct LayoutResult {
    std::vector<TileGeometry> tiles;
    SDL_FRect bar;
    float baseSize = 48.0f;
    float iconSize = 48.0f;
    float barThickness = 64.0f;
    float totalWidth = 0.0f;
    float totalHeight = 0.0f;
    DockEdge edge = DockEdge::Bottom;
};

class DockLayout {
public:
    LayoutResult compute(const std::vector<DockItem> &items, const Settings &settings,
                         float windowWidth, float windowHeight, float mouseX, float mouseY,
                         bool mouseInside, float magEnvelope = 1.0f) const;

    float measureBaseLength(const std::vector<DockItem> &items, const Settings &settings) const;

private:
    static float tileUnits(const DockItem &item);
    static float slotWidth(const DockItem &item, float baseSize);
};

} // namespace revdock
