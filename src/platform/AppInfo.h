#pragma once

#include <string>

namespace revdock {

struct AppInfo {
    std::string id;         // stable identifier (desktop file path / shortcut path)
    std::string name;       // display name
    std::string sourcePath; // .lnk / .desktop
    std::string execPath;   // resolved executable, may be empty
    std::string iconPath;   // icon reference (path or theme name)
    std::string wmClass;    // Linux StartupWMClass / executable stem
    std::string aumid;      // UWP Application User Model ID (packaged apps)
    // Outermost Start Menu folder the shortcut was filed under (empty at the
    // root).  Used for the initial Launchpad folder layout.
    std::string menuGroup;
    bool terminal = false;
    bool hidden = false;
    // The shortcut target is only a launcher stub (Office's wordicon.exe).
    // Launch execPath directly instead of going through the .lnk.
    bool launchExec = false;
};

// One application-provided Dock menu entry (the equivalent of macOS'
// applicationDockMenu: / Windows jump-list task and recent-document entries).
struct AppMenuItem {
    std::string id;       // stable command ("new-window", "recent", ...)
    std::string label;
    std::string target;   // file path or URL for recent entries
    std::string detail;   // secondary text (host, parent directory)
    std::string iconPath; // optional icon override
    bool recent = false;
};

} // namespace revdock
