#include "core/Icns.h"

#include "core/Log.h"

#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace revdock {
namespace {

uint32_t readBe32(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

bool isPng(const uint8_t *p, size_t available) {
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    return available >= 8 && std::memcmp(p, signature, 8) == 0;
}

} // namespace

SDL_Surface *loadIcnsSurface(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return nullptr;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
    if (data.size() < 16 || std::memcmp(data.data(), "icns", 4) != 0) {
        return nullptr;
    }
    const uint32_t declared = readBe32(data.data() + 4);
    const size_t total = std::min<size_t>(data.size(), declared >= 8 ? declared : data.size());

    // Pick the largest PNG-backed entry (ic07..ic14 and icp4..icp5).
    size_t bestOffset = 0;
    size_t bestSize = 0;
    size_t offset = 8;
    while (offset + 8 <= total) {
        const uint32_t size = readBe32(data.data() + offset + 4);
        if (size < 8 || offset + size > total) {
            break;
        }
        const uint8_t *payload = data.data() + offset + 8;
        const size_t payloadSize = size - 8;
        if (payloadSize > bestSize && isPng(payload, payloadSize)) {
            bestOffset = offset + 8;
            bestSize = payloadSize;
        }
        offset += size;
    }
    if (bestSize == 0) {
        RD_LOG_WARN("ICNS has no PNG payload: %s", path.c_str());
        return nullptr;
    }
    SDL_IOStream *io = SDL_IOFromConstMem(data.data() + bestOffset, bestSize);
    if (!io) {
        return nullptr;
    }
    SDL_Surface *surface = IMG_Load_IO(io, true);
    if (!surface) {
        RD_LOG_WARN("ICNS image decode failed for %s: %s", path.c_str(), SDL_GetError());
    }
    return surface;
}

} // namespace revdock
