#pragma once

#include "platform/AppInfo.h"

#include <cstddef>
#include <string>
#include <vector>

namespace revdock {

enum class AppProfile {
    None,
    Firefox,
    VsCode,
};

// App-specific entries shown above the standard Dock items, like the
// application-provided items macOS inserts through applicationDockMenu:.
// (This is separate from the persisted "Dock menu" in the reverse-engineered
// DockMenus.plist, which describes the standard menu.)
AppProfile classifyApp(const AppInfo &app);

// Tasks (New Window, ...) followed by recent history/workspace entries.
std::vector<AppMenuItem> appProfileMenuItems(const AppInfo &app, size_t limit = 8);

// Command-line arguments for a task/recent entry returned by
// appProfileMenuItems(). An empty vector means the caller should use the
// generic file/URL opener instead.
std::vector<std::string> appMenuArguments(const AppInfo &app, const std::string &command,
                                          const std::string &target);

bool isWebAddress(const std::string &value);

} // namespace revdock
