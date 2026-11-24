#include "core/Blur.h"

#include <algorithm>
#include <cmath>

namespace revdock {
namespace {

void blurPassHorizontal(const std::vector<uint8_t> &src, std::vector<uint8_t> &dst,
                        int width, int height, int radius) {
    const int window = radius * 2 + 1;
    for (int y = 0; y < height; ++y) {
        int sums[4] = {0, 0, 0, 0};
        for (int i = -radius; i <= radius; ++i) {
            const int x = std::clamp(i, 0, width - 1);
            const size_t index = (static_cast<size_t>(y) * width + x) * 4;
            for (int c = 0; c < 4; ++c) {
                sums[c] += src[index + c];
            }
        }
        for (int x = 0; x < width; ++x) {
            const size_t outIndex = (static_cast<size_t>(y) * width + x) * 4;
            for (int c = 0; c < 4; ++c) {
                dst[outIndex + c] = static_cast<uint8_t>(sums[c] / window);
            }
            const int addX = std::clamp(x + radius + 1, 0, width - 1);
            const int removeX = std::clamp(x - radius, 0, width - 1);
            const size_t addIndex = (static_cast<size_t>(y) * width + addX) * 4;
            const size_t removeIndex = (static_cast<size_t>(y) * width + removeX) * 4;
            for (int c = 0; c < 4; ++c) {
                sums[c] += src[addIndex + c] - src[removeIndex + c];
            }
        }
    }
}

void blurPassVertical(const std::vector<uint8_t> &src, std::vector<uint8_t> &dst,
                      int width, int height, int radius) {
    const int window = radius * 2 + 1;
    for (int x = 0; x < width; ++x) {
        int sums[4] = {0, 0, 0, 0};
        for (int i = -radius; i <= radius; ++i) {
            const int y = std::clamp(i, 0, height - 1);
            const size_t index = (static_cast<size_t>(y) * width + x) * 4;
            for (int c = 0; c < 4; ++c) {
                sums[c] += src[index + c];
            }
        }
        for (int y = 0; y < height; ++y) {
            const size_t outIndex = (static_cast<size_t>(y) * width + x) * 4;
            for (int c = 0; c < 4; ++c) {
                dst[outIndex + c] = static_cast<uint8_t>(sums[c] / window);
            }
            const int addY = std::clamp(y + radius + 1, 0, height - 1);
            const int removeY = std::clamp(y - radius, 0, height - 1);
            const size_t addIndex = (static_cast<size_t>(addY) * width + x) * 4;
            const size_t removeIndex = (static_cast<size_t>(removeY) * width + x) * 4;
            for (int c = 0; c < 4; ++c) {
                sums[c] += src[addIndex + c] - src[removeIndex + c];
            }
        }
    }
}

} // namespace

void boxBlurRGBA(std::vector<uint8_t> &pixels, int width, int height, int radius, int passes) {
    if (width <= 0 || height <= 0 || radius <= 0 || pixels.size() < static_cast<size_t>(width) * height * 4) {
        return;
    }
    std::vector<uint8_t> temp(pixels.size());
    for (int pass = 0; pass < passes; ++pass) {
        blurPassHorizontal(pixels, temp, width, height, radius);
        blurPassVertical(temp, pixels, width, height, radius);
    }
}

void desaturateRGBA(std::vector<uint8_t> &pixels, int width, int height, float amount) {
    if (width <= 0 || height <= 0 || amount <= 0.0f) {
        return;
    }
    amount = std::clamp(amount, 0.0f, 1.0f);
    const size_t count = std::min(pixels.size(), static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i + 3 < count; i += 4) {
        const float grey = pixels[i] * 0.2126f + pixels[i + 1] * 0.7152f + pixels[i + 2] * 0.0722f;
        for (int c = 0; c < 3; ++c) {
            pixels[i + c] = static_cast<uint8_t>(pixels[i + c] * (1.0f - amount) + grey * amount);
        }
    }
}

void roundedAlphaMask(std::vector<uint8_t> &pixels, int width, int height, float radius) {
    if (width <= 0 || height <= 0 || radius <= 0.0f) {
        return;
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float dx = 0.0f;
            float dy = 0.0f;
            if (x < radius) {
                dx = radius - (static_cast<float>(x) + 0.5f);
            } else if (x >= width - radius) {
                dx = (static_cast<float>(x) + 0.5f) - (static_cast<float>(width) - radius);
            }
            if (y < radius) {
                dy = radius - (static_cast<float>(y) + 0.5f);
            } else if (y >= height - radius) {
                dy = (static_cast<float>(y) + 0.5f) - (static_cast<float>(height) - radius);
            }
            if (dx == 0.0f && dy == 0.0f) {
                continue;
            }
            const float distance = std::sqrt(dx * dx + dy * dy);
            const float coverage = std::clamp(radius + 0.5f - distance, 0.0f, 1.0f);
            const size_t index = (static_cast<size_t>(y) * width + x) * 4 + 3;
            pixels[index] = static_cast<uint8_t>(pixels[index] * coverage);
        }
    }
}

} // namespace revdock
