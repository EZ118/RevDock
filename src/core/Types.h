#pragma once

#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_rect.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace revdock {

constexpr float kPi = 3.14159265358979323846f;

inline float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(v, hi));
}

inline double clampd(double v, double lo, double hi) {
    return std::max(lo, std::min(v, hi));
}

inline float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

inline float easeInOut(float t) {
    t = clampf(t, 0.0f, 1.0f);
    return 0.5f - 0.5f * std::cos(t * kPi);
}

inline SDL_FRect insetRect(const SDL_FRect &r, float dx, float dy) {
    return SDL_FRect{r.x + dx, r.y + dy, std::max(0.0f, r.w - 2 * dx), std::max(0.0f, r.h - 2 * dy)};
}

inline bool contains(const SDL_FRect &r, float x, float y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

inline SDL_Color rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return SDL_Color{r, g, b, a};
}

inline SDL_Color withAlpha(SDL_Color c, uint8_t a) {
    c.a = a;
    return c;
}

inline std::string lowerAscii(std::string s) {
    for (char &c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

} // namespace revdock
