#pragma once

#include <cstdint>
#include <vector>

namespace revdock {

void boxBlurRGBA(std::vector<uint8_t> &pixels, int width, int height, int radius, int passes = 2);
void roundedAlphaMask(std::vector<uint8_t> &pixels, int width, int height, float radius);
void desaturateRGBA(std::vector<uint8_t> &pixels, int width, int height, float amount);

} // namespace revdock
