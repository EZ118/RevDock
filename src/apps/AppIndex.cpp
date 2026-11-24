#include "apps/AppIndex.h"

#include "core/Clock.h"
#include "core/FileUtil.h"
#include "core/Log.h"
#include "core/Types.h"
#include "core/Utf.h"
#include "platform/Platform.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace revdock {

AppIndex::AppIndex(Platform &platform, std::filesystem::path cacheFile)
    : platform_(platform), cacheFile_(std::move(cacheFile)) {}

void AppIndex::refresh(bool force) {
    if (!force && loadCache()) {
        refreshRunningIfDue(0.0);
        return;
    }
    apps_ = platform_.scanApplications();
    rebuildMaps();
    refreshRunningIfDue(0.0);
    lastScan_ = 1.0;
    saveCache();
}

void AppIndex::rebuildMaps() {
    // Finder is the Dock's single file-manager icon; hide the platform's own
    // Explorer/Nautilus/Dolphin entries from Launchpad and the recent area.
    apps_.erase(std::remove_if(apps_.begin(), apps_.end(),
                               [&](const AppInfo &app) {
                                   return platform_.isFileManagerApp(app);
                               }),
                apps_.end());
    byId_.clear();
    byExec_.clear();
    byAumid_.clear();
    for (size_t i = 0; i < apps_.size(); ++i) {
        byId_[lowerAscii(apps_[i].id)] = i;
        if (!apps_[i].execPath.empty()) {
            byExec_[lowerAscii(apps_[i].execPath)] = i;
        }
        if (!apps_[i].sourcePath.empty()) {
            byExec_[lowerAscii(apps_[i].sourcePath)] = i;
        }
        if (!apps_[i].aumid.empty()) {
            byAumid_[lowerAscii(apps_[i].aumid)] = i;
        }
    }
}

bool AppIndex::loadCache() {
    if (cacheFile_.empty()) {
        return false;
    }
    const std::string text = readTextFile(cacheFile_);
    if (text.empty()) {
        return false;
    }
    try {
        const nlohmann::json root = nlohmann::json::parse(text);
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        const auto scannedAt = root.value("scannedAt", static_cast<long long>(0));
        if (now - scannedAt > 24 * 60 * 60 || root.value("version", 0) != 9) {
            return false;
        }
        const auto array = root.find("apps");
        if (array == root.end() || !array->is_array()) {
            return false;
        }
        apps_.clear();
        for (const auto &entry : *array) {
            AppInfo app;
            app.id = entry.value("id", "");
            app.name = entry.value("name", "");
            app.sourcePath = entry.value("source", "");
            app.execPath = entry.value("exec", "");
            app.iconPath = entry.value("icon", "");
            app.wmClass = entry.value("wmClass", "");
            app.aumid = entry.value("aumid", "");
            app.menuGroup = entry.value("group", "");
            app.terminal = entry.value("terminal", false);
            app.launchExec = entry.value("launchExec", false);
            if (!app.name.empty()) {
                apps_.push_back(std::move(app));
            }
        }
        if (apps_.empty()) {
            return false;
        }
        rebuildMaps();
        const auto aliasArray = root.find("aliases");
        if (aliasArray != root.end() && aliasArray->is_array()) {
            for (const auto &entry : *aliasArray) {
                const std::string id = entry.value("id", "");
                const std::string path = entry.value("process", "");
                if (!id.empty() && !path.empty()) {
                    aliases_[lowerAscii(id)].insert(lowerAscii(path));
                }
            }
        }
        RD_LOG_INFO("Loaded %zu applications from cache", apps_.size());
        return true;
    } catch (const std::exception &error) {
        RD_LOG_WARN("Application cache parse failed: %s", error.what());
        return false;
    }
}

void AppIndex::saveCache() const {
    if (cacheFile_.empty()) {
        return;
    }
    nlohmann::json root;
    root["version"] = 9;
    root["scannedAt"] = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    nlohmann::json array = nlohmann::json::array();
    for (const auto &app : apps_) {
        array.push_back({
            {"id", app.id},
            {"name", app.name},
            {"source", app.sourcePath},
            {"exec", app.execPath},
            {"icon", app.iconPath},
            {"wmClass", app.wmClass},
            {"aumid", app.aumid},
            {"group", app.menuGroup},
            {"terminal", app.terminal},
            {"launchExec", app.launchExec},
        });
    }
    root["apps"] = std::move(array);
    nlohmann::json aliasArray = nlohmann::json::array();
    for (const auto &[id, paths] : aliases_) {
        for (const std::string &path : paths) {
            aliasArray.push_back({{"id", id}, {"process", path}});
        }
    }
    root["aliases"] = std::move(aliasArray);
    if (!writeTextFileAtomic(cacheFile_, root.dump())) {
        RD_LOG_WARN("Failed to write application cache %s", cacheFile_.string().c_str());
    }
}

void AppIndex::refreshRunning() {
    running_ = platform_.runningExecutables();
    processes_ = platform_.runningProcessPaths();
    processStems_.clear();
    for (const std::string &path : processes_) {
        const std::string stem =
            lowerAscii(pathToUtf8(utf8ToPath(path).stem()));
        if (!stem.empty()) {
            processStems_.insert(stem);
        }
    }
    lastRunningRefresh_ = 1.0;
}

bool AppIndex::refreshRunningIfDue(double now) {
    static Clock clock;
    const double current = now > 0.0 ? now : clock.now();
    if (lastRunningRefresh_ <= 0.0 || current - lastRunningRefresh_ > 1.0) {
        const auto previous = running_;
        const auto previousProcesses = processes_;
        running_ = platform_.runningExecutables();
        processes_ = platform_.runningProcessPaths();
        processStems_.clear();
        for (const std::string &path : processes_) {
            const std::string stem =
                lowerAscii(pathToUtf8(utf8ToPath(path).stem()));
            if (!stem.empty()) {
                processStems_.insert(stem);
            }
        }
        lastRunningRefresh_ = current;
        if (previous != running_ || previousProcesses != processes_) {
            RD_LOG_DEBUG("running set changed: %zu windows, %zu processes",
                         running_.size(), processes_.size());
        }
        return previous != running_ || previousProcesses != processes_;
    }
    return false;
}

void AppIndex::setProcessAlias(const std::string &appId,
                               const std::string &processPath) {
    if (appId.empty() || processPath.empty()) {
        return;
    }
    auto &paths = aliases_[lowerAscii(appId)];
    if (!paths.insert(lowerAscii(processPath)).second) {
        return;
    }
    RD_LOG_DEBUG("process alias %s -> %s", appId.c_str(), processPath.c_str());
    saveCache();
}

const AppInfo *AppIndex::findById(const std::string &id) const {
    auto it = byId_.find(lowerAscii(id));
    return it == byId_.end() ? nullptr : &apps_[it->second];
}

const AppInfo *AppIndex::findByPath(const std::string &path) const {
    auto it = byExec_.find(lowerAscii(path));
    return it == byExec_.end() ? nullptr : &apps_[it->second];
}

const AppInfo *AppIndex::findByAumid(const std::string &aumid) const {
    if (aumid.empty()) {
        return nullptr;
    }
    const auto found = byAumid_.find(lowerAscii(aumid));
    return found == byAumid_.end() ? nullptr : &apps_[found->second];
}

bool AppIndex::isRunning(const AppInfo &app) const {
    if (!app.execPath.empty()) {
        const std::string exec = lowerAscii(app.execPath);
        if (running_.count(exec) || processes_.count(exec)) {
            return true;
        }
        const std::string process =
            lowerAscii(pathToUtf8(utf8ToPath(app.execPath).stem()));
        if (running_.count(process) || processStems_.count(process)) {
            return true;
        }
    }
    if (!app.sourcePath.empty() && running_.count(lowerAscii(app.sourcePath))) {
        return true;
    }
    if (!app.aumid.empty() && running_.count(lowerAscii(app.aumid))) {
        return true;
    }
    if (const auto found = aliases_.find(lowerAscii(app.id));
        found != aliases_.end()) {
        for (const std::string &path : found->second) {
            if (processes_.count(path) || running_.count(path)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace revdock
