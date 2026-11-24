#include "render/IconCache.h"

#include "apps/AppIndex.h"
#include "core/Blur.h"
#include "core/Types.h"
#include "platform/Platform.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <numeric>

namespace revdock {
namespace {

// ---------------------------------------------------------------------------
// Small RGBA image used by the icon converter
// ---------------------------------------------------------------------------

struct Image {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> px;

    Image() = default;
    Image(int width, int height) : w(width), h(height), px(static_cast<size_t>(width) * height * 4, 0) {}

    bool empty() const { return w <= 0 || h <= 0 || px.size() < static_cast<size_t>(w) * h * 4; }
    uint8_t *at(int x, int y) { return px.data() + (static_cast<size_t>(y) * w + x) * 4; }
    const uint8_t *at(int x, int y) const {
        return px.data() + (static_cast<size_t>(y) * w + x) * 4;
    }
};

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool valid() const { return w > 0 && h > 0; }
};

struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;
};

Image imageFromSurface(SDL_Surface *surface) {
    Image image;
    if (!surface) {
        return image;
    }
    SDL_Surface *rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
    if (!rgba) {
        return image;
    }
    image.w = rgba->w;
    image.h = rgba->h;
    image.px.resize(static_cast<size_t>(rgba->w) * rgba->h * 4);
    SDL_LockSurface(rgba);
    for (int y = 0; y < rgba->h; ++y) {
        std::memcpy(image.px.data() + static_cast<size_t>(y) * rgba->w * 4,
                    static_cast<const uint8_t *>(rgba->pixels) +
                        static_cast<size_t>(y) * rgba->pitch,
                    static_cast<size_t>(rgba->w) * 4);
    }
    SDL_UnlockSurface(rgba);
    SDL_DestroySurface(rgba);
    return image;
}

SDL_Surface *surfaceFromImage(const Image &image) {
    if (image.empty()) {
        return nullptr;
    }
    SDL_Surface *surface = SDL_CreateSurface(image.w, image.h, SDL_PIXELFORMAT_RGBA32);
    if (!surface) {
        return nullptr;
    }
    SDL_LockSurface(surface);
    for (int y = 0; y < image.h; ++y) {
        std::memcpy(static_cast<uint8_t *>(surface->pixels) +
                        static_cast<size_t>(y) * surface->pitch,
                    image.px.data() + static_cast<size_t>(y) * image.w * 4,
                    static_cast<size_t>(image.w) * 4);
    }
    SDL_UnlockSurface(surface);
    return surface;
}

Image resizedToFit(const Image &src, int maxSide) {
    if (src.empty() || std::max(src.w, src.h) <= maxSide) {
        return src;
    }
    const float scale = static_cast<float>(maxSide) / static_cast<float>(std::max(src.w, src.h));
    const int w = std::max(1, static_cast<int>(std::lround(src.w * scale)));
    const int h = std::max(1, static_cast<int>(std::lround(src.h * scale)));
    Image out(w, h);
    for (int y = 0; y < h; ++y) {
        const float sy = (static_cast<float>(y) + 0.5f) * src.h / h - 0.5f;
        const int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, src.h - 1);
        const int y1 = std::min(y0 + 1, src.h - 1);
        const float fy = std::clamp(sy - std::floor(sy), 0.0f, 1.0f);
        for (int x = 0; x < w; ++x) {
            const float sx = (static_cast<float>(x) + 0.5f) * src.w / w - 0.5f;
            const int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, src.w - 1);
            const int x1 = std::min(x0 + 1, src.w - 1);
            const float fx = std::clamp(sx - std::floor(sx), 0.0f, 1.0f);
            for (int c = 0; c < 4; ++c) {
                const float v00 = src.at(x0, y0)[c];
                const float v10 = src.at(x1, y0)[c];
                const float v01 = src.at(x0, y1)[c];
                const float v11 = src.at(x1, y1)[c];
                const float value = (v00 * (1 - fx) + v10 * fx) * (1 - fy) +
                                    (v01 * (1 - fx) + v11 * fx) * fy;
                out.at(x, y)[c] = static_cast<uint8_t>(std::clamp(value, 0.0f, 255.0f));
            }
        }
    }
    return out;
}

void compositePixel(Image &dst, int x, int y, float r, float g, float b, float a) {
    if (x < 0 || y < 0 || x >= dst.w || y >= dst.h) {
        return;
    }
    uint8_t *p = dst.at(x, y);
    const float da = p[3] / 255.0f;
    const float oa = a + da * (1.0f - a);
    if (oa <= 0.0001f) {
        p[0] = p[1] = p[2] = p[3] = 0;
        return;
    }
    p[0] = static_cast<uint8_t>(std::clamp((r * a + (p[0] / 255.0f) * da * (1.0f - a)) / oa * 255.0f, 0.0f, 255.0f));
    p[1] = static_cast<uint8_t>(std::clamp((g * a + (p[1] / 255.0f) * da * (1.0f - a)) / oa * 255.0f, 0.0f, 255.0f));
    p[2] = static_cast<uint8_t>(std::clamp((b * a + (p[2] / 255.0f) * da * (1.0f - a)) / oa * 255.0f, 0.0f, 255.0f));
    p[3] = static_cast<uint8_t>(std::clamp(oa * 255.0f, 0.0f, 255.0f));
}

// Draws a source rectangle into a destination rectangle with bilinear
// filtering and straight-alpha compositing.
void drawScaled(Image &dst, const Image &src, Rect srcRect, float dx, float dy, float dw, float dh) {
    if (src.empty() || !srcRect.valid() || dw <= 0.0f || dh <= 0.0f) {
        return;
    }
    const int x0 = std::max(0, static_cast<int>(std::floor(dx)));
    const int y0 = std::max(0, static_cast<int>(std::floor(dy)));
    const int x1 = std::min(dst.w, static_cast<int>(std::ceil(dx + dw)));
    const int y1 = std::min(dst.h, static_cast<int>(std::ceil(dy + dh)));
    for (int y = y0; y < y1; ++y) {
        const float v = (static_cast<float>(y) + 0.5f - dy) / dh;
        if (v < 0.0f || v >= 1.0f) {
            continue;
        }
        const float sy = srcRect.y + v * srcRect.h - 0.5f;
        const int iy0 = std::clamp(static_cast<int>(std::floor(sy)), 0, src.h - 1);
        const int iy1 = std::min(iy0 + 1, src.h - 1);
        const float fy = std::clamp(sy - std::floor(sy), 0.0f, 1.0f);
        for (int x = x0; x < x1; ++x) {
            const float u = (static_cast<float>(x) + 0.5f - dx) / dw;
            if (u < 0.0f || u >= 1.0f) {
                continue;
            }
            const float sx = srcRect.x + u * srcRect.w - 0.5f;
            const int ix0 = std::clamp(static_cast<int>(std::floor(sx)), 0, src.w - 1);
            const int ix1 = std::min(ix0 + 1, src.w - 1);
            const float fx = std::clamp(sx - std::floor(sx), 0.0f, 1.0f);
            float value[4];
            for (int c = 0; c < 4; ++c) {
                const float v00 = src.at(ix0, iy0)[c];
                const float v10 = src.at(ix1, iy0)[c];
                const float v01 = src.at(ix0, iy1)[c];
                const float v11 = src.at(ix1, iy1)[c];
                value[c] = (v00 * (1 - fx) + v10 * fx) * (1 - fy) +
                           (v01 * (1 - fx) + v11 * fx) * fy;
            }
            const float a = value[3] / 255.0f;
            if (a <= 0.0f) {
                continue;
            }
            compositePixel(dst, x, y, value[0] / 255.0f, value[1] / 255.0f, value[2] / 255.0f, a);
        }
    }
}

void fillRect(Image &dst, int x, int y, int w, int h, Rgb color) {
    for (int py = std::max(0, y); py < std::min(dst.h, y + h); ++py) {
        for (int px = std::max(0, x); px < std::min(dst.w, x + w); ++px) {
            uint8_t *p = dst.at(px, py);
            p[0] = static_cast<uint8_t>(color.r);
            p[1] = static_cast<uint8_t>(color.g);
            p[2] = static_cast<uint8_t>(color.b);
            p[3] = 255;
        }
    }
}

void fillGradient(Image &dst, int x, int y, int w, int h, Rgb top, Rgb bottom) {
    for (int py = std::max(0, y); py < std::min(dst.h, y + h); ++py) {
        const float t = h > 1 ? static_cast<float>(py - y) / static_cast<float>(h - 1) : 0.0f;
        const Rgb color{static_cast<int>(std::lround(top.r + (bottom.r - top.r) * t)),
                        static_cast<int>(std::lround(top.g + (bottom.g - top.g) * t)),
                        static_cast<int>(std::lround(top.b + (bottom.b - top.b) * t))};
        for (int px = std::max(0, x); px < std::min(dst.w, x + w); ++px) {
            uint8_t *p = dst.at(px, py);
            p[0] = static_cast<uint8_t>(color.r);
            p[1] = static_cast<uint8_t>(color.g);
            p[2] = static_cast<uint8_t>(color.b);
            p[3] = 255;
        }
    }
}

// ---------------------------------------------------------------------------
// Colour helpers (same formulas as the reference converter)
// ---------------------------------------------------------------------------

void rgbToHsl(int r, int g, int b, float &h, float &s, float &l) {
    const float rf = r / 255.0f;
    const float gf = g / 255.0f;
    const float bf = b / 255.0f;
    const float maxv = std::max({rf, gf, bf});
    const float minv = std::min({rf, gf, bf});
    l = (maxv + minv) * 0.5f;
    s = 0.0f;
    h = 0.0f;
    if (maxv != minv) {
        const float d = maxv - minv;
        s = l > 0.5f ? d / (2.0f - maxv - minv) : d / (maxv + minv);
        if (maxv == rf) {
            h = ((gf - bf) / d + (gf < bf ? 6.0f : 0.0f)) / 6.0f;
        } else if (maxv == gf) {
            h = ((bf - rf) / d + 2.0f) / 6.0f;
        } else {
            h = ((rf - gf) / d + 4.0f) / 6.0f;
        }
    }
    h *= 360.0f;
    s *= 100.0f;
    l *= 100.0f;
}

float hue2rgb(float p, float q, float t) {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

Rgb hslToRgb(float h, float s, float l) {
    h = std::fmod(std::fmod(h, 360.0f) + 360.0f, 360.0f) / 360.0f;
    s /= 100.0f;
    l /= 100.0f;
    if (s <= 0.0f) {
        const int v = static_cast<int>(std::lround(l * 255.0f));
        return Rgb{v, v, v};
    }
    const float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
    const float p = 2.0f * l - q;
    return Rgb{static_cast<int>(std::lround(hue2rgb(p, q, h + 1.0f / 3.0f) * 255.0f)),
               static_cast<int>(std::lround(hue2rgb(p, q, h) * 255.0f)),
               static_cast<int>(std::lround(hue2rgb(p, q, h - 1.0f / 3.0f) * 255.0f))};
}

void computeBackground(Rgb dominant, Rgb &top, Rgb &bottom) {
    float h = 0.0f;
    float s = 0.0f;
    float l = 0.0f;
    rgbToHsl(dominant.r, dominant.g, dominant.b, h, s, l);
    // Near-white artwork keeps a neutral light gradient instead of turning blue.
    if (l > 92.0f && s < 8.0f) {
        top = hslToRgb(0.0f, 0.0f, 88.0f);
        bottom = hslToRgb(0.0f, 0.0f, 72.0f);
        return;
    }
    // Near-black artwork keeps a neutral dark gradient.
    if (l < 12.0f) {
        top = hslToRgb(0.0f, 0.0f, 38.0f);
        bottom = hslToRgb(0.0f, 0.0f, 22.0f);
        return;
    }
    if (s < 12.0f) {
        h = 212.0f;
        s = 85.0f;
        l = 55.0f;
    }
    float topL = 0.0f;
    float botL = 0.0f;
    float topS = 0.0f;
    float botS = 0.0f;
    if (l > 85.0f) {
        topS = s * 0.60f;
        botS = s * 0.75f;
        topL = 52.0f;
        botL = 36.0f;
    } else if (l > 65.0f) {
        topS = s * 0.55f;
        botS = s * 0.70f;
        topL = 86.0f;
        botL = 70.0f;
    } else {
        topS = s * 0.50f;
        botS = s * 0.65f;
        topL = 90.0f;
        botL = 74.0f;
    }
    top = hslToRgb(h, topS, topL);
    bottom = hslToRgb(h, botS, botL);
}

// ---------------------------------------------------------------------------
// Source analysis
// ---------------------------------------------------------------------------

Image trimTransparent(const Image &src) {
    int minX = src.w;
    int minY = src.h;
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < src.h; ++y) {
        for (int x = 0; x < src.w; ++x) {
            if (src.at(x, y)[3] > 110) {
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
        }
    }
    if (maxX < 0 || (minX == 0 && minY == 0 && maxX == src.w - 1 && maxY == src.h - 1)) {
        return src;
    }
    Image out(maxX - minX + 1, maxY - minY + 1);
    for (int y = 0; y < out.h; ++y) {
        std::memcpy(out.px.data() + static_cast<size_t>(y) * out.w * 4,
                    src.at(minX, minY + y), static_cast<size_t>(out.w) * 4);
    }
    return out;
}

Rect alphaBounds(const Image &src) {
    int minX = src.w;
    int minY = src.h;
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < src.h; ++y) {
        for (int x = 0; x < src.w; ++x) {
            if (src.at(x, y)[3] >= 110) {
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
        }
    }
    if (maxX < 0) {
        return Rect{};
    }
    return Rect{minX, minY, maxX - minX + 1, maxY - minY + 1};
}

struct Background {
    enum Type { None, Solid, Complex } type = Complex;
    Rgb color;
};

Background analyzeBackground(const Image &src) {
    const int pad = std::max(2, static_cast<int>(std::lround(std::min(src.w, src.h) * 0.04)));
    std::vector<Rgb> samples;
    int transparent = 0;
    int total = 0;
    auto sample = [&](int x, int y) {
        ++total;
        const uint8_t *p = src.at(x, y);
        if (p[3] < 24) {
            ++transparent;
            return;
        }
        samples.push_back(Rgb{p[0], p[1], p[2]});
    };
    for (int x = 0; x < src.w; ++x) {
        for (int d = 0; d < pad; ++d) {
            sample(x, d);
            sample(x, src.h - 1 - d);
        }
    }
    for (int y = 0; y < src.h; ++y) {
        for (int d = 0; d < pad; ++d) {
            sample(d, y);
            sample(src.w - 1 - d, y);
        }
    }
    if (total <= 0 || static_cast<float>(transparent) / total > 0.5f || samples.size() < 8) {
        return Background{Background::None, Rgb{}};
    }
    std::vector<int> rs;
    std::vector<int> gs;
    std::vector<int> bs;
    rs.reserve(samples.size());
    gs.reserve(samples.size());
    bs.reserve(samples.size());
    for (const auto &s : samples) {
        rs.push_back(s.r);
        gs.push_back(s.g);
        bs.push_back(s.b);
    }
    std::sort(rs.begin(), rs.end());
    std::sort(gs.begin(), gs.end());
    std::sort(bs.begin(), bs.end());
    const size_t mid = samples.size() / 2;
    const Rgb median{rs[mid], gs[mid], bs[mid]};
    double distance = 0.0;
    for (const auto &s : samples) {
        const double dr = s.r - median.r;
        const double dg = s.g - median.g;
        const double db = s.b - median.b;
        distance += std::sqrt(dr * dr + dg * dg + db * db);
    }
    distance /= static_cast<double>(samples.size());
    return Background{distance < 22.0 ? Background::Solid : Background::Complex, median};
}

Rgb dominantColor(const Image &src, const Rgb *bg) {
    struct Bucket {
        int count = 0;
        double r = 0;
        double g = 0;
        double b = 0;
    };
    std::map<int, Bucket> buckets;
    const int step = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(src.w) * src.h) / 200.0));
    for (int y = 0; y < src.h; y += step) {
        for (int x = 0; x < src.w; x += step) {
            const uint8_t *p = src.at(x, y);
            if (p[3] < 128) {
                continue;
            }
            if (bg) {
                const int dr = p[0] - bg->r;
                const int dg = p[1] - bg->g;
                const int db = p[2] - bg->b;
                if (dr * dr + dg * dg + db * db < 1024) {
                    continue;
                }
            }
            const int key = ((p[0] >> 4) << 8) | ((p[1] >> 4) << 4) | (p[2] >> 4);
            Bucket &bucket = buckets[key];
            ++bucket.count;
            bucket.r += p[0];
            bucket.g += p[1];
            bucket.b += p[2];
        }
    }
    if (buckets.empty()) {
        return Rgb{10, 132, 255};
    }
    Rgb best{10, 132, 255};
    double bestScore = -1.0;
    for (const auto &[key, bucket] : buckets) {
        (void)key;
        const Rgb color{static_cast<int>(std::lround(bucket.r / bucket.count)),
                        static_cast<int>(std::lround(bucket.g / bucket.count)),
                        static_cast<int>(std::lround(bucket.b / bucket.count))};
        float h = 0.0f;
        float s = 0.0f;
        float l = 0.0f;
        rgbToHsl(color.r, color.g, color.b, h, s, l);
        const double lightnessFactor = 1.0 - std::abs(l - 50.0) / 100.0;
        const double score = bucket.count * (0.3 + s / 100.0) * (0.3 + lightnessFactor);
        if (score > bestScore) {
            bestScore = score;
            best = color;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Foreground placement with a soft shadow
// ---------------------------------------------------------------------------

void drawForeground(Image &canvas, const Image &foreground, Rect box, float offset, float shape,
                    float ratio, float shadowAlpha) {
    if (!box.valid()) {
        return;
    }
    const float target = shape * ratio;
    const float scale = target / static_cast<float>(std::max(box.w, box.h));
    const float dw = box.w * scale;
    const float dh = box.h * scale;
    const float dx = offset + (shape - dw) * 0.5f;
    const float dy = offset + (shape - dh) * 0.5f;

    if (shadowAlpha > 0.0f) {
        Image silhouette(canvas.w, canvas.h);
        drawScaled(silhouette, foreground, box, dx, dy + shape * 0.010f, dw, dh);
        for (size_t i = 0; i + 3 < silhouette.px.size(); i += 4) {
            silhouette.px[i + 0] = 0;
            silhouette.px[i + 1] = 0;
            silhouette.px[i + 2] = 0;
        }
        boxBlurRGBA(silhouette.px, silhouette.w, silhouette.h,
                    std::max(1, static_cast<int>(std::lround(shape * 0.035f))), 2);
        for (int y = 0; y < canvas.h; ++y) {
            for (int x = 0; x < canvas.w; ++x) {
                const uint8_t alpha = silhouette.at(x, y)[3];
                if (alpha == 0) {
                    continue;
                }
                compositePixel(canvas, x, y, 0.0f, 0.0f, 0.0f,
                               alpha / 255.0f * shadowAlpha);
            }
        }
    }
    drawScaled(canvas, foreground, box, dx, dy, dw, dh);
}

// ---------------------------------------------------------------------------
// Shape mask + baked drop shadow
// ---------------------------------------------------------------------------

struct BakedShape {
    int size = 0;
    std::vector<uint8_t> coverage;
    std::vector<uint8_t> shadow;
};

constexpr float kShapeRatio = 0.805f;
// Standard circular corner radius (relative to the icon tile), not the
// reverse-engineered superellipse/squircle.
constexpr float kCornerRatio = 0.2237f;

BakedShape buildShapeMask(int size) {
    BakedShape mask;
    mask.size = size;
    mask.coverage.assign(static_cast<size_t>(size) * size, 0);
    const float shape = size * kShapeRatio;
    const float offset = (size - shape) * 0.5f;
    const float half = shape * 0.5f;
    const float cx = offset + half;
    const float cy = offset + half;
    const float radius = shape * kCornerRatio;
    const float innerHalf = half - radius;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            // Analytic signed-distance coverage: a smooth 1 px antialiased
            // edge (2x2 supersampling left visible facets on small icons).
            const float px = x + 0.5f;
            const float py = y + 0.5f;
            const float qx = std::abs(px - cx) - innerHalf;
            const float qy = std::abs(py - cy) - innerHalf;
            const float outside =
                std::sqrt(std::max(qx, 0.0f) * std::max(qx, 0.0f) +
                          std::max(qy, 0.0f) * std::max(qy, 0.0f));
            const float inside = std::min(std::max(qx, qy), 0.0f);
            const float distance = outside + inside - radius;
            const float coverage = std::clamp(0.5f - distance, 0.0f, 1.0f);
            mask.coverage[static_cast<size_t>(y) * size + x] =
                static_cast<uint8_t>(std::lround(coverage * 255.0f));
        }
    }
    std::vector<uint8_t> blur(static_cast<size_t>(size) * size * 4, 0);
    for (size_t i = 0; i < mask.coverage.size(); ++i) {
        blur[i * 4 + 3] = mask.coverage[i];
    }
    boxBlurRGBA(blur, size, size, std::max(1, static_cast<int>(std::lround(size * 0.02f))), 2);
    mask.shadow.resize(mask.coverage.size());
    for (size_t i = 0; i < mask.coverage.size(); ++i) {
        mask.shadow[i] = blur[i * 4 + 3];
    }
    return mask;
}

// Removes the background colour only where it is connected to the border, so
// white areas that belong to the artwork (a page, a logo cut-out) survive.
Image removeBorderBackground(const Image &src, Rgb bg, int tolerance) {
    Image out = src;
    const int w = out.w;
    const int h = out.h;
    if (w <= 0 || h <= 0) {
        return out;
    }
    std::vector<uint8_t> seen(static_cast<size_t>(w) * h, 0);
    std::vector<int> stack;
    const int limit = tolerance * tolerance;
    auto removable = [&](int index) {
        const uint8_t *p = out.px.data() + static_cast<size_t>(index) * 4;
        if (p[3] == 0) {
            return true;
        }
        const int dr = static_cast<int>(p[0]) - bg.r;
        const int dg = static_cast<int>(p[1]) - bg.g;
        const int db = static_cast<int>(p[2]) - bg.b;
        return dr * dr + dg * dg + db * db <= limit;
    };
    auto push = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= w || y >= h) {
            return;
        }
        const int index = y * w + x;
        if (!seen[index] && removable(index)) {
            seen[index] = 1;
            stack.push_back(index);
        }
    };
    for (int x = 0; x < w; ++x) {
        push(x, 0);
        push(x, h - 1);
    }
    for (int y = 0; y < h; ++y) {
        push(0, y);
        push(w - 1, y);
    }
    while (!stack.empty()) {
        const int index = stack.back();
        stack.pop_back();
        out.px[static_cast<size_t>(index) * 4 + 3] = 0;
        const int x = index % w;
        const int y = index / w;
        push(x - 1, y);
        push(x + 1, y);
        push(x, y - 1);
        push(x, y + 1);
    }
    return out;
}

// Unsharp mask in premultiplied space: recovers detail from icons that the
// shell only provides as a small (32 px) bitmap scaled up to the working size.
void sharpenImage(Image &image, float amount) {
    if (image.empty() || amount <= 0.0f) {
        return;
    }
    std::vector<uint8_t> premul(image.px.size(), 0);
    for (size_t i = 0; i + 3 < image.px.size(); i += 4) {
        const int a = image.px[i + 3];
        premul[i + 0] = static_cast<uint8_t>(image.px[i + 0] * a / 255);
        premul[i + 1] = static_cast<uint8_t>(image.px[i + 1] * a / 255);
        premul[i + 2] = static_cast<uint8_t>(image.px[i + 2] * a / 255);
        premul[i + 3] = static_cast<uint8_t>(a);
    }
    std::vector<uint8_t> blurred = premul;
    boxBlurRGBA(blurred, image.w, image.h, 1, 2);
    for (size_t i = 0; i + 3 < image.px.size(); i += 4) {
        const int a = image.px[i + 3];
        if (a < 8) {
            continue;
        }
        for (int c = 0; c < 3; ++c) {
            const float value = static_cast<float>(premul[i + c]) +
                                amount * (static_cast<float>(premul[i + c]) -
                                          static_cast<float>(blurred[i + c]));
            const float unpremultiplied = value * 255.0f / static_cast<float>(a);
            image.px[i + c] =
                static_cast<uint8_t>(std::clamp(unpremultiplied, 0.0f, 255.0f));
        }
    }
}

size_t opaquePixels(const Image &image, int threshold) {
    size_t count = 0;
    for (size_t i = 3; i < image.px.size(); i += 4) {
        if (image.px[i] >= threshold) {
            ++count;
        }
    }
    return count;
}

bool isNeutralBackground(Rgb color) {
    float h = 0.0f;
    float s = 0.0f;
    float l = 0.0f;
    rgbToHsl(color.r, color.g, color.b, h, s, l);
    return (l > 90.0f && s < 10.0f) || l < 14.0f;
}

float aspectDifference(const Image &image) {
    if (image.w <= 0 || image.h <= 0) {
        return 0.0f;
    }
    return std::abs(image.w - image.h) / static_cast<float>(std::max(image.w, image.h));
}

Image renderMacIcon(const Image &source, int outSize, const std::vector<uint8_t> &coverageMask,
                    const std::vector<uint8_t> &shadowMask) {
    const Image trimmed = trimTransparent(source);
    if (trimmed.empty()) {
        return Image{};
    }
    const float shape = outSize * kShapeRatio;
    const float offset = (outSize - shape) * 0.5f;
    const int rectX = static_cast<int>(std::floor(offset));
    const int rectSize = static_cast<int>(std::ceil(shape));

    Image content(outSize, outSize);
    enum class Style { Gradient, Solid, Cover, Contain } style = Style::Cover;
    Image art = trimmed;
    Rect artBox = alphaBounds(trimmed);
    Rgb solidColor;
    Rgb gradientSource;

    // Tiny sources are upscaled as-is; analysing them produces noise.
    if (std::min(trimmed.w, trimmed.h) < 64) {
        style = Style::Cover;
    } else {
        const Background background = analyzeBackground(trimmed);
        if (background.type == Background::None) {
            style = Style::Gradient;
            gradientSource = dominantColor(trimmed, nullptr);
        } else if (background.type == Background::Solid) {
            Image cut = removeBorderBackground(trimmed, background.color, 30);
            const Rect box = alphaBounds(cut);
            if (!box.valid()) {
                style = Style::Cover;
            } else if (isNeutralBackground(background.color)) {
                // White/black plates (shell icons with a matte, blank pages) are
                // not a real macOS backdrop. Dense artwork that covers the plate
                // is kept whole; thin artwork (a page outline, a logo) is lifted
                // onto a neutral gradient so it is not lost in the plate.
                const size_t filled = opaquePixels(cut, 110);
                const size_t total = static_cast<size_t>(trimmed.w) * trimmed.h;
                if (filled * 2 > total) {
                    style = Style::Cover;
                } else {
                    style = Style::Gradient;
                    gradientSource = dominantColor(trimmed, nullptr);
                    art = std::move(cut);
                    artBox = box;
                }
            } else {
                const long area = static_cast<long>(box.w) * box.h;
                const long total = static_cast<long>(trimmed.w) * trimmed.h;
                const bool nearSquare =
                    std::abs(box.w - box.h) / static_cast<float>(std::max(box.w, box.h)) < 0.18f;
                if (area > total * 85 / 100 && nearSquare) {
                    // Dense, full-bleed artwork already reads as an icon tile;
                    // keep it edge to edge.
                    style = Style::Cover;
                } else {
                    // A solid plate with a distinct central body (most UWP
                    // icons) becomes a macOS-style tile: keep the plate colour
                    // and scale the body to a centred, inset size with clear
                    // margins from the rounded border.
                    style = Style::Solid;
                    solidColor = background.color;
                    art = std::move(cut);
                    artBox = box;
                }
            }
        } else {
            style = Style::Cover;
        }
    }

    switch (style) {
    case Style::Gradient: {
        Rgb top;
        Rgb bottom;
        computeBackground(gradientSource, top, bottom);
        fillGradient(content, rectX, rectX, rectSize, rectSize, top, bottom);
        drawForeground(content, art, artBox, offset, shape, 0.76f, 0.20f);
        break;
    }
    case Style::Solid:
        fillRect(content, rectX, rectX, rectSize, rectSize, solidColor);
        drawForeground(content, art, artBox, offset, shape, 0.82f, 0.12f);
        break;
    case Style::Contain: {
        Rgb top;
        Rgb bottom;
        computeBackground(gradientSource, top, bottom);
        fillGradient(content, rectX, rectX, rectSize, rectSize, top, bottom);
        drawForeground(content, trimmed, artBox, offset, shape, 0.86f, 0.0f);
        break;
    }
    case Style::Cover: {
        // Cover-cropping is only acceptable for near-square artwork. Wide or tall
        // artwork is scaled down to fit instead of losing its edges.
        if (aspectDifference(trimmed) > 0.12f) {
            gradientSource = dominantColor(trimmed, nullptr);
            Rgb top;
            Rgb bottom;
            computeBackground(gradientSource, top, bottom);
            fillGradient(content, rectX, rectX, rectSize, rectSize, top, bottom);
            drawForeground(content, trimmed, artBox, offset, shape, 0.86f, 0.0f);
        } else {
            const float coverScale = std::max(shape / static_cast<float>(trimmed.w),
                                              shape / static_cast<float>(trimmed.h));
            const int srcW = std::max(1, static_cast<int>(std::lround(shape / coverScale)));
            const int srcH = std::max(1, static_cast<int>(std::lround(shape / coverScale)));
            const int srcX = (trimmed.w - srcW) / 2;
            const int srcY = (trimmed.h - srcH) / 2;
            drawScaled(content, trimmed, Rect{srcX, srcY, srcW, srcH}, offset, offset, shape,
                       shape);
        }
        break;
    }
    }

    // Mask the content and add the baked drop shadow.
    Image out(outSize, outSize);
    const int shadowOffset = std::max(1, static_cast<int>(std::lround(outSize * 0.010f)));
    for (int y = 0; y < outSize; ++y) {
        for (int x = 0; x < outSize; ++x) {
            const size_t index = static_cast<size_t>(y) * outSize + x;
            const int shadowY = y - shadowOffset;
            if (shadowY >= 0) {
                const uint8_t alpha = shadowMask[static_cast<size_t>(shadowY) * outSize + x];
                if (alpha > 0) {
                    compositePixel(out, x, y, 0.0f, 0.0f, 0.0f, alpha / 255.0f * 0.30f);
                }
            }
            const uint8_t coverage = coverageMask[index];
            if (coverage == 0) {
                continue;
            }
            const uint8_t *p = content.at(x, y);
            if (p[3] == 0) {
                continue;
            }
            const float alpha = (p[3] / 255.0f) * (coverage / 255.0f);
            compositePixel(out, x, y, p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, alpha);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Fallback (non-macOS) sanitiser
// ---------------------------------------------------------------------------

// Plain files, folders and non-app shortcuts keep the shell's original icon:
// trim transparent padding but do not round, square-crop or recolour it.
SDL_Surface *sanitizePlain(SDL_Surface *input) {
    Image image = trimTransparent(imageFromSurface(input));
    SDL_DestroySurface(input);
    if (image.empty()) {
        return nullptr;
    }
    return surfaceFromImage(image);
}

} // namespace

// ---------------------------------------------------------------------------
// IconCache
// ---------------------------------------------------------------------------

IconCache::IconCache(SDL_Renderer *renderer, Platform &platform, AppIndex &apps, float contentScale)
    : renderer_(renderer), platform_(platform), apps_(apps), scale_(contentScale > 0 ? contentScale : 1.0f) {}

void IconCache::setMacStyle(bool enabled) {
    if (macStyle_ == enabled) {
        return;
    }
    macStyle_ = enabled;
    clear();
}

const IconCache::ShapeMask &IconCache::shapeMaskFor(int size) {
    auto found = masks_.find(size);
    if (found != masks_.end()) {
        return found->second;
    }
    if (masks_.size() >= 10) {
        masks_.clear();
    }
    BakedShape baked = buildShapeMask(size);
    ShapeMask &slot = masks_[size];
    slot.size = baked.size;
    slot.coverage = std::move(baked.coverage);
    slot.shadow = std::move(baked.shadow);
    return slot;
}

SDL_Texture *IconCache::textureFor(const DockItem &item, int logicalSize) {
    if (item.isSpecial() || item.type == ItemType::MinimizedWindow) {
        return nullptr;
    }
    int pixelSize = std::max(16, static_cast<int>(std::lround(logicalSize * scale_)));
    if (pixelSize >= 40) {
        pixelSize = ((pixelSize + 7) / 8) * 8;
    }
    const std::string key = item.id + "#" + std::to_string(pixelSize);
    auto found = cache_.find(key);
    if (found != cache_.end()) {
        return found->second.texture;
    }

    const AppInfo *app = nullptr;
    if (item.isApplication()) {
        app = apps_.findById(item.id);
        if (!app && !item.path.empty()) {
            app = apps_.findByPath(item.path);
        }
    }
    // Only applications get the macOS rounded-square treatment; documents
    // and other plain files keep their original shell icon.
    return load(key, item.path, app, pixelSize, item.isApplication());
}

SDL_Texture *IconCache::textureForApp(const AppInfo &app, int logicalSize) {
    int pixelSize = std::max(16, static_cast<int>(std::lround(logicalSize * scale_)));
    if (pixelSize >= 40) {
        pixelSize = ((pixelSize + 7) / 8) * 8;
    }
    const std::string key = app.id + "#" + std::to_string(pixelSize);
    auto found = cache_.find(key);
    if (found != cache_.end()) {
        return found->second.texture;
    }
    return load(key, app.sourcePath, &app, pixelSize, true);
}

SDL_Texture *IconCache::shapeTexture(int logicalSize, SDL_Color body) {
    int pixelSize = std::max(16, static_cast<int>(std::lround(logicalSize * scale_)));
    if (pixelSize >= 40) {
        pixelSize = ((pixelSize + 7) / 8) * 8;
    }
    const std::string key = "shape#" + std::to_string(pixelSize) + "#" +
                            std::to_string(body.r) + "," + std::to_string(body.g) +
                            "," + std::to_string(body.b) + "," +
                            std::to_string(body.a);
    auto found = cache_.find(key);
    if (found != cache_.end()) {
        return found->second.texture;
    }
    const ShapeMask &mask = shapeMaskFor(pixelSize);
    // Mirror renderMacIcon's compositing: the same blurred shadow mask and the
    // same 30% shadow strength, then the body colour at the requested alpha.
    Image out(pixelSize, pixelSize);
    const int shadowOffset =
        std::max(1, static_cast<int>(std::lround(pixelSize * 0.010f)));
    const float bodyR = body.r / 255.0f;
    const float bodyG = body.g / 255.0f;
    const float bodyB = body.b / 255.0f;
    const float bodyA = body.a / 255.0f;
    for (int y = 0; y < pixelSize; ++y) {
        for (int x = 0; x < pixelSize; ++x) {
            const size_t index = static_cast<size_t>(y) * pixelSize + x;
            const int shadowY = y - shadowOffset;
            if (shadowY >= 0) {
                const uint8_t alpha =
                    mask.shadow[static_cast<size_t>(shadowY) * pixelSize + x];
                if (alpha > 0) {
                    compositePixel(out, x, y, 0.0f, 0.0f, 0.0f,
                                   alpha / 255.0f * 0.30f);
                }
            }
            const uint8_t coverage = mask.coverage[index];
            if (coverage == 0) {
                continue;
            }
            compositePixel(out, x, y, bodyR, bodyG, bodyB,
                           bodyA * (coverage / 255.0f));
        }
    }
    SDL_Surface *surface = surfaceFromImage(out);
    if (!surface) {
        return nullptr;
    }
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer_, surface);
    SDL_DestroySurface(surface);
    Entry entry;
    entry.pixelSize = pixelSize;
    entry.texture = texture;
    if (texture) {
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    }
    cache_[key] = entry;
    return texture;
}

SDL_Texture *IconCache::load(const std::string &key, const std::string &path, const AppInfo *app,
                             int pixelSize, bool macStyle) {
    const int sourceSize = std::clamp(pixelSize * 2, 128, 256);
    SDL_Surface *surface = nullptr;
    if (app) {
        surface = platform_.loadIcon(*app, sourceSize);
    }
    if (!surface && !path.empty()) {
        surface = platform_.loadIconPath(path, sourceSize);
    }
    if (!surface) {
        Entry entry;
        entry.pixelSize = pixelSize;
        cache_[key] = entry;
        return nullptr;
    }

    SDL_Surface *converted = nullptr;
    if (macStyle && macStyle_ && pixelSize >= 16) {
        const ShapeMask &mask = shapeMaskFor(pixelSize);
        Image source = resizedToFit(imageFromSurface(surface), 256);
        sharpenImage(source, 0.75f);
        SDL_DestroySurface(surface);
        Image result = renderMacIcon(source, pixelSize, mask.coverage, mask.shadow);
        converted = surfaceFromImage(result);
    } else {
        converted = sanitizePlain(surface);
    }

    SDL_Texture *texture = converted ? SDL_CreateTextureFromSurface(renderer_, converted) : nullptr;
    if (texture) {
        // Bilinear filtering keeps the rounded corners smooth when the cached
        // icon is drawn at a fractional/magnified size.
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    }
    if (converted) {
        SDL_DestroySurface(converted);
    }
    Entry entry;
    entry.texture = texture;
    entry.pixelSize = pixelSize;
    cache_[key] = entry;
    return texture;
}

void IconCache::clear() {
    for (auto &[key, entry] : cache_) {
        (void)key;
        if (entry.texture) {
            SDL_DestroyTexture(entry.texture);
        }
    }
    cache_.clear();
}

} // namespace revdock
