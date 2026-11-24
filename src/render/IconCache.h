#pragma once

#include "model/DockModel.h"
#include "platform/AppInfo.h"

#include <SDL3/SDL_render.h>

#include <map>
#include <string>
#include <vector>

namespace revdock {

class Platform;
class AppIndex;

class IconCache {
public:
    IconCache(SDL_Renderer *renderer, Platform &platform, AppIndex &apps, float contentScale);

    SDL_Texture *textureFor(const DockItem &item, int logicalSize);
    SDL_Texture *textureForApp(const AppInfo &app, int logicalSize);
    // A blank squircle using the exact same mask, body alpha and baked soft
    // shadow as the application icons (used by Launchpad folder tiles).
    SDL_Texture *shapeTexture(int logicalSize, SDL_Color body);
    void clear();

    // Enables the macOS-style squircle conversion. Changing the value drops the
    // cached textures so the next frame regenerates them.
    void setMacStyle(bool enabled);

private:
    struct Entry {
        SDL_Texture *texture = nullptr;
        int pixelSize = 0;
    };
    struct ShapeMask {
        int size = 0;
        std::vector<uint8_t> coverage;
        std::vector<uint8_t> shadow;
    };

    SDL_Renderer *renderer_ = nullptr;
    Platform &platform_;
    AppIndex &apps_;
    float scale_ = 1.0f;
    bool macStyle_ = true;
    std::map<std::string, Entry> cache_;
    std::map<int, ShapeMask> masks_;

    const ShapeMask &shapeMaskFor(int size);
    SDL_Texture *load(const std::string &key, const std::string &path, const AppInfo *app,
                      int pixelSize, bool macStyle);
};

} // namespace revdock
