#pragma once

#include "anim/AnimatedValue.h"
#include "apps/AppIndex.h"
#include "config/Settings.h"
#include "core/Clock.h"
#include "core/Paths.h"
#include "layout/DockLayout.h"
#include "model/DockModel.h"
#include "render/IconCache.h"
#include "render/Renderer.h"
#include "render/TextRenderer.h"
#include "ui/LaunchpadLayout.h"
#include "ui/StackLayout.h"

#include <SDL3/SDL.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace revdock {

class Platform;

class Application {
public:
    Application();
    ~Application();

    int run(int argc, char **argv);
    int dumpSourceIcons(const std::string &directory);

private:
    enum class MenuAction {
        None,
        OpenItem,
        RevealItem,
        AddToDock,
        RemoveFromDock,
        QuitApplication,
        ForceQuitApplication,
        ShowAllWindows,
        MinimizeApplication,
        HideApplication,
        HideOthers,
        ShowApplication,
        OpenAtLogin,
        RunAppMenuItem,
        SetOrientation,
        SetMineffect,
        SetStackAuto,
        SetStackFan,
        SetStackGrid,
        SetStackArrangement,
        SetFolderDisplay,
        OpenTrash,
        EmptyTrash,
        ToggleMagnification,
        ToggleAutohide,
        ToggleMacIcons,
        ToggleLabels,
        RefreshApplications,
        OpenGrid,
        OpenSettings,
        QuitDock,
        CloseWindow,
    };

    struct MenuItem {
        std::string label;
        MenuAction action = MenuAction::None;
        std::string targetId;
        std::string argument;
        std::string secondary;
        std::string detail;
        std::string iconPath;
        std::vector<MenuItem> submenu;
        bool separator = false;
        bool header = false;
        bool enabled = true;
        bool checked = false;
    };

    struct MenuState {
        bool open = false;
        SDL_Window *window = nullptr;
        SDL_Renderer *renderer = nullptr;
        std::unique_ptr<Renderer> draw;
        std::unique_ptr<TextRenderer> text;
        std::vector<MenuItem> items;
        std::map<std::string, SDL_Texture *> icons;
        SDL_Texture *backdrop = nullptr;
        float width = 0.0f;
        float height = 0.0f;
        float panelHeight = 0.0f;
        float rowHeight = 24.0f;
        float fontPx = 13.0f;
        float arrowWidth = 16.0f;
        float arrowHeight = 8.0f;
        int hover = -1;
        float x = 0.0f;
        float y = 0.0f;
        bool hasChecks = false;
        bool hasIcons = false;
        int submenuParentRow = -1;
        int submenuPendingRow = -1;
        double submenuPendingAt = 0.0;
    };

    struct StackState {
        bool open = false;
        bool closing = false;
        double closedAt = 0.0;
        std::string pendingOpenPath;
        SDL_Window *window = nullptr;
        SDL_Renderer *renderer = nullptr;
        std::unique_ptr<Renderer> draw;
        std::unique_ptr<TextRenderer> text;
        std::unique_ptr<IconCache> icons;
        std::string path;
        std::string title;
        std::string itemId;
        int arrangement = 1;
        std::vector<DockItem> entries;
        int page = 0;
        int hover = -1;
        bool pressed = false;
        bool dragging = false;
        int dragIndex = -1;
        float dragStartX = 0.0f;
        float dragStartY = 0.0f;
        stack::Mode mode = stack::Mode::Grid;
        // Grid arrangement.
        int columns = 6;
        int rows = 4;
        float iconSize = 52.0f;
        float cellWidth = 86.0f;
        float cellHeight = 92.0f;
        SDL_FRect gridRect{};
        // Fan arrangement (see StackLayout.h). The fan is vertical: items are
        // stacked upward and curve to the right, with labels on their right.
        int fanCount = 0;
        float fanAnchorX = 0.0f;
        float fanBaseY = 0.0f;
        float fanItemSize = stack::kFanItemSize;
        float fanSpacing = stack::kFanSpacing;
        float fanLabelWidth = stack::kFanLabelWidth;
        double openedAt = 0.0;
        double hoverAt = 0.0;
        double dismissedAt = -10.0;
        std::string dismissedPath;
        SDL_Texture *folderEmpty = nullptr;
        SDL_Texture *folderNonempty = nullptr;
        float x = 0.0f;
        float y = 0.0f;
        float width = 420.0f;
        float height = 320.0f;
    };

    struct FolderPreview {
        std::vector<DockItem> entries;
        bool hasEntries = false;
        double refreshedAt = -100.0;
    };

    struct SettingsHit {
        SDL_FRect rect;
        std::string id;
    };

    struct SettingsState {
        bool open = false;
        SDL_Window *window = nullptr;
        SDL_Renderer *renderer = nullptr;
        std::unique_ptr<Renderer> draw;
        std::unique_ptr<TextRenderer> text;
        int hover = -1;
        int activeSlider = -1;
        std::vector<SettingsHit> hits;
    };

    struct MinimizeState {
        bool active = false;
        SDL_Window *window = nullptr;
        SDL_Renderer *renderer = nullptr;
        std::unique_ptr<Renderer> draw;
        SDL_Texture *texture = nullptr;
        SDL_FRect from{};
        SDL_FRect to{};
        double start = 0.0;
        double duration = 0.32;
        bool restoring = false;
        std::uintptr_t restoreHandle = 0;
    };

    struct GridState {
        bool open = false;
        bool closing = false;
        double openedAt = 0.0;
        SDL_Window *window = nullptr;
        SDL_Renderer *renderer = nullptr;
        std::unique_ptr<Renderer> draw;
        std::unique_ptr<TextRenderer> text;
        std::unique_ptr<IconCache> icons;
        SDL_Texture *wallpaper = nullptr;
        int page = 0;
        int hover = -1;
        int columns = 7;
        int rows = 5;
        std::set<std::string> attemptedIcons;
        bool firstFrame = true;
        // Persisted Launchpad tree (pages + folders) and the page that was
        // active before entering a folder or the search results.
        launchpad::Layout layout;
        bool layoutReady = false;
        int rootPage = 0;
        // Open folder (ECSBGroup / showGroup:).  folderId stays set during the
        // close animation and then resets to the root page.
        std::string folderId;
        bool folderClosing = false;
        AnimatedValue folderAnim{0.0f};
        SDL_FRect folderPanel{};
        SDL_FRect folderTitleRect{};
        // The folder tile body rect at open time; the panel expands from this
        // rounded rectangle instead of scaling from its own centre.
        SDL_FRect folderAnimFrom{};
        // Per-icon position animation.  Keys are "a:<app id>" / "f:<folder id>";
        // moving a gap or closing it animates every affected icon instead of
        // snapping.
        struct CellAnim {
            bool init = false;
            float x = 0.0f;
            float y = 0.0f;
            float fromX = 0.0f;
            float fromY = 0.0f;
            float toX = 0.0f;
            float toY = 0.0f;
            double start = -100.0;
            double duration = 0.18;
        };
        std::map<std::string, CellAnim> cellAnims;
        std::map<std::string, SDL_FPoint> returnFrom;
        // Search (ECTextInputLayer / SBSearchPage).
        std::string query;
        std::string composing;
        bool searchFocused = false;
        std::vector<std::string> searchResults;
        std::string searchKey;
        SDL_FRect searchRect{};
        SDL_FRect searchCloseRect{};
        double searchCaretAt = 0.0;
        // Fallback for platforms/IMEs that do not emit SDL_EVENT_TEXT_INPUT:
        // a printable KEY_DOWN is inserted after ~30 ms unless a TEXT_INPUT or
        // TEXT_EDITING event arrives first.
        std::string pendingText;
        double pendingTextAt = -1.0;
        std::string lastNativeText;
        double lastNativeAt = -100.0;
        double nativeCommitUntil = -100.0;
        // A dragged app that was dropped onto a folder shrinks into the folder
        // tile instead of vanishing.
        struct FolderDropAnim {
            bool active = false;
            std::string appId;
            SDL_FRect from{};
            SDL_FRect to{};
            double start = 0.0;
            double duration = 0.24;
        };
        FolderDropAnim folderDrop;
        // Folder title editing (springboard-edit-group-title).
        bool renaming = false;
        bool renameFresh = false;
        std::string renameText;
        std::string renameFolderId;
        // Drag source (application or whole folder) and the hovered drop
        // target.  ECSB*DragState maps onto these two small states.
        enum class DragTarget { None, Gap, App, Folder };
        int dragPage = -1;
        int dragSlot = -1;
        std::string dragAppId;
        bool dragIsFolder = false;
        DragTarget dragTarget = DragTarget::None;
        int dragTargetPage = -1;
        int dragTargetSlot = -1;
        std::string dragTargetAppId;
        std::string dragTargetFolderId;
        std::string dragHoverFolderId;
        std::string dragOriginFolderId;
        double dragHoverAt = -1.0;
        double dragTargetAt = -1.0;
        double dragEdgeAt = -1.0;
        int dragEdgeDirection = 0;
        double folderAwayAt = -1.0;
        // Launchpad artwork from Dock.app/Contents/Resources.
        SDL_Texture *searchBackground = nullptr;
        SDL_Texture *searchLoupe = nullptr;
        SDL_Texture *searchClose = nullptr;
        bool pressed = false;
        // Horizontal page swipe on the wallpaper.
        bool blankPressed = false;
        bool swiping = false;
        float swipeStartX = 0.0f;
        float swipeOffset = 0.0f;
        bool swipeAnimating = false;
        bool swipeEaseOut = false;
        // Precision touchpad two-finger horizontal swipe (WM_MOUSEHWHEEL).
        bool wheelSwiping = false;
        double wheelSwipeAt = -100.0;
        double wheelSmoothAt = -100.0;
        float wheelSwipeOffset = 0.0f;
        // Holding at the first/last page must not keep the rubber band stuck
        // when the touchpad driver keeps emitting release/inertia events.
        double wheelEdgeHoldAt = -1.0;
        bool wheelEdgeSuppressed = false;
        double wheelLastAt = -100.0;
        // Multi-touch (touchscreen, or macOS trackpad with
        // SDL_HINT_TRACKPAD_IS_TOUCH_ONLY): two-finger horizontal swipe.
        std::set<SDL_FingerID> fingers;
        bool fingerSwiping = false;
        double fingerSwipeAt = -100.0;
        float fingerSwipeOffset = 0.0f;
        double swipeFromTime = 0.0;
        double swipeDuration = 0.22;
        float swipeFrom = 0.0f;
        float swipeTo = 0.0f;
        // Clickable/draggable page indicator (ECPagerControlLayer).
        bool pagerPressed = false;
        float dotY = 0.0f;
        float dotStartX = 0.0f;
        float dotSpacing = 16.0f;
        float dotActiveSize = 9.0f;
        float dotInactiveSize = 7.0f;
        bool dragMoved = false;
        float dragStartX = 0.0f;
        float dragStartY = 0.0f;
        float dragX = 0.0f;
        float dragY = 0.0f;
        float iconSize = 64.0f;
        float cellWidth = 0.0f;
        float cellHeight = 0.0f;
        SDL_FRect gridRect{};
        std::string hint;
    };

    bool init();
    void shutdown();
    void buildDockWindow();
    void destroyDockWindow();
    void updateDisplay();
    void relayoutWindow();
    void createMenuWindow(MenuState &state, const char *title);
    void createGridWindow();
    void createSettingsWindow();
    void createStackWindow();
    void openStack(const DockItem &item, float globalX, float globalY);
    void closeStack(bool immediate = false);
    void finishClosingStack(bool launchPending = true);
    void handleStackEvent(const SDL_Event &event);
    void renderStack();
    void refreshStackEntries();
    void updateStackAnchor();
    void ensureMinimizeWindow();
    void startMinimizeAnimation(const std::string &execPath, const std::string &aumid,
                                std::uintptr_t handle, const std::string &title,
                                const SDL_Rect &rect, const std::vector<uint8_t> &rgba,
                                int width, int height);
    void restoreMinimizedItem(const DockItem &item);
    void startRestoreAnimation(const DockItem &item, std::uintptr_t handle,
                               const SDL_Rect &normalRect, const std::vector<uint8_t> &rgba,
                               int width, int height);
    void renderMinimizeAnimation();
    void finishMinimizeAnimation();
    void openSettings();
    void closeSettings();
    void handleSettingsEvent(const SDL_Event &event);
    void renderSettings();
    void applySettingsControl(const std::string &id, float x, bool pressed);
    void renderDock();
    void renderMenu();
    void renderMenuPanel(MenuState &state);
    void renderGrid();
    void renderTile(const DockItem &item, const TileGeometry &geometry, const LayoutResult &layout);
    void drawLabel(const DockItem &item, const TileGeometry &geometry);
    void drawFolderStackIcon(const DockItem &item, const SDL_FRect &icon,
                             SDL_Color tint, int logicalSize);
    FolderPreview &folderPreviewFor(const std::string &path);
    void handleEvent(const SDL_Event &event);
    void handleDockEvent(const SDL_Event &event);
    void handleMenuEvent(const SDL_Event &event);
    void handleMenuPanelEvent(MenuState &state, const SDL_Event &event, bool submenu);
    float menuContentHeight(const MenuState &state) const;
    float menuRowTop(const MenuState &state, int index) const;
    int menuRowAt(const MenuState &state, float y) const;
    void openSubmenu(const MenuItem &item, int row);
    void closeSubmenu();
    void clearMenuIcons(MenuState &state);
    SDL_Texture *menuIconTexture(MenuState &state, const MenuItem &item);
    void handleGridEvent(const SDL_Event &event);
    void update(double now);
    void updateAutohide(double now);
    void updateCursor();
    void updateRunning(double now);
    void refreshBackdrop();
    void saveConfig(bool force);
    void loadConfig();
    void markConfigDirty(double now);
    void openMenuForItem(size_t index, float globalX, float globalY);
    void openMenuForBackground(float globalX, float globalY);
    std::vector<MenuItem> dockMenuItems(const std::string &separatorId) const;
    void openMenu(std::vector<MenuItem> items, float anchorX, float tipY);
    void openMenuPanel(MenuState &state, std::vector<MenuItem> items, float anchorX,
                       float tipY, bool anchored, float left, float top);
    // Raise the Dock and any open overlay (menu/stack) above the Launchpad grid.
    void raiseDockLayers();
    SDL_Texture *buildPopoverTexture(SDL_Window *captureWindow, SDL_Renderer *renderer,
                                     float x, float y, float width, float height,
                                     float radius, float arrowWidth, float arrowHeight,
                                     float tipX, bool dark, bool allowCapture = true,
                                     bool triangle = true, float blurRadius = 8.0f,
                                     float tintOverride = -1.0f);
    void closeMenu();
    void executeMenu(const MenuItem &item);
    void openGrid();
    void closeGrid();
    void finishGridSwipe();
    // Decide the destination from the current swipe offset (page flip or snap back).
    void settleGridSwipe(float offset, float thresholdFraction);
    // Handles launchpad wheel input; horizontal deltas behave like a blank-area drag.
    bool handleGridWheel(float x, float y, SDL_MouseWheelDirection direction);
    void startGridSwipeAnimation(float from, float to, double duration, bool easeOut = false);
    void switchGridPage(int direction);
    void switchGridPageTo(int target);
    int gridDotAt(float x, float y) const;
    int gridPageCount() const;
    float gridIconSize() const;
    struct LaunchpadCell {
        bool folder = false;
        const launchpad::Item *item = nullptr;
        const AppInfo *app = nullptr;
        std::string appId;
        std::string folderId;
        std::string title;
    };
    struct LaunchWatch {
        std::set<std::string> windows;
        std::string foreground;
        double startedAt = 0.0;
    };
    int launchpadPageCount() const;
    LaunchpadCell launchpadCellAt(int page, int slot) const;
    const std::vector<std::string> &launchpadSearchResults();
    void computeGridMetrics();
    bool launchpadOpenFolder(const std::string &folderId, bool keepDrag = false);
    void launchpadCloseFolder(bool immediate = false, bool keepDrag = false);
    void launchpadBeginRename(bool fresh);
    void launchpadCommitRename(bool cancel);
    void launchpadMarkDirty();
    void handleLaunchpadTextCommit(const std::string &text);
    void bindLaunchAlias(const AppInfo &app, const LaunchWatch &watch);
    void launchpadUpdateDragTarget(float x, float y);
    void launchpadResetDrag();
    void launchpadFinishDrag(float localX, float localY, bool overDock);
    void drawLaunchpadSearchField(SDL_Color labelColor);
    void drawLaunchpadFolderTile(const launchpad::Item &folder, const SDL_FRect &icon,
                                 int iconPx);
    void drawLaunchpadFolderPanel();
    void toggleDockItem(const AppInfo &app);
    void rebuildVisibleItems();
    int hitTest(float x, float y) const;
    const std::vector<DockItem> &layoutItems() const;
    float dockHideOffset() const;
    bool modelContainsId(const std::string &id) const;
    int dragTargetModelIndex() const;
    void updateDockDragPreview();
    void updateDragHover(float x, float y);
    void handleFileDrop(const std::string &path, float x, float y);
    void updateGridDragPreview(float localX, float localY);
    void clearDragPreview();
    void finishDockDrag();
    const AppInfo *appForItem(const DockItem &item) const;
    bool isItemRunning(const DockItem &item) const;
    std::string fallbackFontPath() const;
    void launchItem(const DockItem &item);
    void activateOrLaunch(const AppInfo &app);
    void noteRecentApp(const std::string &id);
    bool shouldKeepAnimating() const;
    bool stackAnimating() const;
    void ensureGridResources();
    void ensureGridWallpaper();
    void ensureLaunchpadIcons();
    SDL_Texture *buildDockMaterial(int width, int height, float radius, bool dark);

    Paths paths_;
    Clock clock_;
    std::unique_ptr<Platform> platform_;
    std::unique_ptr<AppIndex> apps_;
    Settings settings_;
    DockModel model_;
    std::vector<DockItem> visibleItems_;
    DockLayout layout_;

    SDL_Window *dockWindow_ = nullptr;
    SDL_Renderer *dockRenderer_ = nullptr;
    std::unique_ptr<Renderer> renderer_;
    std::unique_ptr<TextRenderer> text_;
    std::unique_ptr<IconCache> icons_;
    SDL_Texture *dockMaterial_ = nullptr;
    std::string dockMaterialKey_;
    SDL_Texture *folderEmptyTexture_ = nullptr;
    SDL_Texture *folderNonemptyTexture_ = nullptr;
    std::map<std::string, FolderPreview> folderPreviews_;
    SDL_Texture *finderIcon_ = nullptr;
    std::map<int, SDL_Texture *> stackPills_;
    SDL_Texture *stackGridMaterial_ = nullptr;
    std::map<std::uintptr_t, SDL_Texture *> windowThumbnails_;
    std::string stackGridMaterialKey_;
    SDL_Texture *trashEmptyLight_ = nullptr;
    SDL_Texture *trashEmptyDark_ = nullptr;
    SDL_Texture *trashFullLight_ = nullptr;
    SDL_Texture *trashFullDark_ = nullptr;
    SDL_Texture *openInFinderBlack_ = nullptr;
    SDL_Texture *openInFinderWhite_ = nullptr;
    SDL_Texture *pileArrowBlack_ = nullptr;
    SDL_Texture *pileArrowWhite_ = nullptr;
    bool trashEmpty_ = true;
    double lastTrashPoll_ = -100.0;
    SDL_Texture *launchpadDark_ = nullptr;
    SDL_Texture *launchpadLight_ = nullptr;
    bool launchpadIconsLoaded_ = false;
    SDL_Texture *backdrop_ = nullptr;
    SDL_Texture *tooltipBackdrop_ = nullptr;
    std::string tooltipKey_;
    double tooltipRefreshAt_ = 0.0;
    bool backdropDirty_ = true;
    uint64_t backdropSignature_ = 0;
    int backdropMissCount_ = 0;
    double backdropCaptureAt_ = 0.0;
    double nextBackdropCapture_ = 0.0;

    SDL_DisplayID displayId_ = 0;
    SDL_Rect displayBounds_{};
    float contentScale_ = 1.0f;
    float windowWidth_ = 640.0f;
    float windowHeight_ = 180.0f;
    float mouseX_ = 0.0f;
    float mouseY_ = 0.0f;
    bool mouseInside_ = false;
    bool running_ = true;
    bool dirty_ = true;
    bool firstFrame_ = true;
    double now_ = 0.0;
    double lastConfigDirty_ = -100.0;
    double lastConfigWrite_ = -100.0;
    double lastCursorPoll_ = -100.0;
    // Global right-button fallback: SDL occasionally fails to route a
    // right-click to the Dock while Launchpad is open.
    bool pollRightDown_ = false;
    double lastDockButtonAt_ = -100.0;
    double lastRunningPoll_ = -100.0;
    double lastTopmostCheck_ = -100.0;
    int hovered_ = -1;
    int dragged_ = -1;
    bool dragging_ = false;
    bool dragMoved_ = false;
    std::vector<DockItem> dragPreviewItems_;
    int dragInsertIndex_ = -1;
    bool dockDragPreview_ = false;
    bool gridDragOverDock_ = false;
    bool fileDrag_ = false;
    std::string fileDragPath_;
    // Set while dragging files over a folder/Trash tile so those tiles can dim.
    std::string dropFolderId_;
    std::string dragHoverId_;
    double dragHoverAt_ = 0.0;
    bool dragHoverActivated_ = false;
    std::string flashId_;
    double flashAt_ = -10.0;
    float dragStartX_ = 0.0f;
    float dragStartY_ = 0.0f;
    float dragX_ = 0.0f;
    float dragY_ = 0.0f;

    AnimatedValue autohide_;
    AnimatedValue magEnvelope_{0.0f};
    AnimatedValue gridOpacity_{1.0f};
    double autohideDeadline_ = 0.0;
    double autohideRevealAt_ = 0.0;
    // Time the pointer first left both the Dock and an open folder stack.
    double stackAwayAt_ = 0.0;
    bool autohideHidden_ = false;
    bool cursorWasDown_ = false;
    bool stackWasDown_ = false;
    bool forceRender_ = false;
    bool modelDirty_ = false;
    bool launchpadDirty_ = false;
    LayoutResult layoutResult_;
    std::map<std::string, double> bounce_;
    std::vector<std::string> recentApps_;
    std::set<std::string> launching_;
    std::map<std::string, LaunchWatch> launchWatch_;
    std::map<std::string, float> shiftAnim_;
    bool reorderAnimating_ = false;
    double lastFrameTime_ = 0.0;
    int preloadIndex_ = 0;
    size_t preloadCount_ = 0;
    bool preloadDone_ = false;
    double nextPreloadAt_ = 0.0;
    double gridOpenAt_ = 0.0;
    double lastMinimizedPoll_ = -100.0;
    std::set<std::uintptr_t> minimizedSignature_;
    DockItem pendingMinimized_;
    bool hasPendingMinimized_ = false;
    // Handle whose Dock tile is hidden while its restore Genie plays.
    std::uintptr_t restoringHandle_ = 0;
    double restoringClearAt_ = 0.0;
    // Short slide of the existing tiles when the visible item set changes
    // (e.g. a minimized tile is inserted or removed).
    struct SlotAnim {
        float offset = 0.0f;
        double start = -100.0;
    };
    std::map<std::string, SlotAnim> slotAnim_;
    std::map<std::string, float> slotAnimFromScreen_;
    std::vector<std::string> lastVisibleIds_;
    bool slotAnimPending_ = false;
    double pendingMinimizedAt_ = 0.0;

    MenuState menu_;
    MenuState submenu_;
    GridState grid_;
    StackState stack_;
    SettingsState settingsUi_;
    MinimizeState minimize_;
    void *instanceHandle_ = nullptr;
    int instanceFd_ = -1;
    bool initialized_ = false;
    bool taskbarHiddenByUs_ = false;
    bool verbose_ = false;
};

} // namespace revdock
