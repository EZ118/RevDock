#include <doctest/doctest.h>

#include "config/Settings.h"
#include "core/Types.h"
#include "layout/DockLayout.h"
#include "ui/StackLayout.h"

#include <nlohmann/json.hpp>

using namespace revdock;

TEST_CASE("cosine ease is symmetric and clamped") {
    CHECK(easeInOut(0.0f) == doctest::Approx(0.0f));
    CHECK(easeInOut(1.0f) == doctest::Approx(1.0f));
    CHECK(easeInOut(0.5f) == doctest::Approx(0.5f));
    CHECK(easeInOut(-1.0f) == doctest::Approx(0.0f));
    CHECK(easeInOut(2.0f) == doctest::Approx(1.0f));
}

TEST_CASE("settings clamp and round-trip") {
    Settings settings;
    settings.setTileSize(500.0f);
    CHECK(settings.tileSize() == doctest::Approx(128.0f));
    settings.setLargeSize(1000.0f);
    CHECK(settings.largeSize() <= 192.0f);
    settings.setOrientation("left");
    CHECK(settings.orientation() == "left");
    settings.setOrientation("diagonal");
    CHECK(settings.orientation() == "left");

    const nlohmann::json saved = settings.toJson();
    Settings restored;
    restored.applyJson(saved);
    CHECK(restored.tileSize() == doctest::Approx(settings.tileSize()));
    CHECK(restored.magnification() == settings.magnification());
}

TEST_CASE("layout keeps the dock centered and fits the display") {
    Settings settings;
    settings.setTileSize(48.0f);
    settings.setLargeSize(64.0f);
    settings.setMagnification(false);

    std::vector<DockItem> items;
    for (int i = 0; i < 30; ++i) {
        DockItem item;
        item.type = ItemType::Application;
        item.id = "app:" + std::to_string(i);
        item.label = "App";
        items.push_back(item);
    }

    DockLayout layout;
    const LayoutResult result = layout.compute(items, settings, 800.0f, 140.0f, 400.0f, 100.0f, true);
    CHECK(result.tiles.size() == items.size());
    CHECK(result.bar.w <= 800.0f + 1.0f);
    CHECK(result.tiles.front().cell.x >= 0.0f);
    CHECK(result.tiles.back().cell.x + result.tiles.back().cell.w <= 800.0f);
}

TEST_CASE("magnification enlarges the tile under the pointer") {
    Settings settings;
    settings.setTileSize(48.0f);
    settings.setLargeSize(96.0f);
    settings.setMagnification(true);

    std::vector<DockItem> items(7);
    for (auto &item : items) {
        item.type = ItemType::Application;
        item.label = "App";
    }
    DockLayout layout;
    const float centerX = 400.0f;
    const LayoutResult result = layout.compute(items, settings, 800.0f, 160.0f, centerX, 100.0f, true);
    CHECK(result.tiles.size() == 7);
    const auto &middle = result.tiles[3];
    CHECK(middle.scale > 1.0f);
    CHECK(middle.icon.w > settings.tileSize());
    CHECK(middle.icon.w <= settings.largeSize() + 1.0f);
}

TEST_CASE("separator is narrower than an icon tile") {
    Settings settings;
    settings.setMagnification(false);
    std::vector<DockItem> items(2);
    items[0].type = ItemType::Application;
    items[1].type = ItemType::Separator;
    DockLayout layout;
    const LayoutResult result = layout.compute(items, settings, 400.0f, 120.0f, 0.0f, 0.0f, false);
    CHECK(result.tiles[1].cell.w < result.tiles[0].cell.w);
}

TEST_CASE("minimized windows keep an icon-sized slot") {
    Settings settings;
    settings.setMagnification(false);
    std::vector<DockItem> items(3);
    items[0].type = ItemType::Application;
    items[1].type = ItemType::MinimizedWindow;
    items[1].previewAspect = 1.8f;
    items[2].type = ItemType::Trash;
    DockLayout layout;
    const LayoutResult result =
        layout.compute(items, settings, 600.0f, 120.0f, 0.0f, 0.0f, false);
    REQUIRE(result.tiles.size() == 3);
    // The thumbnail is fitted inside this slot; the slot itself must not grow
    // into a wide window strip.
    CHECK(result.tiles[1].icon.w == doctest::Approx(result.tiles[0].icon.w));
    CHECK(result.tiles[1].icon.h == doctest::Approx(result.tiles[0].icon.h));
}

TEST_CASE("fan icons stack upwards and curve to the right") {
    const auto bottom = stack::fanSlot(0, 5, 100.0f, 400.0f);
    const auto top = stack::fanSlot(4, 5, 100.0f, 400.0f);
    CHECK(bottom.centerY > top.centerY);
    CHECK(bottom.centerX < top.centerX);
    CHECK(bottom.sagitta < top.sagitta);
    CHECK(bottom.angle > 0.0f);
    CHECK(bottom.angle < top.angle);
    // The first icon sits half a spacing above the column bottom.
    CHECK(bottom.centerY == doctest::Approx(400.0f - stack::kFanSpacing * 0.5f));
    // Every step is one spacing.
    const auto second = stack::fanSlot(1, 5, 100.0f, 400.0f);
    CHECK(top.centerY == doctest::Approx(bottom.centerY - 4.0f * stack::kFanSpacing));
    CHECK(second.centerX > bottom.centerX);
    // A 3000 pt radius is deliberately gentle at this size.
    CHECK(top.sagitta < 25.0f);
}

TEST_CASE("fan mode falls back to a grid when the folder is too large") {
    CHECK(stack::modeFor(1, 5) == stack::Mode::Fan);
    CHECK(stack::modeFor(0, 5) == stack::Mode::Fan);
    CHECK(stack::modeFor(2, 5) == stack::Mode::Grid);
    CHECK(stack::modeFor(3, 5) == stack::Mode::Grid);
    CHECK(stack::modeFor(1, 18) == stack::Mode::Grid);
    CHECK(stack::modeFor(0, 18) == stack::Mode::Grid);
    CHECK(stack::modeFor(1, 5, 300.0f) == stack::Mode::Grid);
}

TEST_CASE("fan hit testing snaps to the nearest icon plate") {
    const float anchorX = 100.0f;
    const float baseY = 500.0f;
    const int count = 5;
    for (int i = 0; i < count; ++i) {
        const auto slot = stack::fanSlot(i, count, anchorX, baseY);
        CHECK(stack::fanIndexAt(slot.centerX, slot.centerY, count, anchorX, baseY) == i);
    }
    CHECK(stack::fanIndexAt(5.0f, 5.0f, count, anchorX, baseY) == -1);
}
