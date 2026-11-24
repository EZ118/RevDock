#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace revdock {

class Settings {
public:
    Settings();

    void applyJson(const nlohmann::json &json);
    nlohmann::json toJson() const;

    float tileSize() const;
    float largeSize() const;
    bool magnification() const;
    std::string orientation() const;
    bool autohide() const;
    float autohideDelay() const;
    float autohideTimeModifier() const;
    bool launchAnimation() const;
    bool showIndicators() const;
    bool showLabels() const;
    bool showRecents() const;
    int recentCount() const;
    std::string mineffect() const;
    bool staticOnly() const;
    bool showHidden() const;
    std::string theme() const;
    bool reduceMotion() const;
    bool macIcons() const;
    // Launchpad two-finger swipe: movement per wheel notch (page widths) and
    // the fraction of the screen the gesture must travel before it flips.
    float launchpadSwipeScale() const;
    float launchpadSwipeThreshold() const;
    // Reverse-engineered springboard-columns / springboard-rows (defaults 7x5).
    int launchpadColumns() const;
    int launchpadRows() const;
    std::vector<std::string> recentApps() const;

    void setTileSize(float value);
    void setLargeSize(float value);
    void setMagnification(bool value);
    void setOrientation(const std::string &value);
    void setAutohide(bool value);
    void setAutohideDelay(float value);
    void setAutohideTimeModifier(float value);
    void setLaunchAnimation(bool value);
    void setShowIndicators(bool value);
    void setShowLabels(bool value);
    void setShowRecents(bool value);
    void setRecentCount(int value);
    void setMineffect(const std::string &value);
    void setStaticOnly(bool value);
    void setShowHidden(bool value);
    void setTheme(const std::string &value);
    void setReduceMotion(bool value);
    void setMacIcons(bool value);
    void setLaunchpadColumns(int value);
    void setLaunchpadRows(int value);
    void setRecentApps(const std::vector<std::string> &ids);

    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }
    void markDirty() { dirty_ = true; }
    void resetDefaults();

    float effectiveIconSize() const;
    float effectiveLargeSize() const;

private:
    nlohmann::json data_;
    bool dirty_ = false;

    float number(const char *key, float fallback) const;
    bool boolean(const char *key, bool fallback) const;
    int integer(const char *key, int fallback) const;
    std::string text(const char *key, const std::string &fallback) const;
    void setValue(const char *key, const nlohmann::json &value);
};

} // namespace revdock
