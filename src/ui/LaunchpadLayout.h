#pragma once

// Launchpad's data model, reverse-engineered from LPRoot/LPPage/LPGroup and
// ECSBSpringboard.  The layout is a list of pages; a page holds applications
// and folders.  Folders hold a flat list of applications and paginate with the
// same cells-per-page capacity as the root.
//
// The model is header-only so the UI and the unit tests can share it without a
// separate library target.

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace revdock::launchpad {

inline std::string lowerKey(std::string value) {
    for (char &ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

struct Item {
    bool folder = false;
    // Application item.
    std::string appId;
    // Folder item.
    std::string folderId;
    std::string title;
    std::vector<std::string> apps;

    bool isFolder() const { return folder; }
};

struct Page {
    std::vector<Item> items;
};

// One entry for the auto-generated initial layout.
struct InitialApp {
    std::string id;
    std::string name;
    // Outermost Start Menu folder name; empty for root shortcuts and UWP apps.
    std::string group;
};

struct Layout {
    std::vector<Page> pages;

    void ensurePage() {
        if (pages.empty()) {
            pages.emplace_back();
        }
    }

    // First-run Launchpad: rebuild the Start Menu's folder tree, but keep only
    // the outermost folder name per group.  Groups with a single app stay
    // standalone, matching the system menu (a folder of one is just an app).
    void buildInitial(const std::vector<InitialApp> &apps, int perPage) {
        pages.clear();
        perPage = std::max(1, perPage);
        struct Group {
            std::string title;
            std::vector<InitialApp> apps;
        };
        std::map<std::string, Group> groups;
        struct Single {
            std::string sort;
            std::string name;
            std::string id;
        };
        std::vector<Single> singles;
        for (const InitialApp &app : apps) {
            if (app.id.empty()) {
                continue;
            }
            if (app.group.empty()) {
                singles.push_back({lowerKey(app.name), app.name, app.id});
                continue;
            }
            Group &group = groups[lowerKey(app.group)];
            if (group.title.empty()) {
                group.title = app.group;
            }
            group.apps.push_back(app);
        }
        struct Entry {
            std::string sort;
            std::string tie;
            Item item;
        };
        std::vector<Entry> entries;
        int folderIndex = 0;
        for (auto &[key, group] : groups) {
            (void)key;
            if (group.apps.size() >= 2) {
                Item item;
                item.folder = true;
                item.folderId = "folder-auto-" + std::to_string(folderIndex++);
                item.title = group.title;
                for (const InitialApp &app : group.apps) {
                    item.apps.push_back(app.id);
                }
                entries.push_back({lowerKey(group.title), group.title, std::move(item)});
            } else if (!group.apps.empty()) {
                const InitialApp &app = group.apps.front();
                singles.push_back({lowerKey(app.name), app.name, app.id});
            }
        }
        for (Single &single : singles) {
            Item item;
            item.appId = std::move(single.id);
            entries.push_back({std::move(single.sort), std::move(single.name),
                               std::move(item)});
        }
        std::sort(entries.begin(), entries.end(),
                  [](const Entry &a, const Entry &b) {
                      if (a.sort != b.sort) {
                          return a.sort < b.sort;
                      }
                      return a.tie < b.tie;
                  });
        for (Entry &entry : entries) {
            if (pages.empty() ||
                static_cast<int>(pages.back().items.size()) >= perPage) {
                pages.emplace_back();
            }
            pages.back().items.push_back(std::move(entry.item));
        }
        ensurePage();
    }

    int pageCount() const { return static_cast<int>(pages.size()); }

    int rootItemCount() const {
        int total = 0;
        for (const Page &page : pages) {
            total += static_cast<int>(page.items.size());
        }
        return total;
    }

    int folderAppCount() const {
        int total = 0;
        for (const Page &page : pages) {
            for (const Item &item : page.items) {
                if (item.folder) {
                    total += static_cast<int>(item.apps.size());
                }
            }
        }
        return total;
    }

    const Item *itemAt(int page, int slot) const {
        if (page < 0 || page >= static_cast<int>(pages.size())) {
            return nullptr;
        }
        const auto &items = pages[page].items;
        if (slot < 0 || slot >= static_cast<int>(items.size())) {
            return nullptr;
        }
        return &items[slot];
    }

    Item *itemAt(int page, int slot) {
        return const_cast<Item *>(static_cast<const Layout *>(this)->itemAt(page, slot));
    }

    bool containsApp(const std::string &id) const {
        for (const Page &page : pages) {
            for (const Item &item : page.items) {
                if (!item.folder && item.appId == id) {
                    return true;
                }
                if (item.folder &&
                    std::find(item.apps.begin(), item.apps.end(), id) != item.apps.end()) {
                    return true;
                }
            }
        }
        return false;
    }

    bool findApp(const std::string &id, int *page, int *slot) const {
        for (int p = 0; p < static_cast<int>(pages.size()); ++p) {
            const auto &items = pages[p].items;
            for (int s = 0; s < static_cast<int>(items.size()); ++s) {
                if (!items[s].folder && items[s].appId == id) {
                    if (page) {
                        *page = p;
                    }
                    if (slot) {
                        *slot = s;
                    }
                    return true;
                }
            }
        }
        return false;
    }

    const Item *findFolder(const std::string &id, int *page = nullptr,
                           int *slot = nullptr) const {
        for (int p = 0; p < static_cast<int>(pages.size()); ++p) {
            const auto &items = pages[p].items;
            for (int s = 0; s < static_cast<int>(items.size()); ++s) {
                if (items[s].folder && items[s].folderId == id) {
                    if (page) {
                        *page = p;
                    }
                    if (slot) {
                        *slot = s;
                    }
                    return &items[s];
                }
            }
        }
        return nullptr;
    }

    Item *findFolder(const std::string &id, int *page = nullptr, int *slot = nullptr) {
        return const_cast<Item *>(
            static_cast<const Layout *>(this)->findFolder(id, page, slot));
    }

    // Insert an item into a page and cascade any overflow to the following
    // pages, matching cleanupOverflowFromPage:.
    void insertItem(Item item, int page, int slot, int perPage) {
        ensurePage();
        page = std::clamp(page, 0, static_cast<int>(pages.size()));
        if (page == static_cast<int>(pages.size())) {
            pages.emplace_back();
        }
        auto &items = pages[page].items;
        slot = std::clamp(slot, 0, static_cast<int>(items.size()));
        items.insert(items.begin() + slot, std::move(item));
        for (int p = page; p < static_cast<int>(pages.size()); ++p) {
            while (perPage > 0 &&
                   static_cast<int>(pages[p].items.size()) > perPage) {
                Item overflow = std::move(pages[p].items.back());
                pages[p].items.pop_back();
                if (p + 1 >= static_cast<int>(pages.size())) {
                    pages.emplace_back();
                }
                pages[p + 1].items.insert(pages[p + 1].items.begin(),
                                          std::move(overflow));
            }
        }
    }

    bool insertApp(const std::string &appId, int page, int slot, int perPage) {
        if (appId.empty()) {
            return false;
        }
        Item item;
        item.appId = appId;
        insertItem(std::move(item), page, slot, perPage);
        return true;
    }

    // Adds a new application to the next free root slot (LPLayout's
    // layoutItemsToNextAvailableSpace:).
    bool addAppToNextAvailable(const std::string &appId, int perPage) {
        if (appId.empty()) {
            return false;
        }
        ensurePage();
        for (Page &page : pages) {
            if (perPage <= 0 ||
                static_cast<int>(page.items.size()) < perPage) {
                Item item;
                item.appId = appId;
                page.items.push_back(std::move(item));
                return true;
            }
        }
        Item item;
        item.appId = appId;
        pages.emplace_back();
        pages.back().items.push_back(std::move(item));
        return true;
    }

    bool removeItemAt(int page, int slot) {
        if (page < 0 || page >= static_cast<int>(pages.size())) {
            return false;
        }
        auto &items = pages[page].items;
        if (slot < 0 || slot >= static_cast<int>(items.size())) {
            return false;
        }
        items.erase(items.begin() + slot);
        return true;
    }

    // Move an existing root item (application or folder) to another slot.
    bool moveItem(int fromPage, int fromSlot, int toPage, int toSlot, int perPage) {
        if (fromPage < 0 || fromPage >= static_cast<int>(pages.size()) ||
            fromSlot < 0 || fromSlot >= static_cast<int>(pages[fromPage].items.size())) {
            return false;
        }
        Item item = std::move(pages[fromPage].items[fromSlot]);
        pages[fromPage].items.erase(pages[fromPage].items.begin() + fromSlot);
        if (fromPage == toPage && fromSlot < toSlot) {
            --toSlot;
        }
        insertItem(std::move(item), toPage, toSlot, perPage);
        cleanup(perPage);
        return true;
    }

    // Removes an application from the root or a folder.  Folders that become
    // empty are removed (shouldRemoveGroup:).
    bool removeAppEverywhere(const std::string &appId) {
        if (appId.empty()) {
            return false;
        }
        for (Page &page : pages) {
            for (auto it = page.items.begin(); it != page.items.end(); ++it) {
                if (!it->folder && it->appId == appId) {
                    page.items.erase(it);
                    return true;
                }
                if (it->folder) {
                    auto found =
                        std::find(it->apps.begin(), it->apps.end(), appId);
                    if (found != it->apps.end()) {
                        it->apps.erase(found);
                        if (it->apps.empty()) {
                            page.items.erase(it);
                        }
                        return true;
                    }
                }
            }
        }
        return false;
    }

    bool addAppToFolder(const std::string &appId, const std::string &folderId) {
        if (appId.empty() || folderId.empty()) {
            return false;
        }
        const Item *folder = findFolder(folderId);
        if (!folder) {
            return false;
        }
        if (std::find(folder->apps.begin(), folder->apps.end(), appId) !=
            folder->apps.end()) {
            return false;
        }
        // Moving into a folder takes the application out of the root or out
        // of a previous folder, so an app can never be in two places.
        removeAppEverywhere(appId);
        Item *target = findFolder(folderId);
        if (!target) {
            return false;
        }
        target->apps.push_back(appId);
        return true;
    }

    // Dragging one application onto another creates the classic Launchpad
    // folder (makeGroupUsingItem:withItem:atIndex:onPage:).
    bool makeFolderFromApps(const std::string &draggedApp,
                            const std::string &targetApp,
                            const std::string &folderId,
                            const std::string &title, int perPage) {
        if (draggedApp.empty() || targetApp.empty() ||
            draggedApp == targetApp) {
            return false;
        }
        int targetPage = 0;
        int targetSlot = 0;
        if (!findApp(targetApp, &targetPage, &targetSlot)) {
            return false;
        }
        if (!removeAppEverywhere(draggedApp)) {
            return false;
        }
        // Removing the dragged application may have shifted the target.
        if (!findApp(targetApp, &targetPage, &targetSlot)) {
            return false;
        }
        Item folder;
        folder.folder = true;
        folder.folderId = folderId;
        folder.title = title;
        folder.apps.push_back(targetApp);
        folder.apps.push_back(draggedApp);
        pages[targetPage].items[targetSlot] = std::move(folder);
        cleanup(perPage);
        return true;
    }

    bool moveAppIntoFolder(const std::string &appId,
                           const std::string &folderId, int perPage) {
        if (appId.empty() || folderId.empty()) {
            return false;
        }
        const Item *folder = findFolder(folderId);
        if (!folder) {
            return false;
        }
        if (std::find(folder->apps.begin(), folder->apps.end(), appId) !=
            folder->apps.end()) {
            return false;
        }
        if (!removeAppEverywhere(appId)) {
            return false;
        }
        Item *target = findFolder(folderId);
        if (!target) {
            return false;
        }
        target->apps.push_back(appId);
        cleanup(perPage);
        return true;
    }

    bool moveAppIntoFolderAt(const std::string &appId,
                             const std::string &folderId, int index,
                             int perPage) {
        if (appId.empty() || folderId.empty()) {
            return false;
        }
        const Item *folder = findFolder(folderId);
        if (!folder) {
            return false;
        }
        if (std::find(folder->apps.begin(), folder->apps.end(), appId) !=
            folder->apps.end()) {
            return false;
        }
        if (!removeAppEverywhere(appId)) {
            return false;
        }
        Item *target = findFolder(folderId);
        if (!target) {
            return false;
        }
        index = std::clamp(index, 0, static_cast<int>(target->apps.size()));
        target->apps.insert(target->apps.begin() + index, appId);
        cleanup(perPage);
        return true;
    }

    bool moveAppOutOfFolder(const std::string &folderId,
                            const std::string &appId, int perPage) {
        Item *folder = findFolder(folderId);
        if (!folder) {
            return false;
        }
        auto found = std::find(folder->apps.begin(), folder->apps.end(), appId);
        if (found == folder->apps.end()) {
            return false;
        }
        folder->apps.erase(found);
        if (folder->apps.empty()) {
            for (Page &page : pages) {
                page.items.erase(
                    std::remove_if(page.items.begin(), page.items.end(),
                                   [&](const Item &item) {
                                       return item.folder &&
                                              item.folderId == folderId;
                                   }),
                    page.items.end());
            }
        }
        addAppToNextAvailable(appId, perPage);
        cleanup(perPage);
        return true;
    }

    bool moveFolderApp(const std::string &folderId, int fromIndex,
                       int toIndex) {
        Item *folder = findFolder(folderId);
        if (!folder) {
            return false;
        }
        const int count = static_cast<int>(folder->apps.size());
        if (fromIndex < 0 || fromIndex >= count) {
            return false;
        }
        const std::string appId = folder->apps[fromIndex];
        folder->apps.erase(folder->apps.begin() + fromIndex);
        if (toIndex < 0) {
            toIndex = 0;
        }
        if (toIndex > static_cast<int>(folder->apps.size())) {
            toIndex = static_cast<int>(folder->apps.size());
        }
        folder->apps.insert(folder->apps.begin() + toIndex, appId);
        return true;
    }

    bool removeFolder(const std::string &folderId, int perPage) {
        for (Page &page : pages) {
            const auto before = page.items.size();
            page.items.erase(
                std::remove_if(page.items.begin(), page.items.end(),
                               [&](const Item &item) {
                                   return item.folder &&
                                          item.folderId == folderId;
                               }),
                page.items.end());
            if (page.items.size() != before) {
                cleanup(perPage);
                return true;
            }
        }
        return false;
    }

    bool renameFolder(const std::string &folderId, const std::string &title) {
        Item *folder = findFolder(folderId);
        if (!folder) {
            return false;
        }
        folder->title = title;
        return true;
    }

    // Removes empty folders/pages and enforces the page capacity.  Returns true
    // when the structure changed.
    bool cleanup(int perPage) {
        bool changed = false;
        for (Page &page : pages) {
            const auto before = page.items.size();
            page.items.erase(
                std::remove_if(page.items.begin(), page.items.end(),
                               [](const Item &item) {
                                   return item.folder && item.apps.empty();
                               }),
                page.items.end());
            changed = changed || page.items.size() != before;
        }
        for (int p = 0; p < static_cast<int>(pages.size()); ++p) {
            while (perPage > 0 &&
                   static_cast<int>(pages[p].items.size()) > perPage) {
                Item overflow = std::move(pages[p].items.back());
                pages[p].items.pop_back();
                if (p + 1 >= static_cast<int>(pages.size())) {
                    pages.emplace_back();
                }
                pages[p + 1].items.insert(pages[p + 1].items.begin(),
                                          std::move(overflow));
                changed = true;
            }
        }
        if (pages.size() > 1) {
            const auto before = pages.size();
            pages.erase(std::remove_if(pages.begin(), pages.end(),
                                       [](const Page &page) {
                                           return page.items.empty();
                                       }),
                        pages.end());
            changed = changed || pages.size() != before;
        }
        if (pages.empty()) {
            pages.emplace_back();
            changed = true;
        }
        // Keep at least one page, even when the first page is empty.
        if (pages.size() > 1 && pages.front().items.empty()) {
            // The erase above already removed empty pages, so this only keeps
            // the invariant explicit for callers that manipulated pages.
        }
        return changed;
    }

    // Reconciles the persisted layout with the applications the platform
    // currently exposes.  Existing ordering is preserved; missing applications
    // are appended to the next available slot, stale ids are dropped, and
    // duplicate entries (including inside folders) are collapsed.
    bool reconcile(const std::vector<std::string> &installedApps, int perPage) {
        ensurePage();
        if (installedApps.empty()) {
            return false;
        }
        bool changed = false;
        std::map<std::string, std::string> canonical;
        for (const std::string &id : installedApps) {
            canonical.emplace(lowerKey(id), id);
        }
        auto canonicalId = [&](const std::string &id) -> std::string {
            auto found = canonical.find(lowerKey(id));
            return found == canonical.end() ? std::string() : found->second;
        };

        std::set<std::string> seen;
        for (Page &page : pages) {
            std::vector<Item> rebuilt;
            rebuilt.reserve(page.items.size());
            for (Item &item : page.items) {
                if (!item.folder) {
                    const std::string id = canonicalId(item.appId);
                    if (id.empty() || !seen.insert(id).second) {
                        changed = true;
                        continue;
                    }
                    if (id != item.appId) {
                        changed = true;
                        item.appId = id;
                    }
                    rebuilt.push_back(std::move(item));
                    continue;
                }
                std::vector<std::string> kept;
                kept.reserve(item.apps.size());
                for (const std::string &appId : item.apps) {
                    const std::string id = canonicalId(appId);
                    if (id.empty() || !seen.insert(id).second) {
                        changed = true;
                        continue;
                    }
                    if (id != appId) {
                        changed = true;
                    }
                    kept.push_back(id);
                }
                if (kept.empty()) {
                    changed = true;
                    continue;
                }
                if (kept.size() != item.apps.size()) {
                    changed = true;
                }
                item.apps = std::move(kept);
                if (item.folderId.empty()) {
                    item.folderId = "folder-" + std::to_string(seen.size());
                    changed = true;
                }
                if (item.title.empty()) {
                    item.title = "Untitled Folder";
                    changed = true;
                }
                rebuilt.push_back(std::move(item));
            }
            if (rebuilt.size() != page.items.size()) {
                changed = true;
            }
            page.items = std::move(rebuilt);
        }
        for (const std::string &raw : installedApps) {
            const std::string id = canonicalId(raw);
            if (id.empty() || seen.count(id) > 0) {
                continue;
            }
            seen.insert(id);
            addAppToNextAvailable(id, perPage);
            changed = true;
        }
        changed = cleanup(perPage) || changed;
        return changed;
    }

    nlohmann::json toJson() const {
        nlohmann::json root;
        root["version"] = 1;
        nlohmann::json pageArray = nlohmann::json::array();
        for (const Page &page : pages) {
            nlohmann::json items = nlohmann::json::array();
            for (const Item &item : page.items) {
                if (item.folder) {
                    if (item.apps.empty()) {
                        continue;
                    }
                    items.push_back({{"type", "folder"},
                                     {"id", item.folderId},
                                     {"title", item.title},
                                     {"apps", item.apps}});
                } else if (!item.appId.empty()) {
                    items.push_back(
                        {{"type", "app"}, {"app", item.appId}});
                }
            }
            pageArray.push_back({{"items", std::move(items)}});
        }
        root["pages"] = std::move(pageArray);
        return root;
    }

    static Layout fromJson(const nlohmann::json &root) {
        Layout layout;
        const auto found = root.find("pages");
        if (found == root.end() || !found->is_array()) {
            layout.ensurePage();
            return layout;
        }
        for (const auto &pageJson : *found) {
            Page page;
            const auto itemsIt = pageJson.find("items");
            if (itemsIt != pageJson.end() && itemsIt->is_array()) {
                for (const auto &itemJson : *itemsIt) {
                    Item item;
                    const std::string type =
                        itemJson.value("type", "app");
                    if (type == "folder") {
                        item.folder = true;
                        item.folderId = itemJson.value("id", "");
                        item.title = itemJson.value("title", "");
                        const auto apps = itemJson.find("apps");
                        if (apps != itemJson.end() && apps->is_array()) {
                            for (const auto &app : *apps) {
                                if (app.is_string()) {
                                    item.apps.push_back(app.get<std::string>());
                                }
                            }
                        }
                        if (item.folderId.empty() || item.apps.empty()) {
                            continue;
                        }
                    } else {
                        item.appId = itemJson.value("app", "");
                        if (item.appId.empty()) {
                            continue;
                        }
                    }
                    page.items.push_back(std::move(item));
                }
            }
            layout.pages.push_back(std::move(page));
        }
        layout.ensurePage();
        return layout;
    }

    // Search order: exact match, prefix, word prefix, then substring.  Ties
    // are alphabetical.  Duplicate ids are collapsed.
    static int searchScore(const std::string &name, const std::string &query) {
        if (query.empty()) {
            return -1;
        }
        const std::string haystack = lowerKey(name);
        const std::string needle = lowerKey(query);
        const size_t pos = haystack.find(needle);
        if (pos == std::string::npos) {
            return -1;
        }
        if (pos == 0) {
            return haystack.size() == needle.size() ? 0 : 1;
        }
        const unsigned char previous =
            static_cast<unsigned char>(haystack[pos - 1]);
        if (previous == ' ' || previous == '-' || previous == '_' ||
            previous == '(' || previous == '[') {
            return 2;
        }
        return 3;
    }
};

} // namespace revdock::launchpad
