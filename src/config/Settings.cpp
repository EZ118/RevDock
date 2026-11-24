#include "config/Settings.h"

#include "core/Types.h"

#include <algorithm>

namespace revdock {

Settings::Settings() {
    resetDefaults();
}

void Settings::resetDefaults() {
    data_ = nlohmann::json::object();
    data_["tilesize"] = 48.0f;
    data_["largesize"] = 64.0f;
    data_["magnification"] = true;
    data_["mac-icons"] = true;
    data_["orientation"] = "bottom";
    data_["autohide"] = false;
    data_["autohide-delay"] = 0.0f;
    data_["autohide-time-modifier"] = 1.0f;
    data_["launchanim"] = true;
    data_["show-process-indicators"] = true;
    data_["labels"] = true;
    data_["show-recents"] = true;
    data_["show-recent-count"] = 3;
    data_["mineffect"] = "genie";
    data_["static-only"] = false;
    data_["showhidden"] = false;
    data_["theme"] = "dark";
    data_["reduce-motion"] = false;
    data_["launchpad-swipe-scale"] = 0.034f;
    data_["launchpad-swipe-threshold"] = 0.18f;
    data_["springboard-columns"] = 7;
    data_["springboard-rows"] = 5;
    dirty_ = true;
}

void Settings::applyJson(const nlohmann::json &json) {
    if (!json.is_object()) {
        return;
    }
    for (auto it = json.begin(); it != json.end(); ++it) {
        data_[it.key()] = it.value();
    }
    data_["tilesize"] = clampf(number("tilesize", 48.0f), 16.0f, 128.0f);
    data_["largesize"] = clampf(number("largesize", 64.0f), number("tilesize", 48.0f), 192.0f);
    const std::string orientationValue = text("orientation", "bottom");
    if (orientationValue != "bottom" && orientationValue != "left" && orientationValue != "right") {
        data_["orientation"] = "bottom";
    }
    const std::string effect = text("mineffect", "genie");
    if (effect != "genie" && effect != "scale" && effect != "suck") {
        data_["mineffect"] = "genie";
    }
    // Removed option: the Dock now reserves space based on auto-hide.
    data_.erase("reserve-work-area");
    dirty_ = false;
}

nlohmann::json Settings::toJson() const {
    return data_;
}

float Settings::number(const char *key, float fallback) const {
    auto it = data_.find(key);
    if (it == data_.end() || !it->is_number()) {
        return fallback;
    }
    return it->get<float>();
}

bool Settings::boolean(const char *key, bool fallback) const {
    auto it = data_.find(key);
    if (it == data_.end() || !it->is_boolean()) {
        return fallback;
    }
    return it->get<bool>();
}

int Settings::integer(const char *key, int fallback) const {
    auto it = data_.find(key);
    if (it == data_.end() || !it->is_number_integer()) {
        return fallback;
    }
    return it->get<int>();
}

std::string Settings::text(const char *key, const std::string &fallback) const {
    auto it = data_.find(key);
    if (it == data_.end() || !it->is_string()) {
        return fallback;
    }
    return it->get<std::string>();
}

void Settings::setValue(const char *key, const nlohmann::json &value) {
    if (data_[key] == value) {
        return;
    }
    data_[key] = value;
    dirty_ = true;
}

float Settings::tileSize() const { return clampf(number("tilesize", 48.0f), 16.0f, 128.0f); }
float Settings::largeSize() const { return clampf(number("largesize", 64.0f), tileSize(), 192.0f); }
bool Settings::magnification() const { return boolean("magnification", true); }
bool Settings::macIcons() const { return boolean("mac-icons", true); }

std::vector<std::string> Settings::recentApps() const {
    std::vector<std::string> result;
    auto found = data_.find("recent-apps");
    if (found != data_.end() && found->is_array()) {
        for (const auto &value : *found) {
            if (value.is_string()) {
                result.push_back(value.get<std::string>());
            }
        }
    }
    return result;
}
std::string Settings::orientation() const { return text("orientation", "bottom"); }
bool Settings::autohide() const { return boolean("autohide", false); }
float Settings::autohideDelay() const { return clampf(number("autohide-delay", 0.0f), 0.0f, 10.0f); }
float Settings::autohideTimeModifier() const {
    return clampf(number("autohide-time-modifier", 1.0f), 0.1f, 5.0f);
}
bool Settings::launchAnimation() const { return boolean("launchanim", true); }
bool Settings::showIndicators() const { return boolean("show-process-indicators", true); }
bool Settings::showLabels() const { return boolean("labels", true); }
bool Settings::showRecents() const { return boolean("show-recents", true); }
int Settings::recentCount() const { return std::clamp(integer("show-recent-count", 3), 0, 10); }
std::string Settings::mineffect() const { return text("mineffect", "genie"); }
bool Settings::staticOnly() const { return boolean("static-only", false); }
bool Settings::showHidden() const { return boolean("showhidden", false); }
std::string Settings::theme() const { return text("theme", "dark"); }
bool Settings::reduceMotion() const { return boolean("reduce-motion", false); }

void Settings::setTileSize(float value) {
    value = clampf(value, 16.0f, 128.0f);
    setValue("tilesize", value);
    if (largeSize() < value) {
        setValue("largesize", value);
    }
}
void Settings::setLargeSize(float value) { setValue("largesize", clampf(value, tileSize(), 192.0f)); }
void Settings::setMagnification(bool value) { setValue("magnification", value); }
void Settings::setMacIcons(bool value) { setValue("mac-icons", value); }

void Settings::setRecentApps(const std::vector<std::string> &ids) {
    nlohmann::json values = nlohmann::json::array();
    for (const auto &id : ids) {
        values.push_back(id);
    }
    setValue("recent-apps", values);
}
void Settings::setOrientation(const std::string &value) {
    if (value == "bottom" || value == "left" || value == "right") {
        setValue("orientation", value);
    }
}
void Settings::setAutohide(bool value) { setValue("autohide", value); }
void Settings::setAutohideDelay(float value) { setValue("autohide-delay", clampf(value, 0.0f, 10.0f)); }
void Settings::setAutohideTimeModifier(float value) {
    setValue("autohide-time-modifier", clampf(value, 0.1f, 5.0f));
}
void Settings::setLaunchAnimation(bool value) { setValue("launchanim", value); }
void Settings::setShowIndicators(bool value) { setValue("show-process-indicators", value); }
void Settings::setShowLabels(bool value) { setValue("labels", value); }
void Settings::setShowRecents(bool value) { setValue("show-recents", value); }
void Settings::setRecentCount(int value) { setValue("show-recent-count", std::clamp(value, 0, 10)); }
void Settings::setMineffect(const std::string &value) {
    if (value == "genie" || value == "scale" || value == "suck" || value == "none") {
        setValue("mineffect", value);
    }
}
void Settings::setStaticOnly(bool value) { setValue("static-only", value); }
void Settings::setShowHidden(bool value) { setValue("showhidden", value); }
void Settings::setTheme(const std::string &value) {
    if (value == "dark" || value == "light" || value == "system") {
        setValue("theme", value);
    }
}
void Settings::setReduceMotion(bool value) { setValue("reduce-motion", value); }

float Settings::launchpadSwipeScale() const {
    return clampf(number("launchpad-swipe-scale", 0.034f), 0.01f, 2.0f);
}

float Settings::launchpadSwipeThreshold() const {
    return clampf(number("launchpad-swipe-threshold", 0.18f), 0.05f, 0.95f);
}

int Settings::launchpadColumns() const {
    return std::clamp(integer("springboard-columns", 7), 2, 20);
}

int Settings::launchpadRows() const {
    return std::clamp(integer("springboard-rows", 5), 2, 20);
}

void Settings::setLaunchpadColumns(int value) {
    setValue("springboard-columns", std::clamp(value, 2, 20));
}

void Settings::setLaunchpadRows(int value) {
    setValue("springboard-rows", std::clamp(value, 2, 20));
}

float Settings::effectiveIconSize() const {
    return tileSize();
}

float Settings::effectiveLargeSize() const {
    return magnification() ? largeSize() : tileSize();
}

} // namespace revdock
