#pragma once

#include "platform/AppInfo.h"

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace revdock {

class Platform;

class AppIndex {
public:
    AppIndex(Platform &platform, std::filesystem::path cacheFile);

    void refresh(bool force = false);
    void refreshRunning();
    bool refreshRunningIfDue(double now);

    const std::vector<AppInfo> &apps() const { return apps_; }
    const AppInfo *findById(const std::string &id) const;
    const AppInfo *findByPath(const std::string &path) const;
    const AppInfo *findByAumid(const std::string &aumid) const;
    bool isRunning(const AppInfo &app) const;
    // Record that a Dock item's real program runs under another process name
    // (for example an Office icon-stub shortcut launching WINWORD.EXE, or a
    // command prompt hosted by Windows Terminal).
    void setProcessAlias(const std::string &appId, const std::string &processPath);

    double lastScanTime() const { return lastScan_; }

private:
    bool loadCache();
    void saveCache() const;
    void rebuildMaps();

    Platform &platform_;
    std::filesystem::path cacheFile_;
    std::vector<AppInfo> apps_;
    std::map<std::string, size_t> byId_;
    std::map<std::string, size_t> byExec_;
    std::map<std::string, size_t> byAumid_;
    std::set<std::string> running_;
    std::set<std::string> processes_;
    std::set<std::string> processStems_;
    std::map<std::string, std::set<std::string>> aliases_;
    double lastScan_ = 0.0;
    double lastRunningRefresh_ = 0.0;
};

} // namespace revdock
