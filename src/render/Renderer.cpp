#include "render/Renderer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace revdock {
namespace {

float cornerCoverage(float x, float y, float radius) {
    // 4x4 supersampling for a smooth quarter circle.
    int hits = 0;
    for (int sy = 0; sy < 4; ++sy) {
        for (int sx = 0; sx < 4; ++sx) {
            const float px = x + (sx + 0.5f) / 4.0f;
            const float py = y + (sy + 0.5f) / 4.0f;
            if (px * px + py * py <= radius * radius) {
                ++hits;
            }
        }
    }
    return static_cast<float>(hits) / 16.0f;
}

} // namespace

Renderer::Renderer(SDL_Renderer *renderer, float contentScale)
    : renderer_(renderer), scale_(contentScale > 0.0f ? contentScale : 1.0f) {}

Renderer::~Renderer() {
    for (auto &[radius, texture] : maskCache_) {
        (void)radius;
        SDL_DestroyTexture(texture);
    }
    for (auto &[key, texture] : shapeCache_) {
        (void)key;
        SDL_DestroyTexture(texture);
    }
}

void Renderer::beginFrame() {
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
    SDL_RenderClear(renderer_);
}

void Renderer::endFrame() {
    SDL_RenderPresent(renderer_);
}

void Renderer::setDrawColor(SDL_Color color) {
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
}

void Renderer::fillRect(const SDL_FRect &rect, SDL_Color color) {
    color.a = static_cast<uint8_t>(color.a * opacity_);
    setDrawColor(color);
    SDL_RenderFillRect(renderer_, &rect);
}

void Renderer::roundedRect(const SDL_FRect &rect, float radius, SDL_Color color) {
    color.a = static_cast<uint8_t>(color.a * opacity_);
    if (rect.w <= 0.0f || rect.h <= 0.0f || color.a == 0) {
        return;
    }
    // Snap to the device pixel grid: translucent fills with fractional edges
    // read as a 1-2 px soft "border" along the right/bottom of a rounded rect.
    const SDL_FRect aligned{
        std::round(rect.x * scale_) / scale_,
        std::round(rect.y * scale_) / scale_,
        std::round(rect.w * scale_) / scale_,
        std::round(rect.h * scale_) / scale_};
    const int r = std::max(0, static_cast<int>(std::lround(radius * scale_)));
    if (r <= 0) {
        fillRect(aligned, color);
        return;
    }
    const float wDev = aligned.w * scale_;
    const float hDev = aligned.h * scale_;
    if (wDev <= 2.0f * static_cast<float>(r) + 0.5f ||
        hDev <= 2.0f * static_cast<float>(r) + 0.5f) {
        // Small element: bake the whole shape at device resolution so a 4 px
        // dot is still a clean circle instead of a stretched corner mask.
        SDL_Texture *mask = deviceMask(
            std::max(1, static_cast<int>(std::lround(wDev))),
            std::max(1, static_cast<int>(std::lround(hDev))),
            std::max(0, r));
        if (!mask) {
            fillRect(aligned, color);
            return;
        }
        drawMaskSlices(mask, -1, aligned, color);
        return;
    }
    SDL_Texture *mask = cornerMask(r);
    if (!mask) {
        fillRect(aligned, color);
        return;
    }
    drawMaskSlices(mask, r, aligned, color);
}

void Renderer::circle(const SDL_FRect &rect, SDL_Color color) {
    color.a = static_cast<uint8_t>(color.a * opacity_);
    if (rect.w <= 0.0f || rect.h <= 0.0f || color.a == 0) {
        return;
    }
    const int w = std::max(1, static_cast<int>(std::lround(rect.w * scale_)));
    const int h = std::max(1, static_cast<int>(std::lround(rect.h * scale_)));
    SDL_Texture *mask = deviceMask(w, h, -1);
    if (!mask) {
        return;
    }
    // Snap to whole device pixels; a half-pixel offset would blur the AA mask.
    const SDL_FRect snapped{
        std::round(rect.x * scale_) / scale_,
        std::round(rect.y * scale_) / scale_,
        static_cast<float>(w) / scale_,
        static_cast<float>(h) / scale_};
    SDL_SetTextureColorMod(mask, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(mask, color.a);
    SDL_RenderTexture(renderer_, mask, nullptr, &snapped);
}

void Renderer::roundedRectExact(const SDL_FRect &rect, float radius, SDL_Color color) {
    color.a = static_cast<uint8_t>(color.a * opacity_);
    if (rect.w <= 0.0f || rect.h <= 0.0f || color.a == 0) {
        return;
    }
    const SDL_FRect aligned{
        std::round(rect.x * scale_) / scale_,
        std::round(rect.y * scale_) / scale_,
        std::round(rect.w * scale_) / scale_,
        std::round(rect.h * scale_) / scale_};
    const int radiusPx = std::max(0, static_cast<int>(std::lround(radius * scale_)));
    if (radiusPx <= 0) {
        fillRect(aligned, color);
        return;
    }
    SDL_Texture *mask = deviceMask(
        std::max(1, static_cast<int>(std::lround(aligned.w * scale_))),
        std::max(1, static_cast<int>(std::lround(aligned.h * scale_))),
        radiusPx);
    if (!mask) {
        fillRect(aligned, color);
        return;
    }
    drawMaskSlices(mask, -1, aligned, color);
}

void Renderer::thickLine(float x1, float y1, float x2, float y2, float thickness,
                         SDL_Color color) {
    color.a = static_cast<uint8_t>(color.a * opacity_);
    if (thickness <= 0.0f || color.a == 0) {
        return;
    }
    const float dx = x2 - x1;
    const float dy = y2 - y1;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 0.0001f) {
        return;
    }
    const float nx = -dy / length * thickness * 0.5f;
    const float ny = dx / length * thickness * 0.5f;
    const SDL_FColor fc{color.r / 255.0f, color.g / 255.0f, color.b / 255.0f,
                        color.a / 255.0f};
    const SDL_Vertex vertices[4] = {
        {SDL_FPoint{x1 + nx, y1 + ny}, fc, SDL_FPoint{0.0f, 0.0f}},
        {SDL_FPoint{x1 - nx, y1 - ny}, fc, SDL_FPoint{0.0f, 0.0f}},
        {SDL_FPoint{x2 - nx, y2 - ny}, fc, SDL_FPoint{0.0f, 0.0f}},
        {SDL_FPoint{x2 + nx, y2 + ny}, fc, SDL_FPoint{0.0f, 0.0f}},
    };
    const int indices[6] = {0, 1, 2, 0, 2, 3};
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(renderer_, nullptr, vertices, 4, indices, 6);
}

void Renderer::shadow(const SDL_FRect &rect, float radius, SDL_Color color, float spread) {
    for (int i = 2; i >= 1; --i) {
        const float grow = spread * static_cast<float>(i) / 2.0f;
        SDL_Color layer = color;
        layer.a = static_cast<uint8_t>(color.a / (i * 3));
        roundedRect(SDL_FRect{rect.x - grow, rect.y - grow + 1.0f,
                              rect.w + grow * 2.0f, rect.h + grow * 2.0f},
                    radius + grow, layer);
    }
}

void Renderer::texture(SDL_Texture *texture, const SDL_FRect &dst, SDL_Color tint, float rotation,
                       SDL_FPoint center) {
    if (!texture) {
        return;
    }
    tint.a = static_cast<uint8_t>(tint.a * opacity_);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureColorMod(texture, tint.r, tint.g, tint.b);
    SDL_SetTextureAlphaMod(texture, tint.a);
    if (rotation != 0.0f) {
        SDL_RenderTextureRotated(renderer_, texture, nullptr, &dst, rotation, &center,
                                 SDL_FLIP_NONE);
    } else {
        SDL_RenderTexture(renderer_, texture, nullptr, &dst);
    }
}

void Renderer::folderIcon(const SDL_FRect &rect, SDL_Color body, SDL_Color tab) {
    const float radius = rect.w * 0.12f;
    SDL_FRect tabRect{rect.x, rect.y + rect.h * 0.08f, rect.w * 0.46f, rect.h * 0.24f};
    roundedRect(tabRect, radius * 0.7f, tab);
    SDL_FRect bodyRect{rect.x, rect.y + rect.h * 0.20f, rect.w, rect.h * 0.72f};
    roundedRect(bodyRect, radius, body);
    SDL_FRect gloss{bodyRect.x + bodyRect.w * 0.08f, bodyRect.y + bodyRect.h * 0.10f,
                    bodyRect.w * 0.84f, bodyRect.h * 0.18f};
    roundedRect(gloss, radius * 0.5f, rgba(255, 255, 255, 38));
}

void Renderer::trashIcon(const SDL_FRect &rect, SDL_Color body, SDL_Color lid) {
    const float w = rect.w;
    const float h = rect.h;
    const float radius = w * 0.08f;
    roundedRect(SDL_FRect{rect.x + w * 0.22f, rect.y + h * 0.08f, w * 0.56f, h * 0.10f},
                radius, lid);
    roundedRect(SDL_FRect{rect.x + w * 0.38f, rect.y + h * 0.02f, w * 0.24f, h * 0.08f},
                radius * 0.6f, lid);
    roundedRect(SDL_FRect{rect.x + w * 0.20f, rect.y + h * 0.20f, w * 0.60f, h * 0.70f},
                radius, body);
    for (int i = 0; i < 3; ++i) {
        const float x = rect.x + w * (0.32f + static_cast<float>(i) * 0.18f);
        roundedRect(SDL_FRect{x, rect.y + h * 0.30f, w * 0.045f, h * 0.48f}, w * 0.02f,
                    rgba(0, 0, 0, 60));
    }
}

void Renderer::gridIcon(const SDL_FRect &rect, SDL_Color color) {
    const float gap = rect.w * 0.10f;
    const float cell = (rect.w - gap * 4.0f) / 3.0f;
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 3; ++x) {
            roundedRect(SDL_FRect{rect.x + gap + static_cast<float>(x) * (cell + gap),
                                  rect.y + gap + static_cast<float>(y) * (cell + gap),
                                  cell, cell},
                        cell * 0.22f, color);
        }
    }
}

void Renderer::placeholderIcon(const SDL_FRect &rect, SDL_Color body, SDL_Color accent) {
    roundedRect(rect, rect.w * 0.22f, body);
    roundedRect(insetRect(rect, rect.w * 0.16f, rect.w * 0.16f), rect.w * 0.14f, accent);
    SDL_FRect bar{rect.x + rect.w * 0.28f, rect.y + rect.h * 0.30f, rect.w * 0.44f, rect.h * 0.10f};
    roundedRect(bar, bar.h * 0.5f, rgba(255, 255, 255, 150));
    bar.y += rect.h * 0.20f;
    bar.w *= 0.72f;
    roundedRect(bar, bar.h * 0.5f, rgba(255, 255, 255, 110));
}

SDL_Texture *Renderer::deviceMask(int width, int height, float radiusPx) {
    width = std::max(1, width);
    height = std::max(1, height);
    const bool ellipse = radiusPx < 0.0f;
    const int radiusKey = ellipse ? -1
                                  : std::max(0, static_cast<int>(std::lround(radiusPx)));
    const auto key = std::make_tuple(width, height, radiusKey);
    if (auto found = shapeCache_.find(key); found != shapeCache_.end()) {
        return found->second;
    }
    SDL_Surface *surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) {
        return nullptr;
    }
    SDL_LockSurface(surface);
    auto *pixels = static_cast<uint32_t *>(surface->pixels);
    const float halfW = width * 0.5f;
    const float halfH = height * 0.5f;
    const float r = ellipse
                        ? 0.0f
                        : std::min(static_cast<float>(radiusKey),
                                   std::min(halfW, halfH));
    const float innerW = std::max(0.0f, halfW - r);
    const float innerH = std::max(0.0f, halfH - r);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float px = x + 0.5f;
            const float py = y + 0.5f;
            float coverage = 1.0f;
            if (ellipse) {
                // Ellipse inscribed in the rect, anti-aliased by the exact
                // signed distance in pixels.
                const float nx = (px - halfW) / std::max(0.5f, halfW);
                const float ny = (py - halfH) / std::max(0.5f, halfH);
                const float distance =
                    (std::sqrt(nx * nx + ny * ny) - 1.0f) *
                    std::min(halfW, halfH);
                coverage = std::clamp(0.5f - distance, 0.0f, 1.0f);
            } else {
                const float qx = std::abs(px - halfW) - innerW;
                const float qy = std::abs(py - halfH) - innerH;
                const float distance =
                    std::sqrt(std::max(qx, 0.0f) * std::max(qx, 0.0f) +
                              std::max(qy, 0.0f) * std::max(qy, 0.0f)) +
                    std::min(std::max(qx, qy), 0.0f) - r;
                coverage = std::clamp(0.5f - distance, 0.0f, 1.0f);
            }
            const uint8_t alpha = static_cast<uint8_t>(coverage * 255.0f);
            pixels[y * (surface->pitch / 4) + x] =
                SDL_MapSurfaceRGBA(surface, 255, 255, 255, alpha);
        }
    }
    SDL_UnlockSurface(surface);
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer_, surface);
    SDL_DestroySurface(surface);
    if (texture) {
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
        shapeCache_[key] = texture;
    }
    return texture;
}

SDL_Texture *Renderer::cornerMask(int radius) {
    radius = std::max(1, radius);
    auto found = maskCache_.find(radius);
    if (found != maskCache_.end()) {
        return found->second;
    }
    const int size = radius * 2 + 1;
    SDL_Surface *surface = SDL_CreateSurface(size, size, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) {
        return nullptr;
    }
    SDL_LockSurface(surface);
    auto *pixels = static_cast<uint32_t *>(surface->pixels);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float coverage = 1.0f;
            if (x < radius && y < radius) {
                coverage = cornerCoverage(static_cast<float>(radius - x) - 0.5f,
                                          static_cast<float>(radius - y) - 0.5f,
                                          static_cast<float>(radius));
            } else if (x >= radius + 1 && y < radius) {
                coverage = cornerCoverage(static_cast<float>(x - radius) + 0.5f,
                                          static_cast<float>(radius - y) - 0.5f,
                                          static_cast<float>(radius));
            } else if (x < radius && y >= radius + 1) {
                coverage = cornerCoverage(static_cast<float>(radius - x) - 0.5f,
                                          static_cast<float>(y - radius) + 0.5f,
                                          static_cast<float>(radius));
            } else if (x >= radius + 1 && y >= radius + 1) {
                coverage = cornerCoverage(static_cast<float>(x - radius) + 0.5f,
                                          static_cast<float>(y - radius) + 0.5f,
                                          static_cast<float>(radius));
            }
            const uint8_t alpha = static_cast<uint8_t>(coverage * 255.0f);
            pixels[y * (surface->pitch / 4) + x] = SDL_MapSurfaceRGBA(surface, 255, 255, 255, alpha);
        }
    }
    SDL_UnlockSurface(surface);
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer_, surface);
    SDL_DestroySurface(surface);
    if (texture) {
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        maskCache_[radius] = texture;
    }
    return texture;
}

void Renderer::drawMaskSlices(SDL_Texture *mask, int radius, const SDL_FRect &dst, SDL_Color color) {
    if (!mask) {
        return;
    }
    if (radius < 0) {
        // Whole shape baked at device resolution: draw it 1:1.
        SDL_SetTextureColorMod(mask, color.r, color.g, color.b);
        SDL_SetTextureAlphaMod(mask, color.a);
        float textureW = 0.0f;
        float textureH = 0.0f;
        if (!SDL_GetTextureSize(mask, &textureW, &textureH) || textureW <= 0.0f ||
            textureH <= 0.0f) {
            return;
        }
        const SDL_FRect snapped{
            std::round(dst.x * scale_) / scale_,
            std::round(dst.y * scale_) / scale_,
            textureW / scale_, textureH / scale_};
        SDL_RenderTexture(renderer_, mask, nullptr, &snapped);
        return;
    }
    if (radius <= 0) {
        fillRect(dst, color);
        return;
    }
    const int size = radius * 2 + 1;
    // The mask is baked in device pixels, so the destination corner must be
    // radius / scale points.  Using the device radius directly drew corners
    // scale times too large and blurred them (and dots/menu highlights).
    const float r = static_cast<float>(radius) / scale_;
    const float w = dst.w;
    const float h = dst.h;
    if (w <= 2.0f * r || h <= 2.0f * r) {
        SDL_SetTextureColorMod(mask, color.r, color.g, color.b);
        SDL_SetTextureAlphaMod(mask, color.a);
        SDL_RenderTexture(renderer_, mask, nullptr, &dst);
        return;
    }

    SDL_SetTextureColorMod(mask, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(mask, color.a);

    const float rd = static_cast<float>(radius);
    const SDL_FRect srcTL{0.0f, 0.0f, rd, rd};
    const SDL_FRect srcTR{static_cast<float>(size) - rd, 0.0f, rd, rd};
    const SDL_FRect srcBL{0.0f, static_cast<float>(size) - rd, rd, rd};
    const SDL_FRect srcBR{static_cast<float>(size) - rd,
                          static_cast<float>(size) - rd, rd, rd};
    const SDL_FRect srcTop{rd, 0.0f, 1.0f, rd};
    const SDL_FRect srcBottom{rd, static_cast<float>(size) - rd, 1.0f, rd};
    const SDL_FRect srcLeft{0.0f, rd, rd, 1.0f};
    const SDL_FRect srcRight{static_cast<float>(size) - rd, rd, rd, 1.0f};
    const SDL_FRect srcCenter{rd, rd, 1.0f, 1.0f};

    const SDL_FRect dstTL{dst.x, dst.y, r, r};
    const SDL_FRect dstTR{dst.x + w - r, dst.y, r, r};
    const SDL_FRect dstBL{dst.x, dst.y + h - r, r, r};
    const SDL_FRect dstBR{dst.x + w - r, dst.y + h - r, r, r};
    const SDL_FRect dstTop{dst.x + r, dst.y, w - 2.0f * r, r};
    const SDL_FRect dstBottom{dst.x + r, dst.y + h - r, w - 2.0f * r, r};
    const SDL_FRect dstLeft{dst.x, dst.y + r, r, h - 2.0f * r};
    const SDL_FRect dstRight{dst.x + w - r, dst.y + r, r, h - 2.0f * r};
    const SDL_FRect dstCenter{dst.x + r, dst.y + r, w - 2.0f * r, h - 2.0f * r};

    SDL_RenderTexture(renderer_, mask, &srcTL, &dstTL);
    SDL_RenderTexture(renderer_, mask, &srcTR, &dstTR);
    SDL_RenderTexture(renderer_, mask, &srcBL, &dstBL);
    SDL_RenderTexture(renderer_, mask, &srcBR, &dstBR);
    SDL_RenderTexture(renderer_, mask, &srcTop, &dstTop);
    SDL_RenderTexture(renderer_, mask, &srcBottom, &dstBottom);
    SDL_RenderTexture(renderer_, mask, &srcLeft, &dstLeft);
    SDL_RenderTexture(renderer_, mask, &srcRight, &dstRight);
    SDL_RenderTexture(renderer_, mask, &srcCenter, &dstCenter);
}

} // namespace revdock
