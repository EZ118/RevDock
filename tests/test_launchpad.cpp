#include "ui/LaunchpadLayout.h"

#include <doctest/doctest.h>

using revdock::launchpad::Layout;

TEST_CASE("launchpad pages fill and cascade") {
    Layout layout;
    CHECK(layout.pageCount() == 0);
    layout.ensurePage();
    CHECK(layout.pageCount() == 1);
    for (int i = 0; i < 8; ++i) {
        CHECK(layout.addAppToNextAvailable("app" + std::to_string(i), 3));
    }
    CHECK(layout.pageCount() == 3);
    CHECK(layout.pages[0].items.size() == 3);
    CHECK(layout.pages[1].items.size() == 3);
    CHECK(layout.pages[2].items.size() == 2);

    // Inserting into a full page pushes the overflow to the next page.
    CHECK(layout.insertApp("inserted", 0, 1, 3));
    CHECK(layout.pages[0].items[1].appId == "inserted");
    CHECK(layout.pages[1].items.front().appId == "app2");
    CHECK(layout.pageCount() == 3);
}

TEST_CASE("launchpad reorders root items across pages") {
    Layout layout;
    layout.ensurePage();
    for (int i = 0; i < 6; ++i) {
        layout.addAppToNextAvailable("app" + std::to_string(i), 3);
    }
    CHECK(layout.pages[0].items[0].appId == "app0");
    CHECK(layout.moveItem(0, 0, 1, 1, 3));
    CHECK(layout.pages[0].items[0].appId == "app1");
    CHECK(layout.pages[1].items[1].appId == "app0");
    CHECK(layout.pages[1].items.size() == 3);
}

TEST_CASE("launchpad folders hold and release applications") {
    Layout layout;
    layout.ensurePage();
    for (int i = 0; i < 4; ++i) {
        layout.addAppToNextAvailable("app" + std::to_string(i), 3);
    }
    CHECK(layout.makeFolderFromApps("app1", "app0", "folder-1",
                                    "Utilities", 3));
    const auto *folder = layout.findFolder("folder-1");
    REQUIRE(folder != nullptr);
    CHECK(folder->title == "Utilities");
    REQUIRE(folder->apps.size() == 2);
    CHECK(folder->apps[0] == "app0");
    CHECK(folder->apps[1] == "app1");
    CHECK(layout.containsApp("app1"));
    CHECK_FALSE(layout.findApp("app0", nullptr, nullptr));

    CHECK(layout.addAppToFolder("app2", "folder-1"));
    CHECK(layout.removeAppEverywhere("app2"));
    CHECK(layout.findFolder("folder-1")->apps.size() == 2);

    CHECK(layout.moveAppOutOfFolder("folder-1", "app0", 3));
    CHECK(layout.findApp("app0", nullptr, nullptr));
    CHECK(layout.findFolder("folder-1")->apps.size() == 1);
    CHECK(layout.moveAppOutOfFolder("folder-1", "app1", 3));
    CHECK(layout.findFolder("folder-1") == nullptr);
}

TEST_CASE("launchpad inserts into a folder at a specific index") {
    Layout layout;
    layout.ensurePage();
    for (int i = 0; i < 4; ++i) {
        layout.addAppToNextAvailable("app" + std::to_string(i), 10);
    }
    REQUIRE(layout.makeFolderFromApps("app1", "app0", "folder-1", "F", 10));
    CHECK(layout.moveAppIntoFolderAt("app2", "folder-1", 1, 10));
    const auto *folder = layout.findFolder("folder-1");
    REQUIRE(folder != nullptr);
    REQUIRE(folder->apps.size() == 3);
    CHECK(folder->apps[0] == "app0");
    CHECK(folder->apps[1] == "app2");
    CHECK(folder->apps[2] == "app1");
    CHECK_FALSE(layout.findApp("app2", nullptr, nullptr));
}

TEST_CASE("launchpad folder pagination order") {
    Layout layout;
    layout.ensurePage();
    for (int i = 0; i < 5; ++i) {
        layout.addAppToNextAvailable("app" + std::to_string(i), 10);
    }
    REQUIRE(layout.makeFolderFromApps("app1", "app0", "folder-1", "F", 10));
    layout.addAppToFolder("app2", "folder-1");
    layout.addAppToFolder("app3", "folder-1");
    CHECK(layout.moveFolderApp("folder-1", 1, 3));
    const auto *folder = layout.findFolder("folder-1");
    CHECK(folder->apps[3] == "app1");
}

TEST_CASE("launchpad reconcile preserves order and drops stale apps") {
    Layout layout;
    layout.ensurePage();
    layout.addAppToNextAvailable("old-a", 4);
    layout.addAppToNextAvailable("keep-b", 4);
    layout.insertApp("keep-b", 0, 1, 4);
    // Remove the duplicate introduced by insertApp.
    layout.removeItemAt(0, 2);
    REQUIRE(layout.pages[0].items.size() == 2);
    CHECK(layout.reconcile({"keep-b", "new-c", "new-d"}, 4));
    CHECK(layout.pages[0].items[0].appId == "keep-b");
    CHECK(layout.pages[0].items[1].appId == "new-c");
    CHECK(layout.pages[0].items[2].appId == "new-d");
    CHECK_FALSE(layout.containsApp("old-a"));
}

TEST_CASE("launchpad json round trip") {
    Layout layout;
    layout.ensurePage();
    layout.addAppToNextAvailable("app-a", 5);
    layout.addAppToNextAvailable("app-b", 5);
    layout.addAppToNextAvailable("app-c", 5);
    REQUIRE(layout.makeFolderFromApps("app-b", "app-a", "folder-9", "Work", 5));
    layout.addAppToFolder("app-c", "folder-9");
    const nlohmann::json json = layout.toJson();
    const Layout restored = Layout::fromJson(json);
    REQUIRE(restored.pageCount() == 1);
    REQUIRE(restored.pages[0].items.size() == 1);
    const auto *folder = restored.findFolder("folder-9");
    REQUIRE(folder != nullptr);
    CHECK(folder->title == "Work");
    CHECK(folder->apps.size() == 3);
}

TEST_CASE("launchpad search ranking") {
    CHECK(Layout::searchScore("Safari", "sa") == 1);
    CHECK(Layout::searchScore("Safari", "Safari") == 0);
    CHECK(Layout::searchScore("Microsoft Edge", "edge") == 2);
    CHECK(Layout::searchScore("Notepad", "pad") == 3);
    CHECK(Layout::searchScore("Notepad", "zzz") == -1);
}

TEST_CASE("launchpad initial layout groups Start Menu folders") {
    Layout layout;
    std::vector<revdock::launchpad::InitialApp> apps = {
        {"utilities-calc", "Calculator", "Utilities"},
        {"utilities-notes", "Notes", "Utilities"},
        {"solo-app", "Solo App", "Solo"},
        {"alpha", "Alpha", ""},
        {"uwp-app", "Zulu", ""},
    };
    layout.buildInitial(apps, 2);
    CHECK(layout.pageCount() == 2);
    CHECK(layout.pages[0].items.size() == 2);
    CHECK(layout.pages[1].items.size() == 2);

    const auto *folder = layout.findFolder("folder-auto-0");
    REQUIRE(folder != nullptr);
    CHECK(folder->title == "Utilities");
    CHECK(folder->apps.size() == 2);

    // A one-app Start Menu folder becomes a standalone tile, not a folder;
    // UWP apps (empty group) always stay standalone.
    bool sawSolo = false;
    bool sawUwp = false;
    for (const auto &page : layout.pages) {
        for (const auto &item : page.items) {
            if (!item.folder && item.appId == "solo-app") {
                sawSolo = true;
            }
            if (!item.folder && item.appId == "uwp-app") {
                sawUwp = true;
            }
            if (item.folder) {
                CHECK(item.title != "Solo");
            }
        }
    }
    CHECK(sawSolo);
    CHECK(sawUwp);
}
