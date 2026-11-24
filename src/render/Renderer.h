#pragma once

#include "core/Types.h"

#include <SDL3/SDL_render.h>
#include <SDL3/SDL_surface.h>

#include <map>
#include <string>
#include <tuple>

namespace revdock {

class Renderer {
public:
    Renderer(SDL_Renderer *renderer, float contentScale);
    ~Renderer();

    void beginFrame();
    void endFrame();
    void setDrawColor(SDL_Color color);
    void setOpacity(float opacity) { opacity_ = clampf(opacity, 0.0f, 1.0f); }
    float opacity() const { return opacity_; }

    void fillRect(const SDL_FRect &rect, SDL_Color color);
    void roundedRect(const SDL_FRect &rect, float radius, SDL_Color color);
    // Anti-aliased ellipse filled to the rect's bounds.  The mask is built at
    // the display's device resolution and drawn 1:1, so tiny indicators (the
    // Dock running dot, the Launchpad page dots) stay round and sharp instead
    // of being a stretched 5 px mask.
    void circle(const SDL_FRect &rect, SDL_Color color);
    // Bakes the whole rounded rect at device resolution.  Used for large but
    // static panels where a nine-slice would leave faint seams.
    void roundedRectExact(const SDL_FRect &rect, float radius, SDL_Color color);
    void thickLine(float x1, float y1, float x2, float y2, float thickness, SDL_Color color);
    void shadow(const SDL_FRect &rect, float radius, SDL_Color color, float spread);

    void texture(SDL_Texture *texture, const SDL_FRect &dst, SDL_Color tint = rgba(255, 255, 255, 255),
                 float rotation = 0.0f, SDL_FPoint center = {0.0f, 0.0f});

    void folderIcon(const SDL_FRect &rect, SDL_Color body, SDL_Color tab);
    void trashIcon(const SDL_FRect &rect, SDL_Color body, SDL_Color lid);
    void gridIcon(const SDL_FRect &rect, SDL_Color color);
    void placeholderIcon(const SDL_FRect &rect, SDL_Color body, SDL_Color accent);

    SDL_Renderer *raw() const { return renderer_; }
    float scale() const { return scale_; }

private:
    SDL_Renderer *renderer_ = nullptr;
    float scale_ = 1.0f;
    float opacity_ = 1.0f;
    std::map<int, SDL_Texture *> maskCache_;
    std::map<std::tuple<int, int, int>, SDL_Texture *> shapeCache_;

    SDL_Texture *cornerMask(int radius);
    SDL_Texture *deviceMask(int width, int height, float radiusPx);
    void drawMaskSlices(SDL_Texture *mask, int radius, const SDL_FRect &dst, SDL_Color color);
};

} // namespace revdock
