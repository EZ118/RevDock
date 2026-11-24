#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace revdock {

enum class ItemType {
    Application,
    Folder,
    File,
    Url,
    Separator,
    AppGrid,
    Trash,
    MinimizedWindow,
    Finder,
};

struct DockItem {
    ItemType type = ItemType::Application;
    std::string id;
    std::string path;
    std::string label;
    std::string iconPath;
    bool pinned = true;
    // Folder tiles: true = show stacked contents and open the stack on click,
    // false = show the folder artwork and open the folder in the file manager.
    bool stack = true;
    int showAs = 0;      // 0 automatic, 1 fan, 2 grid, 3 list
    int arrangement = 1; // 1 name, 2 date added, 3 modified, 4 created, 5 kind
    int guid = 0;
    std::uintptr_t windowHandle = 0;
    // Landscape/portrait shape of a minimized window's thumbnail (fitted into
    // the compact icon-sized tile when drawn).
    float previewAspect = 1.0f;

    bool isApplication() const { return type == ItemType::Application; }
    bool isFolder() const { return type == ItemType::Folder; }
    bool isSpecial() const {
        return type == ItemType::Separator || type == ItemType::AppGrid ||
               type == ItemType::Trash || type == ItemType::Finder;
    }
};

const char *itemTypeToString(ItemType type);
ItemType itemTypeFromString(const std::string &value);

class DockModel {
public:
    DockModel();

    std::vector<DockItem> &items() { return items_; }
    const std::vector<DockItem> &items() const { return items_; }

    void createDefaultsIfEmpty(const std::string &downloadsPath, const std::string &trashLabel);
    void addItem(DockItem item, int index = -1);
    bool removeAt(size_t index);
    bool moveItem(size_t from, size_t to);
    bool containsApplication(const std::string &id) const;
    int indexOfApplication(const std::string &id) const;

    nlohmann::json toJson() const;
    void fromJson(const nlohmann::json &json);

private:
    std::vector<DockItem> items_;
};

} // namespace revdock
