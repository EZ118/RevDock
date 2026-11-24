#pragma once

#include "core/Types.h"

#include <SDL3/SDL_render.h>

#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

struct TTF_Font;

namespace revdock {

class TextRenderer {
public:
    TextRenderer(SDL_Renderer *renderer, const std::string &regularFont,
                 const std::string &fallbackFont);
    ~TextRenderer();

    void drawText(const std::string &text, float x, float y, float pixelSize, SDL_Color color);
    void drawTextCentered(const std::string &text, const SDL_FRect &box, float pixelSize, SDL_Color color);
    void drawTextEllipsized(const std::string &text, const SDL_FRect &box, float pixelSize,
                            SDL_Color color);
    void drawTextEllipsizedLeft(const std::string &text, const SDL_FRect &box, float pixelSize,
                                SDL_Color color);
    void drawTextEllipsizedRight(const std::string &text, const SDL_FRect &box, float pixelSize,
                                 SDL_Color color);
    void drawTextEllipsizedRotated(const std::string &text, const SDL_FRect &box,
                                   float pixelSize, SDL_Color color, float rotation);
    SDL_FPoint measure(const std::string &text, float pixelSize);
    float lineHeight(float pixelSize);
    void setOpacity(float opacity) { opacity_ = clampf(opacity, 0.0f, 1.0f); }
    void clear();

private:
    struct CachedTexture {
        SDL_Texture *texture = nullptr;
        float width = 0.0f;
        float height = 0.0f;
    };

    SDL_Renderer *renderer_ = nullptr;
    std::string regularPath_;
    std::string fallbackPath_;
    std::map<int, TTF_Font *> fonts_;
    std::vector<TTF_Font *> fallbackFonts_;
    std::map<std::tuple<std::string, int, uint32_t>, CachedTexture> cache_;
    float opacity_ = 1.0f;

    TTF_Font *font(float pixelSize);
    CachedTexture *texture(const std::string &text, float pixelSize, SDL_Color color);
    std::string ellipsized(const std::string &text, float pixelSize, float maxWidth);
};

} // namespace revdock
