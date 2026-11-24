#pragma once

#include <filesystem>

namespace revdock {

struct Paths {
    std::filesystem::path executableDir;
    std::filesystem::path configDir;
    std::filesystem::path cacheDir;
    std::filesystem::path stateDir;
    std::filesystem::path logDir;
    std::filesystem::path assetDir;
    bool portable = false;

    void initialize();
    std::filesystem::path configFile() const { return configDir / "config.json"; }
    std::filesystem::path appsCacheFile() const { return cacheDir / "apps.json"; }
    std::filesystem::path iconCacheDir() const { return cacheDir / "icons"; }
    std::filesystem::path logFile() const { return logDir / "revdock.log"; }
};

} // namespace revdock
