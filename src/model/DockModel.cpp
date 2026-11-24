#include "model/DockModel.h"

#include "core/Types.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>

namespace revdock {

const char *itemTypeToString(ItemType type) {
    switch (type) {
    case ItemType::Application: return "application";
    case ItemType::Folder: return "folder";
    case ItemType::File: return "file";
    case ItemType::Url: return "url";
    case ItemType::Separator: return "separator";
    case ItemType::AppGrid: return "app-grid";
    case ItemType::Trash: return "trash";
    case ItemType::MinimizedWindow: return "minimized-window";
    case ItemType::Finder: return "finder";
    }
    return "application";
}

ItemType itemTypeFromString(const std::string &value) {
    if (value == "folder") return ItemType::Folder;
    if (value == "file") return ItemType::File;
    if (value == "url") return ItemType::Url;
    if (value == "separator") return ItemType::Separator;
    if (value == "app-grid") return ItemType::AppGrid;
    if (value == "trash") return ItemType::Trash;
    if (value == "minimized-window") return ItemType::MinimizedWindow;
    if (value == "finder") return ItemType::Finder;
    return ItemType::Application;
}

DockModel::DockModel() = default;

void DockModel::createDefaultsIfEmpty(const std::string &downloadsPath, const std::string &trashLabel) {
    if (!items_.empty()) {
        return;
    }

    DockItem finder;
    finder.type = ItemType::Finder;
    finder.id = "special:finder";
    finder.label = "Finder";
    items_.push_back(finder);

    DockItem grid;
    grid.type = ItemType::AppGrid;
    grid.id = "special:app-grid";
    grid.label = "Applications";
    items_.push_back(grid);

    DockItem separator;
    separator.type = ItemType::Separator;
    separator.id = "special:separator:1";
    items_.push_back(separator);

    if (!downloadsPath.empty() && std::filesystem::exists(downloadsPath)) {
        DockItem downloads;
        downloads.type = ItemType::Folder;
        downloads.id = "folder:" + downloadsPath;
        downloads.path = downloadsPath;
        downloads.label = "Downloads";
        downloads.showAs = 1;
        items_.push_back(downloads);
    }

    DockItem trash;
    trash.type = ItemType::Trash;
    trash.id = "special:trash";
    trash.label = trashLabel;
    items_.push_back(trash);
}

void DockModel::addItem(DockItem item, int index) {
    if (index < 0 || static_cast<size_t>(index) >= items_.size()) {
        items_.push_back(std::move(item));
    } else {
        items_.insert(items_.begin() + index, std::move(item));
    }
}

bool DockModel::removeAt(size_t index) {
    if (index >= items_.size()) {
        return false;
    }
    items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

bool DockModel::moveItem(size_t from, size_t to) {
    if (from >= items_.size() || to >= items_.size() || from == to) {
        return false;
    }
    DockItem item = std::move(items_[from]);
    items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(from));
    items_.insert(items_.begin() + static_cast<std::ptrdiff_t>(to), std::move(item));
    return true;
}

bool DockModel::containsApplication(const std::string &id) const {
    return indexOfApplication(id) >= 0;
}

int DockModel::indexOfApplication(const std::string &id) const {
    const auto wanted = lowerAscii(id);
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].isApplication() && lowerAscii(items_[i].id) == wanted) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

nlohmann::json DockModel::toJson() const {
    nlohmann::json array = nlohmann::json::array();
    for (const auto &item : items_) {
        nlohmann::json object{
            {"type", itemTypeToString(item.type)},
            {"id", item.id},
            {"path", item.path},
            {"label", item.label},
            {"icon", item.iconPath},
            {"pinned", item.pinned},
            {"stack", item.stack},
            {"showAs", item.showAs},
            {"arrangement", item.arrangement},
            {"guid", item.guid},
        };
        array.push_back(std::move(object));
    }
    return array;
}

void DockModel::fromJson(const nlohmann::json &json) {
    items_.clear();
    if (!json.is_array()) {
        return;
    }
    for (const auto &object : json) {
        if (!object.is_object()) {
            continue;
        }
        DockItem item;
        item.type = itemTypeFromString(object.value("type", "application"));
        item.id = object.value("id", "");
        item.path = object.value("path", "");
        item.label = object.value("label", "");
        item.iconPath = object.value("icon", "");
        item.pinned = object.value("pinned", true);
        item.stack = object.value("stack", true);
        item.showAs = object.value("showAs", 0);
        item.arrangement = object.value("arrangement", 1);
        item.guid = object.value("guid", 0);
        if (item.id.empty() && !item.path.empty()) {
            item.id = item.path;
        }
        items_.push_back(std::move(item));
    }
}

} // namespace revdock
