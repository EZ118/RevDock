#include "core/Paths.h"

#include "core/Log.h"

#include <cstdlib>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <unistd.h>
#endif

namespace revdock {
namespace {

std::filesystem::path envPath(const char *name) {
    const char *value = std::getenv(name);
    return value && *value ? std::filesystem::path(value) : std::filesystem::path{};
}

std::filesystem::path executablePath() {
#ifdef _WIN32
    wchar_t buffer[32768];
    const DWORD len = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (len > 0 && len < std::size(buffer)) {
        return std::filesystem::path(buffer).parent_path();
    }
#else
    char buffer[4096];
    const ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len > 0) {
        buffer[len] = '\0';
        return std::filesystem::path(buffer).parent_path();
    }
#endif
    return std::filesystem::current_path();
}

#ifdef _WIN32
std::filesystem::path knownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw)) && raw) {
        std::filesystem::path result(raw);
        CoTaskMemFree(raw);
        return result;
    }
    return {};
}
#endif

} // namespace

void Paths::initialize() {
    executableDir = executablePath();

    portable = std::filesystem::exists(executableDir / "revdock.portable");

#ifdef _WIN32
    const auto roaming = knownFolder(FOLDERID_RoamingAppData);
    const auto local = knownFolder(FOLDERID_LocalAppData);
    if (portable) {
        configDir = executableDir / "data";
        cacheDir = executableDir / "data" / "cache";
        stateDir = executableDir / "data" / "state";
        logDir = executableDir / "data" / "logs";
    } else {
        configDir = roaming / "RevDock";
        cacheDir = local / "RevDock" / "cache";
        stateDir = local / "RevDock" / "state";
        logDir = local / "RevDock" / "logs";
    }
#else
    const auto home = envPath("HOME");
    const auto configHome = envPath("XDG_CONFIG_HOME");
    const auto cacheHome = envPath("XDG_CACHE_HOME");
    const auto dataHome = envPath("XDG_DATA_HOME");
    const auto stateHome = envPath("XDG_STATE_HOME");

    if (portable) {
        configDir = executableDir / "data";
        cacheDir = executableDir / "data" / "cache";
        stateDir = executableDir / "data" / "state";
        logDir = executableDir / "data" / "logs";
    } else {
        configDir = (configHome.empty() ? home / ".config" : configHome) / "revdock";
        cacheDir = (cacheHome.empty() ? home / ".cache" : cacheHome) / "revdock";
        stateDir = (stateHome.empty() ? home / ".local" / "state" : stateHome) / "revdock";
        logDir = stateDir / "logs";
    }
#endif

    const auto overrideConfig = envPath("REVDOCK_CONFIG_DIR");
    const auto overrideCache = envPath("REVDOCK_CACHE_DIR");
    if (!overrideConfig.empty()) {
        configDir = overrideConfig;
    }
    if (!overrideCache.empty()) {
        cacheDir = overrideCache;
    }

    std::error_code ec;
    std::filesystem::create_directories(configDir, ec);
    std::filesystem::create_directories(cacheDir / "icons", ec);
    std::filesystem::create_directories(stateDir, ec);
    std::filesystem::create_directories(logDir, ec);

    assetDir = executableDir / "assets";
    const auto installedAssets = executableDir.parent_path() / "share" / "revdock" / "assets";
    if (!std::filesystem::exists(assetDir / "fonts" / "Inter" / "Inter-Regular.ttf") &&
        std::filesystem::exists(installedAssets / "fonts" / "Inter" / "Inter-Regular.ttf")) {
        assetDir = installedAssets;
    }
    if (!std::filesystem::exists(assetDir / "fonts" / "Inter" / "Inter-Regular.ttf")) {
#ifdef REVDOCK_SOURCE_ASSETS
        assetDir = REVDOCK_SOURCE_ASSETS;
#endif
    }
    if (!std::filesystem::exists(assetDir / "fonts" / "Inter" / "Inter-Regular.ttf")) {
        RD_LOG_WARN("Asset directory not found (looked at %s)", assetDir.string().c_str());
    }
}

} // namespace revdock
