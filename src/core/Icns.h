#pragma once

#include <SDL3/SDL_surface.h>

#include <string>

namespace revdock {

// Decodes an .icns container and returns its largest embedded PNG as a
// surface. Returns nullptr when the file cannot be read or has no PNG payload.
SDL_Surface *loadIcnsSurface(const std::string &path);

} // namespace revdock
