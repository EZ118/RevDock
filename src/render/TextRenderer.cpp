#include "render/TextRenderer.h"

#include "core/Log.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <cmath>

namespace revdock {

TextRenderer::TextRenderer(SDL_Renderer *renderer, const std::string &regularFont,
                           const std::string &fallbackFont)
    : renderer_(renderer), regularPath_(regularFont), fallbackPath_(fallbackFont) {}

TextRenderer::~TextRenderer() {
    clear();
    for (auto &[size, fontPtr] : fonts_) {
        (void)size;
        if (fontPtr) {
            TTF_CloseFont(fontPtr);
        }
    }
    for (TTF_Font *fontPtr : fallbackFonts_) {
        if (fontPtr) {
            TTF_CloseFont(fontPtr);
        }
    }
}

TTF_Font *TextRenderer::font(float pixelSize) {
    const int key = std::max(8, static_cast<int>(std::lround(pixelSize)));
    auto found = fonts_.find(key);
    if (found != fonts_.end()) {
        return found->second;
    }
    TTF_Font *fontPtr = TTF_OpenFont(regularPath_.c_str(), static_cast<float>(key));
    if (!fontPtr) {
        RD_LOG_ERROR("TTF_OpenFont failed for %s: %s", regularPath_.c_str(), SDL_GetError());
        return nullptr;
    }
    if (!fallbackPath_.empty()) {
        TTF_Font *fallback = TTF_OpenFont(fallbackPath_.c_str(), static_cast<float>(key));
        if (fallback && TTF_AddFallbackFont(fontPtr, fallback)) {
            fallbackFonts_.push_back(fallback);
        } else if (fallback) {
            TTF_CloseFont(fallback);
        }
    }
    TTF_SetFontHinting(fontPtr, TTF_HINTING_LIGHT);
    fonts_[key] = fontPtr;
    return fontPtr;
}

TextRenderer::CachedTexture *TextRenderer::texture(const std::string &text, float pixelSize,
                                                    SDL_Color color) {
    const int key = std::max(8, static_cast<int>(std::lround(pixelSize)));
    const uint32_t colorKey = (static_cast<uint32_t>(color.r) << 24) |
                              (static_cast<uint32_t>(color.g) << 16) |
                              (static_cast<uint32_t>(color.b) << 8) | color.a;
    const auto cacheKey = std::make_tuple(text, key, colorKey);
    auto found = cache_.find(cacheKey);
    if (found != cache_.end()) {
        return &found->second;
    }
    TTF_Font *fontPtr = font(pixelSize);
    if (!fontPtr) {
        return nullptr;
    }
    SDL_Surface *surface = TTF_RenderText_Blended(fontPtr, text.c_str(), text.size(), color);
    if (!surface) {
        return nullptr;
    }
    CachedTexture entry;
    entry.width = static_cast<float>(surface->w);
    entry.height = static_cast<float>(surface->h);
    entry.texture = SDL_CreateTextureFromSurface(renderer_, surface);
    SDL_DestroySurface(surface);
    if (!entry.texture) {
        return nullptr;
    }
    SDL_SetTextureBlendMode(entry.texture, SDL_BLENDMODE_BLEND);
    auto [it, inserted] = cache_.emplace(cacheKey, entry);
    (void)inserted;
    return &it->second;
}

void TextRenderer::drawText(const std::string &text, float x, float y, float pixelSize,
                            SDL_Color color) {
    CachedTexture *cached = texture(text, pixelSize, color);
    if (!cached) {
        return;
    }
    SDL_FRect dst{x, y, cached->width, cached->height};
    SDL_SetTextureAlphaMod(cached->texture, static_cast<uint8_t>(color.a * opacity_));
    SDL_RenderTexture(renderer_, cached->texture, nullptr, &dst);
}

void TextRenderer::drawTextCentered(const std::string &text, const SDL_FRect &box, float pixelSize,
                                    SDL_Color color) {
    CachedTexture *cached = texture(text, pixelSize, color);
    if (!cached) {
        return;
    }
    SDL_FRect dst{box.x + (box.w - cached->width) * 0.5f,
                  box.y + (box.h - cached->height) * 0.5f,
                  cached->width, cached->height};
    SDL_SetTextureAlphaMod(cached->texture, static_cast<uint8_t>(color.a * opacity_));
    SDL_RenderTexture(renderer_, cached->texture, nullptr, &dst);
}

void TextRenderer::drawTextEllipsized(const std::string &text, const SDL_FRect &box,
                                      float pixelSize, SDL_Color color) {
    const std::string draw = ellipsized(text, pixelSize, box.w);
    CachedTexture *cached = texture(draw, pixelSize, color);
    if (!cached) {
        return;
    }
    SDL_FRect dst{box.x + (box.w - cached->width) * 0.5f,
                  box.y + (box.h - cached->height) * 0.5f,
                  cached->width, cached->height};
    SDL_SetTextureAlphaMod(cached->texture, static_cast<uint8_t>(color.a * opacity_));
    SDL_RenderTexture(renderer_, cached->texture, nullptr, &dst);
}

void TextRenderer::drawTextEllipsizedLeft(const std::string &text, const SDL_FRect &box,
                                          float pixelSize, SDL_Color color) {
    const std::string draw = ellipsized(text, pixelSize, box.w);
    CachedTexture *cached = texture(draw, pixelSize, color);
    if (!cached) {
        return;
    }
    SDL_FRect dst{box.x, box.y + (box.h - cached->height) * 0.5f, cached->width,
                  cached->height};
    SDL_SetTextureAlphaMod(cached->texture, static_cast<uint8_t>(color.a * opacity_));
    SDL_RenderTexture(renderer_, cached->texture, nullptr, &dst);
}

void TextRenderer::drawTextEllipsizedRight(const std::string &text, const SDL_FRect &box,
                                           float pixelSize, SDL_Color color) {
    const std::string draw = ellipsized(text, pixelSize, box.w);
    CachedTexture *cached = texture(draw, pixelSize, color);
    if (!cached) {
        return;
    }
    SDL_FRect dst{box.x + box.w - cached->width,
                  box.y + (box.h - cached->height) * 0.5f, cached->width, cached->height};
    SDL_SetTextureAlphaMod(cached->texture, static_cast<uint8_t>(color.a * opacity_));
    SDL_RenderTexture(renderer_, cached->texture, nullptr, &dst);
}

void TextRenderer::drawTextEllipsizedRotated(const std::string &text, const SDL_FRect &box,
                                             float pixelSize, SDL_Color color,
                                             float rotation) {
    const std::string draw = ellipsized(text, pixelSize, box.w);
    CachedTexture *cached = texture(draw, pixelSize, color);
    if (!cached) {
        return;
    }
    SDL_FRect dst{box.x + (box.w - cached->width) * 0.5f,
                  box.y + (box.h - cached->height) * 0.5f,
                  cached->width, cached->height};
    SDL_SetTextureAlphaMod(cached->texture, static_cast<uint8_t>(color.a * opacity_));
    const SDL_FPoint center{dst.w * 0.5f, dst.h * 0.5f};
    SDL_RenderTextureRotated(renderer_, cached->texture, nullptr, &dst, rotation, &center,
                             SDL_FLIP_NONE);
}

std::string TextRenderer::ellipsized(const std::string &text, float pixelSize, float maxWidth) {
    TTF_Font *fontPtr = font(pixelSize);
    if (!fontPtr) {
        return text;
    }
    int width = 0;
    int height = 0;
    TTF_GetStringSize(fontPtr, text.c_str(), text.size(), &width, &height);
    if (static_cast<float>(width) <= maxWidth) {
        return text;
    }
    std::string draw = text;
    const std::string ellipsis = "...";
    while (!draw.empty()) {
        TTF_GetStringSize(fontPtr, (draw + ellipsis).c_str(), draw.size() + ellipsis.size(),
                          &width, &height);
        if (static_cast<float>(width) <= maxWidth) {
            break;
        }
        do {
            draw.pop_back();
        } while (!draw.empty() && (static_cast<unsigned char>(draw.back()) & 0xC0) == 0x80);
    }
    return draw + ellipsis;
}

SDL_FPoint TextRenderer::measure(const std::string &text, float pixelSize) {
    TTF_Font *fontPtr = font(pixelSize);
    if (!fontPtr) {
        return {0.0f, 0.0f};
    }
    int width = 0;
    int height = 0;
    TTF_GetStringSize(fontPtr, text.c_str(), text.size(), &width, &height);
    return {static_cast<float>(width), static_cast<float>(height)};
}

float TextRenderer::lineHeight(float pixelSize) {
    TTF_Font *fontPtr = font(pixelSize);
    return fontPtr ? static_cast<float>(TTF_GetFontHeight(fontPtr)) : pixelSize;
}

void TextRenderer::clear() {
    for (auto &[key, entry] : cache_) {
        (void)key;
        if (entry.texture) {
            SDL_DestroyTexture(entry.texture);
        }
    }
    cache_.clear();
}

} // namespace revdock
