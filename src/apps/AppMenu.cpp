#include "apps/AppMenu.h"

#include "core/Sqlite.h"
#include "core/Utf.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

namespace revdock {
namespace {

std::filesystem::path envPath(const char *name) {
    const char *value = std::getenv(name);
    return value ? utf8ToPath(value) : std::filesystem::path{};
}

std::filesystem::path homeDir() {
#ifdef _WIN32
    auto home = envPath("USERPROFILE");
    if (home.empty()) {
        auto drive = envPath("HOMEDRIVE");
        auto path = envPath("HOMEPATH");
        if (!drive.empty() && !path.empty()) {
            home = drive / path;
        }
    }
    return home;
#else
    return envPath("HOME");
#endif
}

std::filesystem::path configRoot() {
#ifdef _WIN32
    auto appData = envPath("APPDATA");
    return appData.empty() ? homeDir() / "AppData" / "Roaming" : appData;
#else
    auto xdg = envPath("XDG_CONFIG_HOME");
    return xdg.empty() ? homeDir() / ".config" : xdg;
#endif
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string stemOf(const AppInfo &app) {
    if (!app.execPath.empty()) {
        return lower(pathToUtf8(utf8ToPath(app.execPath).stem()));
    }
    if (!app.sourcePath.empty()) {
        return lower(pathToUtf8(utf8ToPath(app.sourcePath).stem()));
    }
    return {};
}

bool isFirefoxStem(const std::string &stem) {
    static const std::set<std::string> kNames = {
        "firefox", "firefox-esr", "firefox-developer-edition", "librewolf",
        "waterfox", "palemoon"};
    return kNames.count(stem) > 0;
}

bool isVsCodeStem(const std::string &stem) {
    static const std::set<std::string> kNames = {
        "code", "code-insiders", "codium", "vscodium", "cursor"};
    return kNames.count(stem) > 0;
}

std::string urlHost(const std::string &url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        return {};
    }
    size_t begin = scheme + 3;
    size_t end = url.find_first_of("/?#", begin);
    if (end == std::string::npos) {
        end = url.size();
    }
    std::string host = url.substr(begin, end - begin);
    const size_t at = host.rfind('@');
    if (at != std::string::npos) {
        host = host.substr(at + 1);
    }
    return host;
}

std::filesystem::path firefoxProfileRoot() {
#ifdef _WIN32
    return configRoot() / "Mozilla" / "Firefox" / "Profiles";
#else
    return homeDir() / ".mozilla" / "firefox";
#endif
}

std::filesystem::path firefoxPlacesPath() {
    const auto root = firefoxProfileRoot();
    std::error_code ec;
    std::filesystem::path best;
    std::filesystem::file_time_type bestTime{};
    if (!std::filesystem::exists(root, ec)) {
        return best;
    }
    for (auto it = std::filesystem::directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (!it->is_directory(ec)) {
            continue;
        }
        const auto places = it->path() / "places.sqlite";
        std::error_code timeError;
        const auto time = std::filesystem::last_write_time(places, timeError);
        if (timeError) {
            continue;
        }
        if (best.empty() || time > bestTime) {
            best = places;
            bestTime = time;
        }
    }
    return best;
}

std::vector<AppMenuItem> firefoxRecentVisits(size_t limit) {
    std::vector<AppMenuItem> result;
    const auto places = firefoxPlacesPath();
    if (places.empty()) {
        return result;
    }
    Sqlite db;
    if (!db.open(pathToUtf8(places), true)) {
        return result;
    }
    const std::string sql =
        "SELECT COALESCE(NULLIF(TRIM(p.title), ''), p.url) AS label, p.url "
        "FROM moz_places p JOIN moz_historyvisits h ON h.place_id = p.id "
        "WHERE p.url LIKE 'http%' OR p.url LIKE 'https%' OR p.url LIKE 'file:%' "
        "GROUP BY p.url ORDER BY MAX(h.visit_date) DESC";
    std::set<std::string> seen;
    db.query(
        sql,
        [&](const Sqlite::Row &row) {
            if (row.size() < 2 || row[1].empty() || !seen.insert(row[1]).second) {
                return true;
            }
            AppMenuItem item;
            item.id = "recent";
            item.label = row[0].empty() ? row[1] : row[0];
            item.target = row[1];
            item.detail = urlHost(row[1]);
            item.recent = true;
            result.push_back(std::move(item));
            return result.size() < limit;
        },
        static_cast<int>(limit));
    return result;
}

std::filesystem::path vscodeUserDataRoot(const AppInfo &app) {
    const std::string stem = stemOf(app);
    std::string product = "Code";
    if (stem.find("insiders") != std::string::npos) {
        product = "Code - Insiders";
    } else if (stem.find("codium") != std::string::npos) {
        product = "VSCodium";
    } else if (stem.find("cursor") != std::string::npos) {
        product = "Cursor";
    }
#ifdef _WIN32
    return configRoot() / product;
#else
    return configRoot() / product;
#endif
}

std::string percentDecode(const std::string &value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int high = hex(value[i + 1]);
            const int low = hex(value[i + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back(static_cast<char>(high * 16 + low));
                i += 2;
                continue;
            }
        }
        out.push_back(value[i]);
    }
    return out;
}

std::string fileUriToPath(const std::string &uri) {
    if (uri.rfind("file://", 0) == 0) {
        std::string rest = uri.substr(7);
        // file:///c%3A/... -> c:/... (the colon is percent-encoded by VS Code).
        if (rest.size() >= 3 && rest[0] == '/' &&
            std::isalpha(static_cast<unsigned char>(rest[1])) &&
            (rest[2] == ':' || rest.compare(2, 3, "%3A") == 0 ||
             rest.compare(2, 3, "%3a") == 0)) {
            rest.erase(rest.begin());
        }
        std::string decoded = percentDecode(rest);
#ifdef _WIN32
        std::replace(decoded.begin(), decoded.end(), '/', '\\');
#endif
        return decoded;
    }
    return uri;
}

std::vector<AppMenuItem> vscodeRecentWorkspaces(const AppInfo &app, size_t limit) {
    std::vector<AppMenuItem> result;
    const auto root = vscodeUserDataRoot(app) / "User" / "workspaceStorage";
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
        return result;
    }
    struct Candidate {
        std::filesystem::file_time_type time{};
        std::filesystem::path workspaceJson;
    };
    std::vector<Candidate> candidates;
    for (auto it = std::filesystem::directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (!it->is_directory(ec)) {
            continue;
        }
        const auto json = it->path() / "workspace.json";
        std::error_code timeError;
        const auto time = std::filesystem::last_write_time(it->path(), timeError);
        if (timeError) {
            continue;
        }
        candidates.push_back(Candidate{time, json});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b) { return a.time > b.time; });

    std::set<std::string> seen;
    for (const auto &candidate : candidates) {
        std::ifstream input(candidate.workspaceJson);
        if (!input) {
            continue;
        }
        nlohmann::json document;
        try {
            input >> document;
        } catch (const std::exception &) {
            continue;
        }
        std::string uri;
        if (document.contains("folder") && document["folder"].is_string()) {
            uri = document["folder"].get<std::string>();
        } else if (document.contains("workspace") && document["workspace"].is_string()) {
            uri = document["workspace"].get<std::string>();
        }
        if (uri.empty()) {
            continue;
        }
        std::string path = fileUriToPath(uri);
        if (path.empty() || !seen.insert(lower(path)).second) {
            continue;
        }
        const std::filesystem::path fsPath = utf8ToPath(path);
        AppMenuItem item;
        item.id = "recent";
        item.label = pathToUtf8(fsPath.filename());
        if (item.label.empty()) {
            item.label = path;
        }
        item.detail = pathToUtf8(fsPath.parent_path().filename());
        item.target = path;
        item.recent = true;
        result.push_back(std::move(item));
        if (result.size() >= limit) {
            break;
        }
    }
    return result;
}

} // namespace

bool isWebAddress(const std::string &value) {
    const std::string lowerValue = lower(value);
    return lowerValue.rfind("http://", 0) == 0 || lowerValue.rfind("https://", 0) == 0 ||
           lowerValue.rfind("file://", 0) == 0 || lowerValue.rfind("ftp://", 0) == 0;
}

AppProfile classifyApp(const AppInfo &app) {
    const std::string stem = stemOf(app);
    const std::string name = lower(app.name);
    if (isFirefoxStem(stem) || name.find("firefox") != std::string::npos) {
        return AppProfile::Firefox;
    }
    if (isVsCodeStem(stem) || name.find("visual studio code") != std::string::npos ||
        name.find("vscodium") != std::string::npos || name == "cursor") {
        return AppProfile::VsCode;
    }
    return AppProfile::None;
}

std::vector<AppMenuItem> appProfileMenuItems(const AppInfo &app, size_t limit) {
    std::vector<AppMenuItem> result;
    const AppProfile profile = classifyApp(app);
    if (profile == AppProfile::Firefox) {
        result.push_back(AppMenuItem{"new-tab", "New Tab", {}, {}, {}, false});
        result.push_back(AppMenuItem{"new-window", "New Window", {}, {}, {}, false});
        result.push_back(
            AppMenuItem{"new-private-window", "New Private Window", {}, {}, {}, false});
        auto recent = firefoxRecentVisits(limit);
        result.insert(result.end(), std::make_move_iterator(recent.begin()),
                      std::make_move_iterator(recent.end()));
    } else if (profile == AppProfile::VsCode) {
        result.push_back(AppMenuItem{"new-window", "New Window", {}, {}, {}, false});
        auto recent = vscodeRecentWorkspaces(app, limit);
        result.insert(result.end(), std::make_move_iterator(recent.begin()),
                      std::make_move_iterator(recent.end()));
    }
    return result;
}

std::vector<std::string> appMenuArguments(const AppInfo &app, const std::string &command,
                                          const std::string &target) {
    const AppProfile profile = classifyApp(app);
    if (profile == AppProfile::Firefox) {
        if (command == "new-tab") {
            return {"-new-tab", "about:newtab"};
        }
        if (command == "new-window") {
            return {"-new-window"};
        }
        if (command == "new-private-window") {
            return {"-private-window"};
        }
        if (command == "recent" && !target.empty()) {
            return isWebAddress(target) ? std::vector<std::string>{"-new-tab", target}
                                        : std::vector<std::string>{"-new-window", target};
        }
    } else if (profile == AppProfile::VsCode) {
        if (command == "new-window") {
            return {"--new-window"};
        }
        if (command == "recent" && !target.empty()) {
            return {"--reuse-window", target};
        }
    }
    return {};
}

} // namespace revdock
