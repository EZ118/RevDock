#include "ui/Application.h"

#include "core/Blur.h"
#include "core/Icns.h"
#include "core/FileUtil.h"
#include "core/Log.h"
#include "apps/AppMenu.h"
#include "core/Utf.h"
#include "platform/Platform.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace revdock {
namespace {

constexpr float kMenuRowHeight = 24.0f;
constexpr float kMenuSeparatorHeight = 9.0f;
constexpr float kMenuFontSize = 13.0f;
constexpr float kMenuPadding = 4.0f;
constexpr const char *kDragSlotId = "__revdock_drag_slot__";
constexpr float kDockBottomInset = 3.0f;

// Displayed window-thumbnail rectangle inside a minimized-window tile. The
// Genie animation uses the same rect so its endpoint matches the final tile.
SDL_FRect minimizedPreviewRect(const SDL_FRect &icon, float aspect) {
    SDL_FRect r{icon.x - 1.0f, icon.y - 1.0f, icon.w + 2.0f, icon.h + 2.0f};
    if (aspect <= 0.01f) {
        aspect = 1.0f;
    }
    const float h = std::min(r.h, r.w / aspect);
    const float w = h * aspect;
    return SDL_FRect{r.x + (r.w - w) * 0.5f, r.y + (r.h - h) * 0.5f, w, h};
}

bool isDragSlot(const DockItem &item) { return item.id == kDragSlotId; }

// Windows leaves shell bookkeeping files in user folders.  macOS never shows
// .DS_Store in a Dock stack either, so hide the Windows equivalents.
bool isShellMetadataFile(const std::string &name) {
    const std::string lower = lowerAscii(name);
    return lower == "desktop.ini" || lower == "thumbs.db" ||
           lower == ".ds_store";
}

// Printable text for a US-layout key press.  Used only as a fallback when SDL
// does not deliver SDL_EVENT_TEXT_INPUT (e.g. some IME/driver combinations).
std::string printableTextFor(SDL_Keycode key, SDL_Keymod mod) {
    const bool shift = (mod & SDL_KMOD_SHIFT) != 0;
    const bool caps = (mod & SDL_KMOD_CAPS) != 0;
    if (key >= SDLK_A && key <= SDLK_Z) {
        char ch = static_cast<char>('a' + (key - SDLK_A));
        if (shift != caps) {
            ch = static_cast<char>(ch - 'a' + 'A');
        }
        return std::string(1, ch);
    }
    if (key >= SDLK_0 && key <= SDLK_9) {
        static const char *shifted = ")!@#$%^&*(";
        return std::string(1, shift ? shifted[key - SDLK_0]
                                    : static_cast<char>('0' + (key - SDLK_0)));
    }
    if (key == SDLK_SPACE) {
        return " ";
    }
    const char *plain = nullptr;
    const char *withShift = nullptr;
    switch (key) {
    case SDLK_MINUS: plain = "-"; withShift = "_"; break;
    case SDLK_EQUALS: plain = "="; withShift = "+"; break;
    case SDLK_LEFTBRACKET: plain = "["; withShift = "{"; break;
    case SDLK_RIGHTBRACKET: plain = "]"; withShift = "}"; break;
    case SDLK_BACKSLASH: plain = "\\"; withShift = "|"; break;
    case SDLK_SEMICOLON: plain = ";"; withShift = ":"; break;
    case SDLK_APOSTROPHE: plain = "'"; withShift = "\""; break;
    case SDLK_GRAVE: plain = "`"; withShift = "~"; break;
    case SDLK_COMMA: plain = ","; withShift = "<"; break;
    case SDLK_PERIOD: plain = "."; withShift = ">"; break;
    case SDLK_SLASH: plain = "/"; withShift = "?"; break;
    default: break;
    }
    if (plain) {
        return shift ? withShift : plain;
    }
    return {};
}

bool isDarkTheme(const Settings &settings) {
    if (settings.theme() == "light") {
        return false;
    }
    if (settings.theme() == "system") {
        return SDL_GetSystemTheme() != SDL_SYSTEM_THEME_LIGHT;
    }
    return true;
}

SDL_Color materialColor(bool dark) {
    return dark ? rgba(36, 36, 36, 224) : rgba(242, 242, 242, 232);
}

SDL_Color primaryText(bool dark) {
    return dark ? rgba(245, 245, 248, 245) : rgba(24, 24, 28, 240);
}

SDL_Color secondaryText(bool dark) {
    return dark ? rgba(220, 220, 226, 180) : rgba(50, 50, 56, 180);
}

bool roundedContains(float x, float y, float width, float height, float radius) {
    if (x < 0.0f || y < 0.0f || x >= width || y >= height) {
        return false;
    }
    if (x < radius && y < radius) {
        return (x - radius) * (x - radius) + (y - radius) * (y - radius) <= radius * radius;
    }
    if (x >= width - radius && y < radius) {
        return (x - (width - radius)) * (x - (width - radius)) + (y - radius) * (y - radius) <=
               radius * radius;
    }
    if (x < radius && y >= height - radius) {
        return (x - radius) * (x - radius) +
                   (y - (height - radius)) * (y - (height - radius)) <=
               radius * radius;
    }
    if (x >= width - radius && y >= height - radius) {
        return (x - (width - radius)) * (x - (width - radius)) +
                   (y - (height - radius)) * (y - (height - radius)) <=
               radius * radius;
    }
    return true;
}

SDL_Surface *makePillSurface(int width, int height, float radius) {
    SDL_Surface *surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
    if (!surface) {
        return nullptr;
    }
    SDL_LockSurface(surface);
    auto *pixels = static_cast<uint8_t *>(surface->pixels);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int hits = 0;
            for (int sy = 0; sy < 4; ++sy) {
                for (int sx = 0; sx < 4; ++sx) {
                    if (roundedContains(x + (sx + 0.5f) * 0.25f, y + (sy + 0.5f) * 0.25f,
                                        static_cast<float>(width), static_cast<float>(height),
                                        radius)) {
                        ++hits;
                    }
                }
            }
            uint8_t *pixel = pixels + static_cast<size_t>(y) * surface->pitch + x * 4;
            pixel[0] = 255;
            pixel[1] = 255;
            pixel[2] = 255;
            pixel[3] = static_cast<uint8_t>(hits * 255 / 16);
        }
    }
    SDL_UnlockSurface(surface);
    return surface;
}

bool triangleContains(float x, float y, float tipX, float baseY, float halfWidth, float tipY) {
    if (y < baseY || y > tipY) {
        return false;
    }
    const float t = (y - baseY) / std::max(0.001f, tipY - baseY);
    const float half = halfWidth * (1.0f - t);
    return std::abs(x - tipX) <= half;
}

Uint32 eventWindowID(const SDL_Event &event) {
    if (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST) {
        return event.window.windowID;
    }
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        return event.key.windowID;
    case SDL_EVENT_MOUSE_MOTION:
        return event.motion.windowID;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return event.button.windowID;
    case SDL_EVENT_MOUSE_WHEEL:
        return event.wheel.windowID;
    case SDL_EVENT_DROP_FILE:
    case SDL_EVENT_DROP_TEXT:
    case SDL_EVENT_DROP_BEGIN:
    case SDL_EVENT_DROP_COMPLETE:
    case SDL_EVENT_DROP_POSITION:
        return event.drop.windowID;
    default:
        return 0;
    }
}

} // namespace

Application::Application() = default;

Application::~Application() {
    shutdown();
}

int Application::dumpSourceIcons(const std::string &directory) {
    SDL_Init(SDL_INIT_VIDEO);
    paths_.initialize();
    platform_ = createPlatform();
    apps_ = std::make_unique<AppIndex>(*platform_, paths_.appsCacheFile());
    apps_->refresh();
    std::filesystem::create_directories(directory);
    int saved = 0;
    for (const AppInfo &app : apps_->apps()) {
        SDL_Surface *surface = platform_->loadIcon(app, 256);
        if (!surface) {
            continue;
        }
        std::string name = app.name;
        for (char &c : name) {
            if (c == ' ' || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                c == '"' || c == '<' || c == '>' || c == '|') {
                c = '_';
            }
        }
        const std::string path = (std::filesystem::path(directory) / (name + ".png")).string();
        if (IMG_SavePNG(surface, path.c_str())) {
            ++saved;
        }
        SDL_DestroySurface(surface);
    }
    RD_LOG_INFO("dumped %d source icons to %s", saved, directory.c_str());
    return 0;
}

int Application::run(int argc, char **argv) {
    bool startWithSettings = false;
    bool startWithGrid = false;
    double gridDelay = -1.0;
    std::string dumpIconsDir;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--verbose" || arg == "-v") {
            verbose_ = true;
        } else if (arg == "--settings") {
            startWithSettings = true;
        } else if (arg == "--grid") {
            startWithGrid = true;
        } else if (arg == "--grid-delay" && i + 1 < argc) {
            startWithGrid = true;
            gridDelay = std::max(0.1, std::atof(argv[++i]));
        } else if (arg == "--dump-icons" && i + 1 < argc) {
            dumpIconsDir = argv[++i];
        }
    }
    if (!dumpIconsDir.empty()) {
        return dumpSourceIcons(dumpIconsDir);
    }

    if (!init()) {
        return 1;
    }
    if (startWithSettings) {
        openSettings();
    }
    if (startWithGrid) {
        if (gridDelay > 0.0) {
            gridOpenAt_ = clock_.now() + gridDelay;
        } else {
            openGrid();
        }
    }

    while (running_) {
        now_ = clock_.now();
        SDL_Event event;
        int timeout = shouldKeepAnimating()
                          ? 4
                          : ((menu_.open || stack_.open || fileDrag_ || !dragHoverId_.empty())
                                 ? 12
                                 : 100);
        if (!shouldKeepAnimating() && dockWindow_ && autohide_.value() < 0.9f) {
            // Wake up in time for the next live-blur capture.
            const double untilCapture = nextBackdropCapture_ - now_;
            timeout = std::min(timeout, std::max(8, static_cast<int>(untilCapture * 1000.0)));
        }
        if (!shouldKeepAnimating() && settings_.autohide() && autohide_.value() >= 0.9f) {
            // Poll the last screen row often enough to honor the 100 ms dwell.
            timeout = std::min(timeout, 30);
        }
        const bool gotEvent = SDL_WaitEventTimeout(&event, timeout);
        if (gotEvent) {
            do {
                handleEvent(event);
            } while (SDL_PollEvent(&event));
        }
        now_ = clock_.now();
        update(now_);
        if (dirty_ || forceRender_) {
            renderDock();
            if (menu_.open) {
                renderMenu();
            }
            if (grid_.open) {
                renderGrid();
            }
            if (stack_.open) {
                renderStack();
            }
            if (settingsUi_.open) {
                renderSettings();
            }
            dirty_ = false;
            forceRender_ = false;
        }
        if (minimize_.active) {
            renderMinimizeAnimation();
        }
        saveConfig(false);
    }

    saveConfig(true);
    return 0;
}

bool Application::init() {
    SDL_SetAppMetadata("RevDock", REVDOCK_VERSION, "com.revdock.app");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "RevDock", SDL_GetError(), nullptr);
        return false;
    }
    if (!TTF_Init()) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "RevDock",
                                 "SDL_ttf initialization failed", nullptr);
        return false;
    }

    paths_.initialize();
    logInit(paths_.logFile(), verbose_);
#ifdef _WIN32
    instanceHandle_ = CreateMutexW(nullptr, TRUE, L"Local\\RevDock.SingleInstance");
    if (instanceHandle_ && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(static_cast<HANDLE>(instanceHandle_));
        instanceHandle_ = nullptr;
        RD_LOG_INFO("Another RevDock instance is already running");
        return false;
    }
#else
    const std::filesystem::path lockFile = paths_.stateDir / "instance.lock";
    instanceFd_ = open(lockFile.c_str(), O_CREAT | O_RDWR, 0600);
    if (instanceFd_ >= 0 && flock(instanceFd_, LOCK_EX | LOCK_NB) != 0) {
        close(instanceFd_);
        instanceFd_ = -1;
        RD_LOG_INFO("Another RevDock instance is already running");
        return false;
    }
#endif
    RD_LOG_INFO("RevDock %s starting", REVDOCK_VERSION);
    RD_LOG_INFO("config=%s cache=%s assets=%s", paths_.configDir.string().c_str(),
                paths_.cacheDir.string().c_str(), paths_.assetDir.string().c_str());

    platform_ = createPlatform();
    if (!platform_) {
        RD_LOG_ERROR("No platform implementation available");
        return false;
    }
    apps_ = std::make_unique<AppIndex>(*platform_, paths_.appsCacheFile());

    loadConfig();
    recentApps_ = settings_.recentApps();
    model_.createDefaultsIfEmpty(platform_->downloadsPath(), platform_->trashLabel());
    // Existing configs predate the Finder tile: add it once, leftmost. It can
    // be moved like Launchpad but never removed/unpinned.
    bool hasFinder = false;
    for (const auto &entry : model_.items()) {
        if (entry.type == ItemType::Finder) {
            hasFinder = true;
            break;
        }
    }
    if (!hasFinder) {
        DockItem finder;
        finder.type = ItemType::Finder;
        finder.id = "special:finder";
        finder.label = "Finder";
        finder.pinned = true;
        model_.addItem(std::move(finder), 0);
        modelDirty_ = true;
    }

    updateDisplay();
    buildDockWindow();
    if (!dockWindow_) {
        return false;
    }

    const std::filesystem::path regular = paths_.assetDir / "fonts" / "Inter" / "Inter-Regular.ttf";
    const std::string fallback = fallbackFontPath();
    renderer_ = std::make_unique<Renderer>(dockRenderer_, contentScale_);
    text_ = std::make_unique<TextRenderer>(dockRenderer_, regular.string(), fallback);
    icons_ = std::make_unique<IconCache>(dockRenderer_, *platform_, *apps_, contentScale_);
    icons_->setMacStyle(settings_.macIcons());
    ensureLaunchpadIcons();
    nextPreloadAt_ = clock_.now() + 1.5;

    apps_->refresh();
    // Finder is the file manager; drop any stale pinned shortcut that resolves
    // to Explorer/another platform file manager.
    for (auto it = model_.items().begin(); it != model_.items().end();) {
        if (it->isApplication() && platform_->isFileManagerShortcut(it->path)) {
            it = model_.items().erase(it);
            modelDirty_ = true;
            markConfigDirty(clock_.now());
        } else {
            ++it;
        }
    }
    rebuildVisibleItems();
    relayoutWindow();
    ensureGridResources();
    // Create the Genie overlay up front so opening it at minimize time costs
    // no window creation. It stays hidden while idle: a full-screen always-on-top
    // sheet can otherwise swallow clicks in other applications.
    ensureMinimizeWindow();
    platform_->setSystemMinimizeAnimationsEnabled(false);
    platform_->setTaskbarVisible(false);
    taskbarHiddenByUs_ = true;
    updateDisplay();
    ensureGridWallpaper();
    relayoutWindow();
    forceRender_ = true;
    initialized_ = true;
    return true;
}

void Application::shutdown() {
    if (initialized_) {
        saveConfig(true);
    }
    if (platform_) {
        platform_->setSystemMinimizeAnimationsEnabled(true);
    }
    if (taskbarHiddenByUs_ && platform_) {
        platform_->setTaskbarVisible(true);
        taskbarHiddenByUs_ = false;
    }
    closeMenu();
    closeGrid();
    closeStack(true);
    closeSettings();
    if (dockMaterial_) {
        SDL_DestroyTexture(dockMaterial_);
        dockMaterial_ = nullptr;
    }
    for (SDL_Texture **texture : {&folderEmptyTexture_, &folderNonemptyTexture_}) {
        if (*texture) {
            SDL_DestroyTexture(*texture);
            *texture = nullptr;
        }
    }
    for (SDL_Texture **texture :
         {&trashEmptyLight_, &trashEmptyDark_, &trashFullLight_, &trashFullDark_,
          &openInFinderBlack_, &openInFinderWhite_, &pileArrowBlack_, &pileArrowWhite_,
          &finderIcon_}) {
        if (*texture) {
            SDL_DestroyTexture(*texture);
            *texture = nullptr;
        }
    }
    if (launchpadDark_) {
        SDL_DestroyTexture(launchpadDark_);
        launchpadDark_ = nullptr;
    }
    if (launchpadLight_) {
        SDL_DestroyTexture(launchpadLight_);
        launchpadLight_ = nullptr;
    }
    if (tooltipBackdrop_) {
        SDL_DestroyTexture(tooltipBackdrop_);
        tooltipBackdrop_ = nullptr;
    }
    for (MenuState *panel : {&menu_, &submenu_}) {
        if (panel->backdrop) {
            SDL_DestroyTexture(panel->backdrop);
            panel->backdrop = nullptr;
        }
        clearMenuIcons(*panel);
    }
    menu_.text.reset();
    menu_.draw.reset();
    submenu_.text.reset();
    submenu_.draw.reset();
    if (grid_.wallpaper) {
        SDL_DestroyTexture(grid_.wallpaper);
        grid_.wallpaper = nullptr;
    }
    for (SDL_Texture **texture :
         {&grid_.searchBackground, &grid_.searchLoupe, &grid_.searchClose}) {
        if (*texture) {
            SDL_DestroyTexture(*texture);
            *texture = nullptr;
        }
    }
    grid_.text.reset();
    grid_.icons.reset();
    grid_.draw.reset();
    stack_.text.reset();
    stack_.icons.reset();
    stack_.draw.reset();
    for (auto &[width, texture] : stackPills_) {
        (void)width;
        if (texture) {
            SDL_DestroyTexture(texture);
        }
    }
    stackPills_.clear();
    if (stackGridMaterial_) {
        SDL_DestroyTexture(stackGridMaterial_);
        stackGridMaterial_ = nullptr;
    }
    for (auto &[handle, texture] : windowThumbnails_) {
        (void)handle;
        if (texture) {
            SDL_DestroyTexture(texture);
        }
    }
    windowThumbnails_.clear();
    if (minimize_.texture) {
        SDL_DestroyTexture(minimize_.texture);
        minimize_.texture = nullptr;
    }
    minimize_.draw.reset();
    if (minimize_.window) {
        if (minimize_.renderer) {
            SDL_DestroyRenderer(minimize_.renderer);
        }
        SDL_DestroyWindow(minimize_.window);
        minimize_.window = nullptr;
        minimize_.renderer = nullptr;
    }
    settingsUi_.text.reset();
    settingsUi_.draw.reset();
    for (SDL_Texture **texture : {&stack_.folderEmpty, &stack_.folderNonempty}) {
        if (*texture) {
            SDL_DestroyTexture(*texture);
            *texture = nullptr;
        }
    }
    if (stack_.window) {
        if (stack_.renderer) {
            SDL_DestroyRenderer(stack_.renderer);
        }
        SDL_DestroyWindow(stack_.window);
        stack_.window = nullptr;
        stack_.renderer = nullptr;
    }
    if (settingsUi_.window) {
        if (settingsUi_.renderer) {
            SDL_DestroyRenderer(settingsUi_.renderer);
        }
        SDL_DestroyWindow(settingsUi_.window);
        settingsUi_.window = nullptr;
        settingsUi_.renderer = nullptr;
    }
    if (menu_.window) {
        if (menu_.renderer) {
            SDL_DestroyRenderer(menu_.renderer);
        }
        SDL_DestroyWindow(menu_.window);
        menu_.window = nullptr;
        menu_.renderer = nullptr;
    }
    if (submenu_.window) {
        if (submenu_.renderer) {
            SDL_DestroyRenderer(submenu_.renderer);
        }
        SDL_DestroyWindow(submenu_.window);
        submenu_.window = nullptr;
        submenu_.renderer = nullptr;
    }
    if (grid_.window) {
        if (grid_.renderer) {
            SDL_DestroyRenderer(grid_.renderer);
        }
        SDL_DestroyWindow(grid_.window);
        grid_.window = nullptr;
        grid_.renderer = nullptr;
    }
    if (backdrop_) {
        SDL_DestroyTexture(backdrop_);
        backdrop_ = nullptr;
    }
    icons_.reset();
    text_.reset();
    renderer_.reset();
    destroyDockWindow();
    apps_.reset();
    platform_.reset();
#ifdef _WIN32
    if (instanceHandle_) {
        ReleaseMutex(static_cast<HANDLE>(instanceHandle_));
        CloseHandle(static_cast<HANDLE>(instanceHandle_));
        instanceHandle_ = nullptr;
    }
#else
    if (instanceFd_ >= 0) {
        flock(instanceFd_, LOCK_UN);
        close(instanceFd_);
        instanceFd_ = -1;
    }
#endif
    if (initialized_) {
        TTF_Quit();
    }
    logShutdown();
    SDL_Quit();
}

void Application::updateDisplay() {
    displayId_ = SDL_GetPrimaryDisplay();
    // The Windows taskbar is hidden while RevDock runs, so use the full display
    // bounds (including the taskbar strip) for both the Dock and Launchpad.
    if (!SDL_GetDisplayBounds(displayId_, &displayBounds_) ||
        displayBounds_.w <= 0 || displayBounds_.h <= 0) {
        displayBounds_ = SDL_Rect{0, 0, 1920, 1080};
    }
    const float scale = SDL_GetDisplayContentScale(displayId_);
    if (scale > 0.0f) {
        contentScale_ = scale;
    }
}

void Application::buildDockWindow() {
    const SDL_WindowFlags flags = static_cast<SDL_WindowFlags>(
        SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP | SDL_WINDOW_TRANSPARENT |
        SDL_WINDOW_UTILITY | SDL_WINDOW_NOT_FOCUSABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);

    const float maxIcon = std::max(settings_.tileSize(), settings_.effectiveLargeSize());
    windowWidth_ = clampf(480.0f, 280.0f, static_cast<float>(displayBounds_.w));
    windowHeight_ = maxIcon + 72.0f;

    dockWindow_ = SDL_CreateWindow("RevDock", static_cast<int>(windowWidth_),
                                   static_cast<int>(windowHeight_), flags);
    if (!dockWindow_) {
        RD_LOG_WARN("Transparent dock window failed, retrying without transparency: %s", SDL_GetError());
        dockWindow_ = SDL_CreateWindow("RevDock", static_cast<int>(windowWidth_),
                                       static_cast<int>(windowHeight_),
                                       static_cast<SDL_WindowFlags>(SDL_WINDOW_BORDERLESS |
                                           SDL_WINDOW_ALWAYS_ON_TOP | SDL_WINDOW_UTILITY |
                                           SDL_WINDOW_NOT_FOCUSABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY));
    }
    if (!dockWindow_) {
        RD_LOG_ERROR("SDL_CreateWindow failed: %s", SDL_GetError());
        return;
    }

    dockRenderer_ = SDL_CreateRenderer(dockWindow_, nullptr);
    if (!dockRenderer_) {
        RD_LOG_ERROR("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(dockWindow_);
        dockWindow_ = nullptr;
        return;
    }
    SDL_SetRenderVSync(dockRenderer_, 1);
    SDL_SetRenderDrawBlendMode(dockRenderer_, SDL_BLENDMODE_BLEND);
    contentScale_ = SDL_GetWindowDisplayScale(dockWindow_);
    if (contentScale_ <= 0.0f) {
        contentScale_ = 1.0f;
    }
    SDL_SetRenderScale(dockRenderer_, contentScale_, contentScale_);
    platform_->prepareDockWindow(dockWindow_);
    // Do not mark the Dock window WS_EX_TRANSPARENT: on Windows that makes
    // clicks pass to other windows *of the same thread*, so with Launchpad
    // open the grid swallowed every Dock click. The custom WM_NCHITTEST in
    // prepareDockWindow already makes the empty area of the Dock transparent.
}

void Application::destroyDockWindow() {
    if (dockRenderer_) {
        SDL_DestroyRenderer(dockRenderer_);
        dockRenderer_ = nullptr;
    }
    if (dockWindow_) {
        SDL_DestroyWindow(dockWindow_);
        dockWindow_ = nullptr;
    }
}

void Application::relayoutWindow() {
    if (!dockWindow_) {
        return;
    }
    const float maxIcon = std::max(settings_.tileSize(), settings_.effectiveLargeSize());
    const float baseLength = layout_.measureBaseLength(visibleItems_, settings_);
    const float extra = maxIcon * 2.0f + 16.0f;

    if (settings_.orientation() == "bottom") {
        windowWidth_ = clampf(baseLength + extra, 260.0f, static_cast<float>(displayBounds_.w));
        windowHeight_ = maxIcon + 76.0f;
        SDL_SetWindowSize(dockWindow_, static_cast<int>(windowWidth_), static_cast<int>(windowHeight_));
        const int x = displayBounds_.x + (displayBounds_.w - static_cast<int>(windowWidth_)) / 2;
        const int y = displayBounds_.y + displayBounds_.h - static_cast<int>(windowHeight_);
        SDL_SetWindowPosition(dockWindow_, x, y);
    } else {
        windowHeight_ = clampf(baseLength + extra, 260.0f, static_cast<float>(displayBounds_.h));
        windowWidth_ = maxIcon + 76.0f;
        SDL_SetWindowSize(dockWindow_, static_cast<int>(windowWidth_), static_cast<int>(windowHeight_));
        const int x = settings_.orientation() == "left"
                          ? displayBounds_.x
                          : displayBounds_.x + displayBounds_.w - static_cast<int>(windowWidth_);
        const int y = displayBounds_.y + (displayBounds_.h - static_cast<int>(windowHeight_)) / 2;
        SDL_SetWindowPosition(dockWindow_, x, y);
    }
    SDL_SyncWindow(dockWindow_);
    backdropDirty_ = true;
    backdropCaptureAt_ = now_ + 0.15;
    // Reserve exactly the Dock bar's height (plus its bottom inset), so a
    // maximized window stops flush against the top edge of the Dock.
    const float dockPadding = std::max(5.0f, settings_.tileSize() * 0.12f);
    const float dockBarHeight = settings_.tileSize() + dockPadding * 2.0f + 6.0f;
    const int thickness =
        static_cast<int>(std::lround(dockBarHeight + kDockBottomInset));
    // The Dock only reserves screen space when it is not auto-hidden.
    platform_->reserveWorkArea(0, settings_.orientation(), thickness,
                               !settings_.autohide());
    forceRender_ = true;
    dirty_ = true;
}

void Application::updateDragHover(float x, float y) {
    const int index = hitTest(x, y);
    std::string appId;
    std::string folderId;
    if (index >= 0 && index < static_cast<int>(visibleItems_.size())) {
        const DockItem &target = visibleItems_[index];
        if (target.isApplication()) {
            appId = target.id;
        } else if (target.type == ItemType::Folder || target.type == ItemType::Trash) {
            folderId = target.id;
        }
    }
    if (appId != dragHoverId_) {
        dragHoverId_ = std::move(appId);
        dragHoverAt_ = clock_.now();
        dragHoverActivated_ = false;
        dirty_ = true;
    }
    if (folderId != dropFolderId_) {
        dropFolderId_ = std::move(folderId);
        dirty_ = true;
    }
}

void Application::handleFileDrop(const std::string &path, float x, float y) {
    if (path.empty()) {
        return;
    }
    const int dropIndex = hitTest(x, y);
    dragHoverId_.clear();
    dropFolderId_.clear();
    dragHoverActivated_ = false;
    const bool onTile = dropIndex >= 0 && dropIndex < static_cast<int>(visibleItems_.size());
    if (onTile && visibleItems_[dropIndex].type == ItemType::Folder) {
        const std::string target = visibleItems_[dropIndex].path;
        if (platform_->movePathInto(path, target)) {
            if (stack_.open && stack_.path == target) {
                refreshStackEntries();
            }
            dirty_ = true;
        }
        return;
    }
    if (onTile && visibleItems_[dropIndex].type == ItemType::Trash) {
        if (platform_->recyclePath(path)) {
            trashEmpty_ = false;
            dirty_ = true;
        }
        return;
    }
    const std::string extension =
        lowerAscii(std::filesystem::path(path).extension().string());
    const bool shortcut = extension == ".lnk" || extension == ".url";
    if (!shortcut && onTile && visibleItems_[dropIndex].isApplication()) {
        if (const AppInfo *app = appForItem(visibleItems_[dropIndex])) {
            platform_->openPathWithApp(*app, path);
            noteRecentApp(app->id);
            if (apps_->isRunning(*app)) {
                flashId_ = app->id;
                flashAt_ = now_;
            }
            return;
        }
    }
    const std::filesystem::path droppedPath = utf8ToPath(path);
    // Shortcuts dropped on the Dock (including UWP/IDList-only .lnk files) are
    // applications, not generic document tiles. Pin the real app so it can be
    // launched and participates in Dock categories.
    const AppInfo *droppedApp = apps_->findByPath(path);
    if (!droppedApp) {
        const std::string target = platform_->shortcutTarget(path);
        if (!target.empty()) {
            droppedApp = apps_->findByPath(target);
        }
    }
    if (!droppedApp) {
        const std::string aumid = platform_->shortcutAumid(path);
        if (!aumid.empty()) {
            droppedApp = apps_->findByAumid(aumid);
        }
    }
    if (droppedApp && modelContainsId(droppedApp->id)) {
        return;
    }
    DockItem item;
    if (droppedApp) {
        item.type = ItemType::Application;
        item.id = droppedApp->id;
        item.path = droppedApp->sourcePath.empty() ? path : droppedApp->sourcePath;
        item.label = droppedApp->name;
        item.iconPath = droppedApp->iconPath;
        item.pinned = true;
    } else {
        item.type = std::filesystem::is_directory(droppedPath) ? ItemType::Folder : ItemType::File;
        item.id = std::string(item.type == ItemType::Folder ? "folder:" : "file:") + path;
        item.path = path;
        item.label = pathToUtf8(droppedPath.stem());
    }
    int insertAt = static_cast<int>(model_.items().size());
    for (int i = 0; i < static_cast<int>(model_.items().size()); ++i) {
        if (model_.items()[i].type == ItemType::Trash) {
            insertAt = i;
            break;
        }
    }
    // The padding left of the first tile is a valid drop target too: insert at
    // the far left instead of appending before the Trash.
    if (dropIndex < 0 && !layoutResult_.tiles.empty()) {
        const SDL_FRect &first = layoutResult_.tiles.front().cell;
        if (x < first.x + first.w * 0.5f) {
            insertAt = 0;
        }
    }
    if (dropIndex >= 0 && dropIndex < static_cast<int>(visibleItems_.size())) {
        const std::string &targetId = visibleItems_[dropIndex].id;
        bool after = false;
        if (dropIndex < static_cast<int>(layoutResult_.tiles.size())) {
            const auto &cell = layoutResult_.tiles[dropIndex].cell;
            after = x > cell.x + cell.w * 0.5f;
        }
        for (int i = 0; i < static_cast<int>(model_.items().size()); ++i) {
            if (model_.items()[i].id == targetId) {
                insertAt = i + (after ? 1 : 0);
                break;
            }
        }
    }
    insertAt = std::clamp(insertAt, 0, static_cast<int>(model_.items().size()));
    model_.addItem(std::move(item), insertAt);
    modelDirty_ = true;
    markConfigDirty(now_);
    rebuildVisibleItems();
    relayoutWindow();
}

void Application::handleEvent(const SDL_Event &event) {
    if (event.type == SDL_EVENT_QUIT) {
        running_ = false;
        return;
    }
    if (event.type == SDL_EVENT_DISPLAY_ADDED || event.type == SDL_EVENT_DISPLAY_REMOVED ||
        event.type == SDL_EVENT_DISPLAY_MOVED ||
        event.type == SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED ||
        event.type == SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED) {
        if (grid_.wallpaper) {
            SDL_DestroyTexture(grid_.wallpaper);
            grid_.wallpaper = nullptr;
        }
        updateDisplay();
        relayoutWindow();
        return;
    }
    if (event.type == SDL_EVENT_DROP_POSITION) {
        if (dockWindow_ && event.drop.windowID == SDL_GetWindowID(dockWindow_)) {
            updateDragHover(event.drop.x, event.drop.y);
        }
        return;
    }
    if (event.type == SDL_EVENT_DROP_COMPLETE) {
        if (!dragHoverId_.empty() || !dropFolderId_.empty()) {
            dragHoverId_.clear();
            dropFolderId_.clear();
            dragHoverActivated_ = false;
            dirty_ = true;
        }
        return;
    }
    if (event.type == SDL_EVENT_DROP_FILE) {
        const std::string path = event.drop.data ? event.drop.data : "";
        if (!path.empty() && dockWindow_ &&
            event.drop.windowID == SDL_GetWindowID(dockWindow_)) {
            handleFileDrop(path, event.drop.x, event.drop.y);
        }
        return;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && grid_.open && grid_.window &&
        eventWindowID(event) == SDL_GetWindowID(grid_.window)) {
        // Search, folder renaming, clipboard paste and keyboard navigation all
        // live in the grid handler; do not swallow key events here.
        handleGridEvent(event);
        return;
    }
    if (event.type == SDL_EVENT_KEY_DOWN) {
        if (event.key.key == SDLK_ESCAPE) {
            if (submenu_.open) {
                closeSubmenu();
            } else if (menu_.open) {
                closeMenu();
            } else if (settingsUi_.open) {
                closeSettings();
            } else if (stack_.open) {
                closeStack();
            } else if (grid_.open) {
                closeGrid();
            }
        }
        return;
    }

    const Uint32 windowId = eventWindowID(event);
    if (windowId == 0) {
        return;
    }
    if (menu_.open && menu_.window && windowId == SDL_GetWindowID(menu_.window)) {
        handleMenuEvent(event);
        return;
    }
    if (submenu_.open && submenu_.window && windowId == SDL_GetWindowID(submenu_.window)) {
        handleMenuEvent(event);
        return;
    }
    if (grid_.open && grid_.window && windowId == SDL_GetWindowID(grid_.window)) {
        handleGridEvent(event);
        // Activating the grid (click / focus) puts it above other topmost
        // windows; put the Dock back immediately so it does not flash.
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
            event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
            event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
            raiseDockLayers();
        }
        return;
    }
    if (stack_.open && stack_.window && windowId == SDL_GetWindowID(stack_.window)) {
        handleStackEvent(event);
        return;
    }
    if (settingsUi_.open && settingsUi_.window &&
        windowId == SDL_GetWindowID(settingsUi_.window)) {
        handleSettingsEvent(event);
        return;
    }
    if (dockWindow_ && windowId == SDL_GetWindowID(dockWindow_)) {
        handleDockEvent(event);
    }
}

const std::vector<DockItem> &Application::layoutItems() const {
    return dockDragPreview_ ? dragPreviewItems_ : visibleItems_;
}

float Application::dockHideOffset() const {
    // A few extra pixels guarantee the bar is fully outside the screen even
    // with fractional window heights / DPI rounding.
    return autohide_.value() * (layoutResult_.barThickness + kDockBottomInset + 6.0f);
}

bool Application::modelContainsId(const std::string &id) const {
    for (const auto &item : model_.items()) {
        if (item.id == id) {
            return true;
        }
    }
    return false;
}

int Application::dragTargetModelIndex() const {
    if (!dockDragPreview_ || dragInsertIndex_ < 0) {
        return -1;
    }
    int target = 0;
    for (int i = 0; i < dragInsertIndex_ && i < static_cast<int>(dragPreviewItems_.size()); ++i) {
        const DockItem &entry = dragPreviewItems_[i];
        if (isDragSlot(entry)) {
            continue;
        }
        if (modelContainsId(entry.id)) {
            ++target;
        }
    }
    return target;
}

void Application::clearDragPreview() {
    dragPreviewItems_.clear();
    dragInsertIndex_ = -1;
    dockDragPreview_ = false;
    dropFolderId_.clear();
}

void Application::updateDockDragPreview() {
    clearDragPreview();
    if (!dragging_ || !dragMoved_ || dragged_ < 0 ||
        dragged_ >= static_cast<int>(visibleItems_.size())) {
        return;
    }
    // Files/folders pinned on the Dock are draggable onto an app (open with
    // it), another folder, or the Trash. They never reorder the Dock and never
    // open an insertion gap.
    const DockItem &draggedItem = visibleItems_[dragged_];
    const bool fileLike =
        draggedItem.type == ItemType::File || draggedItem.type == ItemType::Folder;
    if (fileLike) {
        std::string appId;
        std::string folderId;
        const int hoverIndex = hitTest(dragX_, dragY_);
        if (hoverIndex >= 0 && hoverIndex != dragged_ &&
            hoverIndex < static_cast<int>(visibleItems_.size())) {
            const DockItem &target = visibleItems_[hoverIndex];
            if (target.isApplication()) {
                appId = target.id;
            } else if (target.type == ItemType::Folder || target.type == ItemType::Trash) {
                folderId = target.id;
            }
        }
        if (appId != dragHoverId_) {
            dragHoverId_ = std::move(appId);
            dragHoverAt_ = clock_.now();
            dragHoverActivated_ = false;
        }
        if (folderId != dropFolderId_) {
            dropFolderId_ = std::move(folderId);
        }
        dirty_ = true;
        return;
    }
    std::vector<DockItem> base;
    base.reserve(visibleItems_.size());
    for (int i = 0; i < static_cast<int>(visibleItems_.size()); ++i) {
        if (i != dragged_) {
            base.push_back(visibleItems_[i]);
        }
    }
    const LayoutResult reference = layout_.compute(base, settings_, windowWidth_, windowHeight_,
                                                  mouseX_, mouseY_, true, magEnvelope_.value());
    const bool horizontal = settings_.orientation() == "bottom";
    const float axis = horizontal ? dragX_ : dragY_;
    int insert = 0;
    for (size_t i = 0; i < reference.tiles.size(); ++i) {
        const SDL_FRect &cell = reference.tiles[i].cell;
        const float center = horizontal ? cell.x + cell.w * 0.5f : cell.y + cell.h * 0.5f;
        if (axis > center) {
            insert = static_cast<int>(i) + 1;
        }
    }
    // The application grid always stays leftmost.
    if (!base.empty() && base.front().type == ItemType::AppGrid && insert < 1) {
        insert = 1;
    }
    DockItem slot;
    slot.type = ItemType::File;
    slot.id = kDragSlotId;
    slot.pinned = false;
    base.insert(base.begin() + insert, std::move(slot));
    dragPreviewItems_ = std::move(base);
    dragInsertIndex_ = insert;
    dockDragPreview_ = true;
}

void Application::updateGridDragPreview(float localX, float localY) {
    const bool wasOver = gridDragOverDock_;
    const bool valid = grid_.pressed && grid_.dragMoved &&
                       !grid_.dragIsFolder && !grid_.dragAppId.empty();
    SDL_FRect bar = layoutResult_.bar;
    bar.y += dockHideOffset();
    SDL_FRect hover = bar;
    hover.y -= 40.0f;
    hover.h += 40.0f;
    if (!valid || !contains(hover, localX, localY)) {
        if (wasOver) {
            clearDragPreview();
            gridDragOverDock_ = false;
            dirty_ = true;
        }
        return;
    }
    dragX_ = localX;
    dragY_ = localY;
    std::vector<DockItem> base = visibleItems_;
    const LayoutResult reference = layout_.compute(base, settings_, windowWidth_, windowHeight_,
                                                   localX, localY, true, magEnvelope_.value());
    const bool horizontal = settings_.orientation() == "bottom";
    const float axis = horizontal ? localX : localY;
    int insert = 0;
    for (size_t i = 0; i < reference.tiles.size(); ++i) {
        const SDL_FRect &cell = reference.tiles[i].cell;
        const float center = horizontal ? cell.x + cell.w * 0.5f : cell.y + cell.h * 0.5f;
        if (axis > center) {
            insert = static_cast<int>(i) + 1;
        }
    }
    if (!base.empty() && base.front().type == ItemType::AppGrid && insert < 1) {
        insert = 1;
    }
    DockItem slot;
    slot.type = ItemType::File;
    slot.id = kDragSlotId;
    slot.pinned = false;
    base.insert(base.begin() + insert, std::move(slot));
    dragPreviewItems_ = std::move(base);
    dragInsertIndex_ = insert;
    dockDragPreview_ = true;
    gridDragOverDock_ = true;
    dirty_ = true;
    forceRender_ = true;
}

void Application::finishDockDrag() {
    if (!dragging_ || dragged_ < 0 ||
        dragged_ >= static_cast<int>(visibleItems_.size())) {
        clearDragPreview();
        dragging_ = false;
        dragged_ = -1;
        dragMoved_ = false;
        return;
    }
    float globalX = 0.0f;
    float globalY = 0.0f;
    platform_->globalCursor(globalX, globalY);
    int windowX = 0;
    int windowY = 0;
    SDL_GetWindowPosition(dockWindow_, &windowX, &windowY);
    const float localX = globalX - static_cast<float>(windowX);
    const float localY = globalY - static_cast<float>(windowY);
    SDL_FRect bar = layoutResult_.bar;
    bar.y += dockHideOffset();
    const bool insideDock = contains(bar, localX, localY) || hitTest(localX, localY) >= 0;

    const DockItem item = visibleItems_[dragged_];
    if (!dragHoverId_.empty() &&
        (item.type == ItemType::File || item.type == ItemType::Folder)) {
        for (const auto &target : visibleItems_) {
            if (target.id != dragHoverId_) {
                continue;
            }
            if (const AppInfo *app = appForItem(target)) {
                platform_->openPathWithApp(*app, item.path);
                noteRecentApp(app->id);
                if (apps_->isRunning(*app)) {
                    flashId_ = app->id;
                    flashAt_ = now_;
                }
            }
            break;
        }
        dragHoverId_.clear();
        dragHoverActivated_ = false;
        dropFolderId_.clear();
        clearDragPreview();
        dragging_ = false;
        dragged_ = -1;
        dragMoved_ = false;
        dirty_ = true;
        return;
    }
    int modelIndex = -1;
    for (size_t i = 0; i < model_.items().size(); ++i) {
        if (model_.items()[i].id == item.id) {
            modelIndex = static_cast<int>(i);
            break;
        }
    }
    const bool movable = modelIndex >= 0 && item.pinned && item.type != ItemType::Separator &&
                         item.type != ItemType::Trash;
    const bool removable = movable && item.type != ItemType::AppGrid &&
                           item.type != ItemType::Finder;
    if (!dropFolderId_.empty() && modelIndex >= 0 &&
        (item.type == ItemType::File || item.type == ItemType::Folder)) {
        bool handled = false;
        for (const auto &target : visibleItems_) {
            if (target.id != dropFolderId_) {
                continue;
            }
            if (target.type == ItemType::Trash) {
                handled = platform_->recyclePath(item.path);
                if (handled) {
                    trashEmpty_ = false;
                }
            } else if (target.type == ItemType::Folder) {
                handled = platform_->movePathInto(item.path, target.path);
                if (handled && stack_.open && stack_.path == target.path) {
                    refreshStackEntries();
                }
            }
            break;
        }
        if (handled) {
            model_.removeAt(static_cast<size_t>(modelIndex));
            bounce_.erase(item.id);
            modelDirty_ = true;
            markConfigDirty(now_);
            rebuildVisibleItems();
            relayoutWindow();
        }
        dropFolderId_.clear();
        clearDragPreview();
        dragging_ = false;
        dragged_ = -1;
        dragMoved_ = false;
        dirty_ = true;
        return;
    }
    if (!insideDock && removable) {
        model_.removeAt(static_cast<size_t>(modelIndex));
        bounce_.erase(item.id);
        modelDirty_ = true;
        markConfigDirty(now_);
        rebuildVisibleItems();
        relayoutWindow();
    } else if (insideDock && movable && dockDragPreview_) {
        int target = dragTargetModelIndex();
        if (target >= 0) {
            DockItem moved = model_.items()[modelIndex];
            model_.removeAt(static_cast<size_t>(modelIndex));
            int last = static_cast<int>(model_.items().size());
            for (int i = 0; i < static_cast<int>(model_.items().size()); ++i) {
                if (model_.items()[i].type == ItemType::Trash) {
                    last = i;
                    break;
                }
            }
            target = std::clamp(target, 0, last);
            model_.addItem(std::move(moved), target);
            modelDirty_ = true;
            markConfigDirty(now_);
            rebuildVisibleItems();
            relayoutWindow();
        }
    } else if (insideDock && dockDragPreview_ && item.type == ItemType::Application &&
               !item.pinned && modelIndex < 0 && !dragPreviewItems_.empty()) {
        // Unpinned running apps live in the recent region. Reorder that region
        // by moving the app inside the recent-app history list.
        std::vector<std::string> order;
        int insert = 0;
        bool sawSlot = false;
        for (const auto &entry : dragPreviewItems_) {
            if (isDragSlot(entry)) {
                insert = static_cast<int>(order.size());
                sawSlot = true;
                continue;
            }
            if (entry.type == ItemType::Application && !entry.pinned &&
                !modelContainsId(entry.id)) {
                order.push_back(entry.id);
            }
        }
        if (!sawSlot) {
            insert = static_cast<int>(order.size());
        }
        order.erase(std::remove(order.begin(), order.end(), item.id), order.end());
        insert = std::clamp(insert, 0, static_cast<int>(order.size()));
        order.insert(order.begin() + insert, item.id);
        for (const auto &id : recentApps_) {
            if (std::find(order.begin(), order.end(), id) == order.end()) {
                order.push_back(id);
            }
        }
        if (order.size() > 12) {
            order.resize(12);
        }
        if (order != recentApps_) {
            recentApps_ = order;
            settings_.setRecentApps(recentApps_);
            markConfigDirty(now_);
            rebuildVisibleItems();
            relayoutWindow();
        }
    }
    dragHoverId_.clear();
    dragHoverActivated_ = false;
    clearDragPreview();
    dragging_ = false;
    dragged_ = -1;
    dragMoved_ = false;
    dirty_ = true;
}

int Application::hitTest(float x, float y) const {
    const float offset = dockHideOffset();
    for (size_t i = 0; i < layoutResult_.tiles.size(); ++i) {
        SDL_FRect rect = layoutResult_.tiles[i].cell;
        rect.y += offset;
        if (contains(rect, x, y)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void Application::handleDockEvent(const SDL_Event &event) {
    if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && dockWindow_ &&
        event.window.windowID == SDL_GetWindowID(dockWindow_)) {
        // Allow a normal close (WM_CLOSE / Alt+F4) to run the full shutdown,
        // which restores the taskbar, work area and DWM transitions.
        running_ = false;
        return;
    }
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION: {
        if (menu_.open) {
            // Keep the magnification where it was when the menu opened.
            break;
        }
        mouseX_ = event.motion.x;
        mouseY_ = event.motion.y;
        mouseInside_ = true;
        layoutResult_ = layout_.compute(layoutItems(), settings_, windowWidth_, windowHeight_,
                                        mouseX_, mouseY_, mouseInside_, magEnvelope_.value());
        hovered_ = hitTest(mouseX_, mouseY_);
        if (dragging_) {
            dragX_ = mouseX_;
            dragY_ = mouseY_;
            if (std::abs(mouseX_ - dragStartX_) > 5.0f || std::abs(mouseY_ - dragStartY_) > 5.0f) {
                dragMoved_ = true;
                updateDockDragPreview();
            }
        }
        autohideDeadline_ = now_ + settings_.autohideDelay();
        dirty_ = true;
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        lastDockButtonAt_ = now_;
        if (menu_.open) {
            closeMenu();
        }
        float gx = 0.0f;
        float gy = 0.0f;
        platform_->globalCursor(gx, gy);
        mouseX_ = event.button.x;
        mouseY_ = event.button.y;
        layoutResult_ = layout_.compute(layoutItems(), settings_, windowWidth_, windowHeight_,
                                        mouseX_, mouseY_, true, magEnvelope_.value());
        const int index = hitTest(mouseX_, mouseY_);
        RD_LOG_DEBUG("dock button %d idx=%d at %.0f,%.0f", event.button.button, index, mouseX_,
                     mouseY_);
        if (event.button.button == SDL_BUTTON_RIGHT) {
            if (index >= 0) {
                openMenuForItem(static_cast<size_t>(index), gx, gy);
            } else {
                openMenuForBackground(gx, gy);
            }
            break;
        }
        if (event.button.button != SDL_BUTTON_LEFT) {
            break;
        }
        dragged_ = index;
        const ItemType draggedType =
            index >= 0 ? visibleItems_[index].type : ItemType::Application;
        // Launchpad and Finder can be repositioned; the separator and Trash
        // are fixed, and a minimized-window tile is not reorderable.
        dragging_ = index >= 0 && draggedType != ItemType::Separator &&
                    draggedType != ItemType::Trash &&
                    draggedType != ItemType::MinimizedWindow;
        dragMoved_ = false;
        dragStartX_ = event.button.x;
        dragStartY_ = event.button.y;
        dragX_ = event.button.x;
        dragY_ = event.button.y;
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (event.button.button != SDL_BUTTON_LEFT) {
            break;
        }
        if (dragging_ && dragMoved_) {
            finishDockDrag();
            break;
        }
        if (!dragMoved_ && dragged_ >= 0 &&
            dragged_ < static_cast<int>(visibleItems_.size())) {
            const DockItem item = visibleItems_[dragged_];
            if (item.type == ItemType::AppGrid) {
                // Clicking the Launchpad icon while Launchpad is open closes it.
                if (grid_.open) {
                    closeGrid();
                } else {
                    openGrid();
                }
            } else if (item.type == ItemType::Trash) {
                platform_->openTrash();
            } else if (item.type == ItemType::MinimizedWindow) {
                restoreMinimizedItem(item);
            } else if (item.type == ItemType::Folder) {
                if (item.stack) {
                    float gx = 0.0f;
                    float gy = 0.0f;
                    platform_->globalCursor(gx, gy);
                    openStack(item, gx, gy);
                } else {
                    platform_->launchItem(item);
                }
            } else if (item.type == ItemType::Application) {
                if (const AppInfo *app = appForItem(item)) {
                    activateOrLaunch(*app);
                } else {
                    platform_->launchItem(item);
                }
            } else if (item.type != ItemType::Separator) {
                platform_->launchItem(item);
            }
        }
        clearDragPreview();
        dragging_ = false;
        dragged_ = -1;
        dragMoved_ = false;
        dirty_ = true;
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (grid_.open) {
            handleGridWheel(event.wheel.x, event.wheel.y, event.wheel.direction);
        }
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        mouseInside_ = false;
        hovered_ = -1;
        dirty_ = true;
        break;
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
        mouseInside_ = true;
        dirty_ = true;
        break;
    default:
        break;
    }
}

void Application::handleMenuEvent(const SDL_Event &event) {
    if (menu_.open && menu_.window &&
        event.window.windowID == SDL_GetWindowID(menu_.window)) {
        handleMenuPanelEvent(menu_, event, false);
        return;
    }
    if (submenu_.open && submenu_.window &&
        event.window.windowID == SDL_GetWindowID(submenu_.window)) {
        handleMenuPanelEvent(submenu_, event, true);
    }
}

void Application::handleMenuPanelEvent(MenuState &state, const SDL_Event &event,
                                       bool submenu) {
    auto activateRow = [&](int row) {
        if (row < 0 || row >= static_cast<int>(state.items.size())) {
            return;
        }
        MenuItem item = state.items[row];
        if (item.separator || item.header || !item.enabled) {
            return;
        }
        if (!item.submenu.empty()) {
            if (!submenu) {
                state.hover = row;
                openSubmenu(item, row);
            }
            return;
        }
        closeMenu();
        executeMenu(item);
    };

    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        const int row = menuRowAt(state, event.motion.y);
        const bool valid =
            row >= 0 && row < static_cast<int>(state.items.size()) &&
            !state.items[row].separator && !state.items[row].header;
        state.hover = valid ? row : -1;
        if (!submenu) {
            if (valid && !state.items[row].submenu.empty()) {
                if (row != menu_.submenuParentRow &&
                    row != menu_.submenuPendingRow) {
                    menu_.submenuPendingRow = row;
                    menu_.submenuPendingAt = now_ + 0.12;
                }
            } else if (submenu_.open || menu_.submenuParentRow >= 0) {
                menu_.submenuPendingRow = -1;
                if (valid) {
                    closeSubmenu();
                }
            }
        }
        dirty_ = true;
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if (event.button.button != SDL_BUTTON_LEFT) {
            closeMenu();
            return;
        }
        const int row = menuRowAt(state, event.button.y);
        if (row >= 0 && row < static_cast<int>(state.items.size()) &&
            !state.items[row].separator && !state.items[row].header) {
            activateRow(row);
        } else {
            closeMenu();
        }
        return;
    }
    if (event.type != SDL_EVENT_KEY_DOWN) {
        return;
    }
    if (event.key.key == SDLK_ESCAPE) {
        if (submenu) {
            closeSubmenu();
        } else {
            closeMenu();
        }
        return;
    }
    if (event.key.key == SDLK_LEFT && submenu) {
        closeSubmenu();
        return;
    }
    if (event.key.key == SDLK_RIGHT && !submenu) {
        if (state.hover >= 0 && state.hover < static_cast<int>(state.items.size())) {
            const MenuItem &item = state.items[state.hover];
            if (!item.submenu.empty()) {
                openSubmenu(item, state.hover);
            }
        }
        return;
    }
    if (event.key.key == SDLK_DOWN || event.key.key == SDLK_UP) {
        if (state.items.empty()) {
            return;
        }
        int row = state.hover;
        for (int step = 0; step < static_cast<int>(state.items.size()); ++step) {
            row += (event.key.key == SDLK_DOWN ? 1 : -1);
            if (row < 0) {
                row = static_cast<int>(state.items.size()) - 1;
            }
            if (row >= static_cast<int>(state.items.size())) {
                row = 0;
            }
            const MenuItem &item = state.items[row];
            if (!item.separator && !item.header && item.enabled) {
                break;
            }
        }
        state.hover = row;
        if (!submenu) {
            menu_.submenuPendingRow = -1;
            if (menu_.submenuParentRow >= 0 && row != menu_.submenuParentRow) {
                closeSubmenu();
            }
        }
        dirty_ = true;
        return;
    }
    if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
        activateRow(state.hover);
    }
}

void Application::openSubmenu(const MenuItem &item, int row) {
    if (item.submenu.empty()) {
        return;
    }
    closeSubmenu();
    if (!submenu_.window) {
        createMenuWindow(submenu_, "RevDock Submenu");
    }
    if (!submenu_.window || !submenu_.text) {
        return;
    }
    std::vector<MenuItem> items = item.submenu;
    float provisionalWidth = 150.0f;
    bool hasChecks = false;
    bool hasIcons = false;
    for (const auto &entry : items) {
        hasChecks = hasChecks || entry.checked;
        hasIcons = hasIcons || !entry.iconPath.empty();
        if (entry.separator || entry.header) {
            continue;
        }
        float width = submenu_.text->measure(entry.label, kMenuFontSize).x + 32.0f;
        if (!entry.detail.empty()) {
            width += submenu_.text->measure(entry.detail, 11.0f).x + 18.0f;
        }
        if (!entry.submenu.empty()) {
            width += 16.0f;
        }
        provisionalWidth = std::max(provisionalWidth, width);
    }
    provisionalWidth += (hasChecks ? 12.0f : 0.0f) + (hasIcons ? 20.0f : 0.0f);
    provisionalWidth = clampf(provisionalWidth, 150.0f, 520.0f);

    float x = menu_.x + menu_.width - 3.0f;
    if (x + provisionalWidth >
        static_cast<float>(displayBounds_.x + displayBounds_.w) - 8.0f) {
        x = menu_.x - provisionalWidth + 3.0f;
    }
    float y = menu_.y + menuRowTop(menu_, row) - kMenuPadding;
    x = clampf(x, static_cast<float>(displayBounds_.x + 8),
               static_cast<float>(displayBounds_.x + displayBounds_.w) - provisionalWidth - 8.0f);
    y = clampf(y, static_cast<float>(displayBounds_.y + 8),
               static_cast<float>(displayBounds_.y + displayBounds_.h) - 40.0f);
    menu_.submenuParentRow = row;
    menu_.submenuPendingRow = -1;
    openMenuPanel(submenu_, std::move(items), 0.0f, 0.0f, false, x, y);
}

void Application::handleGridEvent(const SDL_Event &event) {
    if (grid_.closing) {
        return;
    }
    computeGridMetrics();
    launchpadSearchResults();
    auto clearSearch = [&]() {
        grid_.query.clear();
        grid_.composing.clear();
        grid_.searchKey.clear();
        grid_.searchResults.clear();
        grid_.searchFocused = false;
        grid_.pendingText.clear();
        grid_.pendingTextAt = -1.0;
        if (grid_.folderId.empty()) {
            grid_.page = grid_.rootPage;
        }
        dirty_ = true;
    };
    auto backspaceUtf8 = [](std::string &text) {
        while (!text.empty() &&
               (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) {
            text.pop_back();
        }
        if (!text.empty()) {
            text.pop_back();
        }
    };

    // Multi-touch page swipe: with two or more fingers down, treat the average
    // horizontal movement like a drag on the wallpaper. Single-finger input is
    // left to the normal mouse emulation (tap/drag icons).
    if (event.type == SDL_EVENT_FINGER_DOWN) {
        grid_.fingers.insert(event.tfinger.fingerID);
        if (grid_.fingers.size() >= 2) {
            grid_.blankPressed = false;
            grid_.swiping = false;
            grid_.wheelSwiping = false;
            grid_.wheelSwipeOffset = 0.0f;
            if (!grid_.fingerSwiping) {
                grid_.fingerSwiping = true;
                grid_.swipeAnimating = false;
                grid_.fingerSwipeOffset = grid_.swipeOffset;
                grid_.hover = -1;
            }
        }
        return;
    }
    if (event.type == SDL_EVENT_FINGER_MOTION) {
        if (!grid_.open || grid_.fingers.size() < 2) {
            return;
        }
        const float width = static_cast<float>(displayBounds_.w);
        const int pages = gridPageCount();
        const float delta = event.tfinger.dx * width /
                            static_cast<float>(std::max<size_t>(1, grid_.fingers.size()));
        const float edgeCap = width * 0.10f;
        if (delta > 0.0f && grid_.page <= 0) {
            grid_.fingerSwipeOffset =
                std::min(grid_.fingerSwipeOffset + delta, edgeCap);
        } else if (delta < 0.0f && grid_.page >= pages - 1) {
            grid_.fingerSwipeOffset =
                std::max(grid_.fingerSwipeOffset + delta, -edgeCap);
        } else {
            grid_.fingerSwipeOffset =
                clampf(grid_.fingerSwipeOffset + delta, -width, width);
        }
        // Commit the page as soon as the threshold is crossed, like the
        // precision-touchpad path. This lets two quick swipes flip two pages
        // without waiting for the first page animation or the dots.
        const float threshold = width * settings_.launchpadSwipeThreshold();
        if (grid_.fingerSwipeOffset <= -threshold && grid_.page + 1 < pages) {
            grid_.page++;
            startGridSwipeAnimation(grid_.swipeOffset + width, 0.0f, 0.18);
            grid_.fingerSwipeOffset = 0.0f;
        } else if (grid_.fingerSwipeOffset >= threshold && grid_.page > 0) {
            grid_.page--;
            startGridSwipeAnimation(grid_.swipeOffset - width, 0.0f, 0.18);
            grid_.fingerSwipeOffset = 0.0f;
        } else {
            startGridSwipeAnimation(grid_.swipeOffset, grid_.fingerSwipeOffset, 0.05);
        }
        grid_.fingerSwipeAt = now_;
        dirty_ = true;
        return;
    }
    if (event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_FINGER_CANCELED) {
        grid_.fingers.erase(event.tfinger.fingerID);
        if (grid_.fingers.empty() && grid_.fingerSwiping) {
            grid_.fingerSwiping = false;
            const float offset = grid_.fingerSwipeOffset;
            grid_.fingerSwipeOffset = 0.0f;
            settleGridSwipe(offset, settings_.launchpadSwipeThreshold());
        }
        return;
    }
    if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
        // Activating the grid raises it above other topmost windows; put the
        // Dock (and an open menu) back on top (macOS keeps the Dock over
        // Launchpad).  Re-enable text input after focus changes.
        raiseDockLayers();
        if (grid_.window) {
            SDL_StartTextInput(grid_.window);
        }
        return;
    }
    if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        closeGrid();
        return;
    }
    if (event.type == SDL_EVENT_TEXT_EDITING) {
        grid_.pendingText.clear();
        grid_.pendingTextAt = -1.0;
        if (grid_.renaming || grid_.folderId.empty()) {
            grid_.composing = event.edit.text ? event.edit.text : "";
            dirty_ = true;
        }
        return;
    }
    if (event.type == SDL_EVENT_TEXT_INPUT) {
        const std::string text = event.text.text ? event.text.text : "";
        if (!text.empty() && now_ < grid_.nativeCommitUntil &&
            grid_.lastNativeText.find(text) != std::string::npos) {
            // The native IME result already inserted this text (possibly split
            // into one TEXT_INPUT per character by SDL).
            return;
        }
        grid_.pendingText.clear();
        grid_.pendingTextAt = -1.0;
        if (text.empty()) {
            return;
        }
        if (grid_.renaming) {
            if (grid_.renameFresh) {
                grid_.renameText.clear();
                grid_.renameFresh = false;
            }
            grid_.renameText += text;
            grid_.composing.clear();
            grid_.searchCaretAt = now_;
            dirty_ = true;
            return;
        }
        if (!grid_.folderId.empty()) {
            return;
        }
        if (!grid_.query.empty()) {
            grid_.rootPage = grid_.page;
        }
        grid_.query += text;
        grid_.composing.clear();
        grid_.searchKey.clear();
        grid_.searchFocused = true;
        grid_.searchCaretAt = now_;
        grid_.page = 0;
        grid_.blankPressed = false;
        grid_.swiping = false;
        dirty_ = true;
        return;
    }
    if (event.type == SDL_EVENT_KEY_DOWN) {
        const bool ctrl =
            (event.key.mod & SDL_KMOD_CTRL) != 0 ||
            (event.key.mod & SDL_KMOD_GUI) != 0;
        if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_RETURN ||
            event.key.key == SDLK_KP_ENTER ||
            event.key.key == SDLK_BACKSPACE || event.key.key == SDLK_DELETE ||
            event.key.key == SDLK_LEFT || event.key.key == SDLK_RIGHT ||
            event.key.key == SDLK_HOME || event.key.key == SDLK_END) {
            grid_.pendingText.clear();
            grid_.pendingTextAt = -1.0;
        }
        if (event.key.key == SDLK_ESCAPE) {
            if (grid_.renaming) {
                launchpadCommitRename(true);
            } else if (!grid_.query.empty()) {
                clearSearch();
            } else if (!grid_.folderId.empty()) {
                launchpadCloseFolder();
            } else {
                closeGrid();
            }
            return;
        }
        if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
            if (grid_.renaming) {
                launchpadCommitRename(false);
                return;
            }
            if (!grid_.query.empty()) {
                const auto &results = launchpadSearchResults();
                if (!results.empty()) {
                    const AppInfo *app = apps_->findById(results.front());
                    if (app) {
                        const AppInfo copy = *app;
                        closeGrid();
                        activateOrLaunch(copy);
                    }
                }
                return;
            }
            return;
        }
        if (event.key.key == SDLK_BACKSPACE) {
            if (grid_.renaming) {
                if (grid_.renameFresh) {
                    grid_.renameText.clear();
                    grid_.renameFresh = false;
                } else {
                    backspaceUtf8(grid_.renameText);
                }
                grid_.searchCaretAt = now_;
                dirty_ = true;
                return;
            }
            if (!grid_.query.empty()) {
                backspaceUtf8(grid_.query);
                grid_.searchKey.clear();
                grid_.searchFocused = true;
                grid_.searchCaretAt = now_;
                grid_.page = grid_.query.empty() ? grid_.rootPage : 0;
                dirty_ = true;
            }
            return;
        }
        if (event.key.key == SDLK_DELETE) {
            if (grid_.renaming) {
                grid_.renameText.clear();
                grid_.renameFresh = false;
                dirty_ = true;
            } else if (!grid_.query.empty()) {
                clearSearch();
            }
            return;
        }
        if (ctrl && event.key.key == SDLK_V) {
            char *clipboard = SDL_GetClipboardText();
            if (clipboard) {
                std::string text = clipboard;
                SDL_free(clipboard);
                for (char &ch : text) {
                    if (ch == '\r' || ch == '\n' || ch == '\t') {
                        ch = ' ';
                    }
                }
                if (grid_.renaming) {
                    if (grid_.renameFresh) {
                        grid_.renameText.clear();
                        grid_.renameFresh = false;
                    }
                    grid_.renameText += text;
                    grid_.composing.clear();
                    grid_.searchCaretAt = now_;
                    dirty_ = true;
                } else if (grid_.folderId.empty() && !text.empty()) {
                    if (grid_.query.empty()) {
                        grid_.rootPage = grid_.page;
                    }
                    grid_.query += text;
                    grid_.composing.clear();
                    grid_.searchKey.clear();
                    grid_.searchFocused = true;
                    grid_.searchCaretAt = now_;
                    grid_.page = 0;
                    grid_.blankPressed = false;
                    grid_.swiping = false;
                    dirty_ = true;
                }
            }
            return;
        }
        if (ctrl && (event.key.key == SDLK_C || event.key.key == SDLK_X)) {
            if (grid_.renaming) {
                SDL_SetClipboardText(grid_.renameText.c_str());
                if (event.key.key == SDLK_X) {
                    grid_.renameText.clear();
                    grid_.renameFresh = false;
                }
                grid_.searchCaretAt = now_;
                dirty_ = true;
            } else if (!grid_.query.empty()) {
                SDL_SetClipboardText(grid_.query.c_str());
                if (event.key.key == SDLK_X) {
                    clearSearch();
                }
            }
            return;
        }
        if (ctrl && event.key.key == SDLK_F) {
            grid_.searchFocused = true;
            grid_.searchCaretAt = now_;
            dirty_ = true;
            return;
        }
        if (!ctrl && (event.key.mod & SDL_KMOD_ALT) == 0 &&
            (event.key.mod & SDL_KMOD_GUI) == 0 &&
            (grid_.renaming || grid_.folderId.empty())) {
            const std::string text =
                printableTextFor(event.key.key, event.key.mod);
            if (!text.empty()) {
                grid_.pendingText += text;
                if (grid_.pendingTextAt < 0.0) {
                    grid_.pendingTextAt = now_;
                }
                dirty_ = true;
                return;
            }
        }
        if (event.key.key == SDLK_LEFT || event.key.key == SDLK_PAGEUP) {
            if (grid_.folderId.empty() && !grid_.query.empty()) {
                switchGridPage(-1);
            } else {
                switchGridPage(-1);
            }
        } else if (event.key.key == SDLK_RIGHT || event.key.key == SDLK_PAGEDOWN) {
            switchGridPage(1);
        } else if (event.key.key == SDLK_HOME) {
            switchGridPageTo(0);
        } else if (event.key.key == SDLK_END) {
            switchGridPageTo(gridPageCount() - 1);
        }
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_WHEEL) {
        handleGridWheel(event.wheel.x, event.wheel.y, event.wheel.direction);
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION && grid_.pagerPressed) {
        const int dot = gridDotAt(event.motion.x, event.motion.y);
        if (dot >= 0) {
            switchGridPageTo(dot);
        }
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION && grid_.blankPressed) {
        float raw = event.motion.x - grid_.swipeStartX;
        const int pages = gridPageCount();
        const float width = static_cast<float>(displayBounds_.w);
        if ((raw < 0.0f && grid_.page >= pages - 1) || (raw > 0.0f && grid_.page <= 0)) {
            // At the first/last page keep the overscroll to a small rubber band.
            const float edgeCap = static_cast<float>(displayBounds_.w) * 0.10f;
            raw = std::clamp(raw * 0.12f, -edgeCap, edgeCap);
        }
        if (!grid_.swiping && std::abs(raw) > 10.0f) {
            grid_.swiping = true;
            grid_.hover = -1;
        }
        if (grid_.swiping) {
            grid_.swipeOffset = clampf(raw, -width, width);
            dirty_ = true;
        }
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        const float x = event.motion.x;
        const float y = event.motion.y;
        if (grid_.pressed) {
            grid_.dragX = x;
            grid_.dragY = y;
            if (std::abs(x - grid_.dragStartX) > 5.0f ||
                std::abs(y - grid_.dragStartY) > 5.0f) {
                if (!grid_.dragMoved) {
                    SDL_CaptureMouse(true);
                }
                grid_.dragMoved = true;
            }
            if (grid_.dragMoved) {
                launchpadUpdateDragTarget(x, y);
            }
        }
        grid_.hover = -1;
        dirty_ = true;
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event.button.button == SDL_BUTTON_LEFT) {
        // A Dock launch can leave the Win32 foreground on the previous app
        // (the Dock is WS_EX_NOACTIVATE).  Make Launchpad foreground before
        // handling the click so IME composition results are delivered.
        platform_->focusWindow(grid_.window);
        if (grid_.renaming) {
            launchpadCommitRename(false);
        }
        if (!grid_.query.empty() && grid_.searchClose &&
            contains(grid_.searchCloseRect, event.button.x, event.button.y)) {
            clearSearch();
            return;
        }
        if (grid_.folderId.empty() && contains(grid_.searchRect, event.button.x,
                                                event.button.y)) {
            grid_.searchFocused = true;
            grid_.searchCaretAt = now_;
            SDL_StartTextInput(grid_.window);
            dirty_ = true;
            return;
        }
        if (!grid_.folderId.empty() &&
            contains(grid_.folderTitleRect, event.button.x, event.button.y)) {
            // Single-clicking the title above the folder panel edits it.
            launchpadBeginRename(false);
            return;
        }
        if (grid_.query.empty()) {
            const int dot = gridDotAt(event.button.x, event.button.y);
            if (dot >= 0) {
                grid_.pagerPressed = true;
                grid_.blankPressed = false;
                grid_.swiping = false;
                switchGridPageTo(dot);
                return;
            }
        }
        const int col = static_cast<int>(
            std::floor((event.button.x - grid_.gridRect.x) /
                       std::max(1.0f, grid_.cellWidth)));
        const int row = static_cast<int>(
            std::floor((event.button.y - grid_.gridRect.y) /
                       std::max(1.0f, grid_.cellHeight)));
        const int slot = row * grid_.columns + col;
        LaunchpadCell cell;
        bool onIcon = false;
        if (col >= 0 && col < grid_.columns && row >= 0 && row < grid_.rows) {
            cell = launchpadCellAt(grid_.page, slot);
            if (!cell.appId.empty() || cell.folder) {
                const float cellCenterX =
                    grid_.gridRect.x + (static_cast<float>(col) + 0.5f) *
                                           grid_.cellWidth;
                const float cellCenterY =
                    grid_.gridRect.y + (static_cast<float>(row) + 0.5f) *
                                           grid_.cellHeight;
                const SDL_FRect icon{
                    cellCenterX - grid_.iconSize * 0.5f,
                    cellCenterY - grid_.iconSize * 0.5f - 8.0f,
                    grid_.iconSize, grid_.iconSize};
                onIcon = contains(icon, event.button.x, event.button.y);
            }
        }
        if (onIcon) {
            grid_.dragPage = grid_.page;
            grid_.dragSlot = slot;
            grid_.dragAppId = cell.appId;
            grid_.dragIsFolder = cell.folder;
            grid_.pressed = true;
            grid_.dragMoved = false;
            grid_.dragStartX = event.button.x;
            grid_.dragStartY = event.button.y;
            grid_.dragX = event.button.x;
            grid_.dragY = event.button.y;
            grid_.blankPressed = false;
            grid_.swiping = false;
            launchpadResetDrag();
            grid_.dragPage = grid_.page;
            grid_.dragSlot = slot;
            grid_.dragAppId = cell.appId;
            grid_.dragIsFolder = cell.folder;
            grid_.dragOriginFolderId = grid_.folderId;
            grid_.returnFrom.clear();
            grid_.folderDrop.active = false;
            // Pick-up leaves the original slot open, as in macOS Launchpad.
            grid_.dragTarget = GridState::DragTarget::Gap;
            grid_.dragTargetPage = grid_.page;
            grid_.dragTargetSlot = slot;
            grid_.dragTargetAt = now_;
        } else if (!grid_.swipeAnimating || grid_.wheelSwiping) {
            if (!grid_.folderId.empty() &&
                !contains(grid_.folderPanel, event.button.x,
                          event.button.y)) {
                // Clicking outside an expanded folder returns to its page.
                launchpadCloseFolder();
                return;
            }
            // Blank area: a click dismisses Launchpad (or does nothing inside
            // an expanded folder), a horizontal drag swipes.
            grid_.blankPressed = true;
            grid_.swiping = false;
            grid_.wheelSwiping = false;
            grid_.wheelSwipeOffset = 0.0f;
            grid_.swipeStartX = event.button.x - grid_.swipeOffset;
        }
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
        event.button.button == SDL_BUTTON_LEFT) {
        if (grid_.pagerPressed) {
            grid_.pagerPressed = false;
            return;
        }
        if (grid_.blankPressed) {
            finishGridSwipe();
            return;
        }
        // Release is finalized by the global-cursor poll in update(), so a drag
        // that leaves the grid window still lands on the Dock.
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event.button.button == SDL_BUTTON_RIGHT) {
        const int col = static_cast<int>(
            std::floor((event.button.x - grid_.gridRect.x) /
                       std::max(1.0f, grid_.cellWidth)));
        const int row = static_cast<int>(
            std::floor((event.button.y - grid_.gridRect.y) /
                       std::max(1.0f, grid_.cellHeight)));
        if (col >= 0 && col < grid_.columns && row >= 0 && row < grid_.rows) {
            const LaunchpadCell cell =
                launchpadCellAt(grid_.page, row * grid_.columns + col);
            if (!cell.appId.empty()) {
                if (const AppInfo *app = apps_->findById(cell.appId)) {
                    toggleDockItem(*app);
                    grid_.hint = "Dock updated";
                    dirty_ = true;
                }
            }
        }
    }
}

void Application::update(double now) {
    autohide_.update(now);
    if (autohide_.active() || !bounce_.empty() || reorderAnimating_) {
        // Bounce/slide animations need a frame every tick, otherwise they only
        // advance when something else happens to invalidate the window.
        dirty_ = true;
    }
    if (stackAnimating()) {
        dirty_ = true;
    }
    updateCursor();
    if (grid_.open && grid_.pendingTextAt > 0.0 &&
        now - grid_.pendingTextAt >= 0.03) {
        // No TEXT_INPUT/TEXT_EDITING arrived for this key: insert the fallback
        // printable character ourselves (the path that works on all IMEs).
        const std::string text = grid_.pendingText;
        grid_.pendingText.clear();
        grid_.pendingTextAt = -1.0;
        if (grid_.renaming) {
            if (grid_.renameFresh) {
                grid_.renameText.clear();
                grid_.renameFresh = false;
            }
            grid_.renameText += text;
            grid_.composing.clear();
            grid_.searchCaretAt = now;
            dirty_ = true;
        } else if (grid_.folderId.empty() && !text.empty()) {
            if (grid_.query.empty()) {
                grid_.rootPage = grid_.page;
            }
            grid_.query += text;
            grid_.composing.clear();
            grid_.searchKey.clear();
            grid_.searchFocused = true;
            grid_.searchCaretAt = now;
            grid_.page = 0;
            grid_.blankPressed = false;
            grid_.swiping = false;
            dirty_ = true;
        }
    }
    // A folder stack is a logical extension of an auto-hidden Dock: keep the
    // Dock visible while the pointer is over either one, and fold the stack
    // (so the Dock can hide) once the pointer has left both for a moment.
    if (settings_.autohide() && stack_.open && !stack_.closing) {
        float gx = 0.0f;
        float gy = 0.0f;
        platform_->globalCursor(gx, gy);
        const bool overStack =
            gx >= stack_.x - 6.0f && gy >= stack_.y - 6.0f &&
            gx <= stack_.x + stack_.width + 6.0f &&
            gy <= stack_.y + stack_.height + 6.0f;
        if (mouseInside_ || overStack || stack_.pressed || stack_.dragging) {
            stackAwayAt_ = 0.0;
        } else if (stackAwayAt_ <= 0.0) {
            stackAwayAt_ = now;
        } else if (now - stackAwayAt_ >= 0.20) {
            stackAwayAt_ = 0.0;
            closeStack();
        }
    } else {
        stackAwayAt_ = 0.0;
    }
    // Fallback for a Windows/SDL routing quirk: while Launchpad owns the
    // foreground, a right-click over the Dock is sometimes delivered to the
    // grid only. Poll the global button state and open the Dock menu ourselves
    // if no Dock button event arrived.
    {
        float gx = 0.0f;
        float gy = 0.0f;
        const SDL_MouseButtonFlags flags = SDL_GetGlobalMouseState(&gx, &gy);
        const bool rightDown = (flags & SDL_BUTTON_RMASK) != 0;
        if (rightDown && !pollRightDown_ && dockWindow_ &&
            now_ - lastDockButtonAt_ > 0.15) {
            int wx = 0;
            int wy = 0;
            SDL_GetWindowPosition(dockWindow_, &wx, &wy);
            const float lx = gx - static_cast<float>(wx);
            const float ly = gy - static_cast<float>(wy);
            SDL_FRect bar = layoutResult_.bar;
            bar.y += dockHideOffset();
            if (contains(bar, lx, ly) || hitTest(lx, ly) >= 0) {
                mouseX_ = lx;
                mouseY_ = ly;
                layoutResult_ =
                    layout_.compute(layoutItems(), settings_, windowWidth_, windowHeight_, lx,
                                    ly, true, magEnvelope_.value());
                const int index = hitTest(lx, ly);
                if (menu_.open) {
                    closeMenu();
                }
                float gcx = 0.0f;
                float gcy = 0.0f;
                platform_->globalCursor(gcx, gcy);
                if (index >= 0) {
                    openMenuForItem(static_cast<size_t>(index), gcx, gcy);
                } else {
                    openMenuForBackground(gcx, gcy);
                }
                lastDockButtonAt_ = now_;
            }
        }
        pollRightDown_ = rightDown;
    }
    // macOS eases magnification in/out instead of snapping when the pointer
    // enters or leaves the Dock. While a context menu is open the pointer
    // leaves the Dock window (to the menu), but the icon must stay magnified.
    if (!menu_.open) {
        magEnvelope_.setTarget(now, mouseInside_ && settings_.magnification() ? 1.0f : 0.0f,
                               0.16);
    }
    magEnvelope_.update(now);
    if (magEnvelope_.active()) {
        dirty_ = true;
    }
    updateAutohide(now);
    updateRunning(now);
    if (restoringHandle_ != 0 && restoringClearAt_ > 0.0 && now >= restoringClearAt_) {
        // Safety net for a handle the platform never stops reporting.
        restoringHandle_ = 0;
        restoringClearAt_ = 0.0;
        rebuildVisibleItems();
        relayoutWindow();
    }
    if (now - lastMinimizedPoll_ > 0.5) {
        lastMinimizedPoll_ = now;
        std::set<std::uintptr_t> signature;
        for (const auto &window : platform_->minimizedWindows()) {
            signature.insert(window.handle);
        }
        if (signature != minimizedSignature_) {
            minimizedSignature_ = std::move(signature);
            rebuildVisibleItems();
            relayoutWindow();
            dirty_ = true;
        }
    }
    if (fileDrag_) {
        float gx = 0.0f;
        float gy = 0.0f;
        const SDL_MouseButtonFlags flags = SDL_GetGlobalMouseState(&gx, &gy);
        const bool down = (flags & SDL_BUTTON_LMASK) != 0;
        int wx = 0;
        int wy = 0;
        SDL_GetWindowPosition(dockWindow_, &wx, &wy);
        const float lx = gx - static_cast<float>(wx);
        const float ly = gy - static_cast<float>(wy);
        SDL_FRect bar = layoutResult_.bar;
        bar.y += dockHideOffset();
        const bool overDock = contains(bar, lx, ly) || hitTest(lx, ly) >= 0;
        // Keep the internal drag alive in a band just above the Dock. A fan item
        // is grabbed well above the bar, so handing off to OLE the moment the
        // pointer leaves the bar would make dropping back onto an app/folder
        // impossible (and the Dock could not dim the target).
        SDL_FRect nearBar = bar;
        nearBar.y -= 160.0f;
        nearBar.h += 160.0f;
        const bool nearDock = overDock || contains(nearBar, lx, ly);
        if (overDock) {
            updateDragHover(lx, ly);
            if (!down) {
                const std::string path = fileDragPath_;
                fileDrag_ = false;
                fileDragPath_.clear();
                handleFileDrop(path, lx, ly);
            }
        } else {
            dragHoverId_.clear();
            dropFolderId_.clear();
            dragHoverActivated_ = false;
            const bool stackCollapsing = stack_.open && stack_.closing;
            if (down) {
                if (!nearDock && !stackCollapsing) {
                    const std::string path = fileDragPath_;
                    fileDrag_ = false;
                    fileDragPath_.clear();
                    platform_->startFileDrag({path});
                }
            } else {
                fileDrag_ = false;
                fileDragPath_.clear();
            }
        }
        dirty_ = true;
    }
    if (!dragHoverId_.empty() && !dragHoverActivated_ && now - dragHoverAt_ >= 0.5) {
        for (const auto &entry : visibleItems_) {
            if (entry.id != dragHoverId_) {
                continue;
            }
            if (const AppInfo *app = appForItem(entry); app && apps_->isRunning(*app)) {
                platform_->activateApplication(*app);
                flashId_ = app->id;
                flashAt_ = now;
                dragHoverActivated_ = true;
                dirty_ = true;
            }
            break;
        }
    }
    if (!flashId_.empty() && now - flashAt_ > 0.5) {
        flashId_.clear();
        dirty_ = true;
    }
    if (now - lastTrashPoll_ > 2.0) {
        lastTrashPoll_ = now;
        const bool empty = platform_->isTrashEmpty();
        if (empty != trashEmpty_) {
            trashEmpty_ = empty;
            dirty_ = true;
        }
    }
    if (stack_.open && stack_.closing && now - stack_.closedAt >= 0.16) {
        finishClosingStack();
    }
    updateStackAnchor();
    Platform::MinimizeEvent minimizeEvent;
    if (platform_->pollMinimizeEvent(minimizeEvent)) {
        startMinimizeAnimation(minimizeEvent.execPath, minimizeEvent.aumid,
                               minimizeEvent.handle, minimizeEvent.title, minimizeEvent.rect,
                               minimizeEvent.rgba, minimizeEvent.width, minimizeEvent.height);
    }
    const double topmostInterval = grid_.open ? 0.25 : 2.0;
    if (now - lastTopmostCheck_ > topmostInterval) {
        if (!settingsUi_.open) {
            raiseDockLayers();
        }
        if (taskbarHiddenByUs_) {
            // Explorer occasionally recreates or re-shows the taskbar.
            platform_->setTaskbarVisible(false);
        }
        lastTopmostCheck_ = now;
    }
    if (now >= nextBackdropCapture_) {
        backdropDirty_ = true;
        nextBackdropCapture_ = now + 0.3;
    }
    if (backdropDirty_ && now >= backdropCaptureAt_ && dockWindow_ && !menu_.open &&
        !grid_.open && !stack_.open && !settingsUi_.open && !minimize_.active &&
        autohide_.value() < 0.9f) {
        refreshBackdrop();
    }

    if (menu_.open) {
        if (menu_.submenuPendingRow >= 0 && now_ >= menu_.submenuPendingAt) {
            const int row = menu_.submenuPendingRow;
            menu_.submenuPendingRow = -1;
            if (row >= 0 && row < static_cast<int>(menu_.items.size()) &&
                row == menu_.hover && !menu_.items[row].submenu.empty()) {
                openSubmenu(menu_.items[row], row);
            }
        }
        float gx = 0.0f;
        float gy = 0.0f;
        const SDL_MouseButtonFlags flags = SDL_GetGlobalMouseState(&gx, &gy);
        const bool down = (flags & SDL_BUTTON_LMASK) != 0;
        if (down && !cursorWasDown_) {
            int wx = 0, wy = 0;
            SDL_GetWindowPosition(menu_.window, &wx, &wy);
            bool inside = !(gx < wx || gy < wy || gx > wx + menu_.width ||
                            gy > wy + menu_.height);
            if (!inside && submenu_.open && submenu_.window) {
                int sx = 0, sy = 0;
                SDL_GetWindowPosition(submenu_.window, &sx, &sy);
                inside = !(gx < sx || gy < sy || gx > sx + submenu_.width ||
                           gy > sy + submenu_.height);
            }
            if (!inside) {
                closeMenu();
            }
        }
        cursorWasDown_ = down;
    } else {
        cursorWasDown_ = false;
    }

    if (stack_.open && !stack_.closing) {
        float gx = 0.0f;
        float gy = 0.0f;
        const SDL_MouseButtonFlags flags = SDL_GetGlobalMouseState(&gx, &gy);
        const bool down = (flags & SDL_BUTTON_LMASK) != 0;
        if (down && !stackWasDown_) {
            if (gx < stack_.x || gy < stack_.y || gx > stack_.x + stack_.width ||
                gy > stack_.y + stack_.height) {
                stack_.dismissedAt = now;
                stack_.dismissedPath = stack_.path;
                closeStack();
            }
        }
        stackWasDown_ = down;
    } else {
        stackWasDown_ = false;
    }

    // Warm the Launchpad icon cache in the background, one icon at a time, so
    // the first open is instant instead of stalling on ~90 icon conversions.
    const bool idleForPreload = !mouseInside_ && !dragging_ && !menu_.open && !stack_.open &&
                                !settingsUi_.open && !grid_.open && bounce_.empty();
    if (!preloadDone_ && idleForPreload && grid_.icons && now >= nextPreloadAt_) {
        const auto &list = apps_->apps();
        if (list.empty()) {
            nextPreloadAt_ = now + 0.5;
        } else if (list.size() != preloadCount_) {
            preloadCount_ = list.size();
            preloadIndex_ = 0;
            nextPreloadAt_ = now + 0.3;
        } else if (preloadIndex_ >= static_cast<int>(list.size())) {
            preloadDone_ = true;
        } else {
            const AppInfo &app = list[preloadIndex_++];
            const int size = static_cast<int>(gridIconSize());
            if (grid_.icons->textureForApp(app, size)) {
                grid_.attemptedIcons.insert(app.id);
            }
            // Gentle cadence: one icon per 100 ms while the Dock is idle.
            nextPreloadAt_ = now + 0.10;
            if (preloadIndex_ >= static_cast<int>(list.size())) {
                preloadDone_ = true;
                RD_LOG_DEBUG("preloaded %zu launchpad icons", list.size());
            }
        }
    }

    if (gridOpenAt_ > 0.0 && now >= gridOpenAt_ && !grid_.open) {
        gridOpenAt_ = 0.0;
        openGrid();
    }

    if (grid_.open && grid_.pagerPressed) {
        float gx = 0.0f;
        float gy = 0.0f;
        const SDL_MouseButtonFlags flags = SDL_GetGlobalMouseState(&gx, &gy);
        if ((flags & SDL_BUTTON_LMASK) == 0) {
            grid_.pagerPressed = false;
            dirty_ = true;
        }
    }

    if (grid_.open && grid_.blankPressed) {
        float gx = 0.0f;
        float gy = 0.0f;
        const SDL_MouseButtonFlags flags = SDL_GetGlobalMouseState(&gx, &gy);
        if ((flags & SDL_BUTTON_LMASK) == 0) {
            finishGridSwipe();
        }
    }

    if (grid_.wheelEdgeSuppressed && now - grid_.wheelLastAt > 0.25) {
        // The stream finally went quiet; allow a fresh outward swipe again.
        grid_.wheelEdgeSuppressed = false;
    }

    if (grid_.open && grid_.wheelSwiping) {
        const double dt = clampd(now - grid_.wheelSmoothAt, 0.0, 0.05);
        grid_.wheelSmoothAt = now;
        const float k = 1.0f - std::exp(-static_cast<float>(dt) * 36.0f);
        const float difference = grid_.wheelSwipeOffset - grid_.swipeOffset;
        if (std::abs(difference) > 0.35f) {
            grid_.swipeOffset += difference * k;
            grid_.swipeAnimating = false;
            dirty_ = true;
        } else {
            grid_.swipeOffset = grid_.wheelSwipeOffset;
        }
        // Past the first/last page there is nothing to flip, so recover almost
        // immediately; a normal swipe keeps a slightly longer accumulation
        // window so slow but steady finger movement still builds up distance.
        const int pageCount = gridPageCount();
        const bool atEdge = (grid_.wheelSwipeOffset > 0.0f && grid_.page <= 0) ||
                            (grid_.wheelSwipeOffset < 0.0f && grid_.page >= pageCount - 1);
        const double settleDelay = atEdge ? 0.10 : 0.06;
        if (now - grid_.wheelSwipeAt > settleDelay) {
            grid_.wheelSwiping = false;
            const float offset = grid_.swipeOffset;
            grid_.wheelSwipeOffset = 0.0f;
            // Only a deliberate swipe flips; smaller movements spring back.
            settleGridSwipe(offset, 0.18f);
        }
    }

    if (grid_.open && grid_.swipeAnimating) {
        const double t = clampd((now - grid_.swipeFromTime) / grid_.swipeDuration, 0.0, 1.0);
        const float tt = static_cast<float>(t);
        // A snap-back (no page left / threshold not reached) uses a fast
        // ease-out so it recovers immediately instead of slowly easing in.
        const float eased = grid_.swipeEaseOut ? 1.0f - std::pow(1.0f - tt, 3.0f)
                                               : easeInOut(tt);
        grid_.swipeOffset = lerpf(grid_.swipeFrom, grid_.swipeTo, eased);
        if (t >= 1.0) {
            grid_.swipeAnimating = false;
            grid_.swipeOffset = grid_.swipeTo;
        }
        dirty_ = true;
    }

    if (grid_.open && grid_.pressed) {
        float gx = 0.0f;
        float gy = 0.0f;
        const SDL_MouseButtonFlags flags = SDL_GetGlobalMouseState(&gx, &gy);
        int gridX = 0;
        int gridY = 0;
        SDL_GetWindowPosition(grid_.window, &gridX, &gridY);
        const float localX = gx - static_cast<float>(gridX);
        const float localY = gy - static_cast<float>(gridY);
        // update() is also the release path for drags that leave the window.
        grid_.dragX = localX;
        grid_.dragY = localY;
        if (std::abs(localX - grid_.dragStartX) > 5.0f ||
            std::abs(localY - grid_.dragStartY) > 5.0f) {
            grid_.dragMoved = true;
        }
        int dockX = 0;
        int dockY = 0;
        if (dockWindow_) {
            SDL_GetWindowPosition(dockWindow_, &dockX, &dockY);
        }
        const float dockLocalX = gx - static_cast<float>(dockX);
        const float dockLocalY = gy - static_cast<float>(dockY);
        if (grid_.dragMoved) {
            computeGridMetrics();
            if (!grid_.folderId.empty() && !grid_.folderClosing) {
                const bool insideFolder =
                    localX >= grid_.folderPanel.x &&
                    localX <= grid_.folderPanel.x + grid_.folderPanel.w &&
                    localY >= grid_.folderPanel.y &&
                    localY <= grid_.folderPanel.y + grid_.folderPanel.h;
                if (insideFolder) {
                    grid_.folderAwayAt = -1.0;
                } else if (grid_.folderAwayAt < 0.0) {
                    grid_.folderAwayAt = now_;
                } else if (now_ - grid_.folderAwayAt >= 0.5) {
                    // Holding a dragged icon outside the folder rectangle for
                    // half a second folds the folder; the drag continues so it
                    // can be dropped on the page.
                    grid_.folderAwayAt = -1.0;
                    launchpadCloseFolder(false, true);
                }
            }
            launchpadUpdateDragTarget(localX, localY);
            updateGridDragPreview(dockLocalX, dockLocalY);
        }
        if ((flags & SDL_BUTTON_LMASK) == 0) {
            const bool moved = grid_.dragMoved;
            SDL_FRect bar = layoutResult_.bar;
            bar.y += dockHideOffset();
            const bool overDock =
                contains(bar, dockLocalX, dockLocalY) ||
                hitTest(dockLocalX, dockLocalY) >= 0;
            if (moved) {
                if (overDock && !grid_.dragIsFolder &&
                    !grid_.dragAppId.empty()) {
                    const AppInfo *app = apps_->findById(grid_.dragAppId);
                    if (app && !model_.containsApplication(app->id)) {
                        DockItem item;
                        item.type = ItemType::Application;
                        item.id = app->id;
                        item.path = app->sourcePath;
                        item.label = app->name;
                        item.iconPath = app->iconPath;
                        int target = dragTargetModelIndex();
                        int last = static_cast<int>(model_.items().size());
                        for (int i = 0; i < static_cast<int>(model_.items().size()); ++i) {
                            if (model_.items()[i].type == ItemType::Trash) {
                                last = i;
                                break;
                            }
                        }
                        if (target < 0) {
                            target = last;
                        }
                        target = std::clamp(target, 0, last);
                        model_.addItem(std::move(item), target);
                        modelDirty_ = true;
                        markConfigDirty(now_);
                        rebuildVisibleItems();
                        relayoutWindow();
                        grid_.hint = "Added to Dock";
                    }
                    launchpadResetDrag();
                    grid_.pressed = false;
                    grid_.dragMoved = false;
                    SDL_CaptureMouse(false);
                } else {
                    launchpadFinishDrag(localX, localY, overDock);
                }
            } else {
                const LaunchpadCell cell = launchpadCellAt(
                    grid_.dragPage, grid_.dragSlot);
                launchpadResetDrag();
                grid_.pressed = false;
                grid_.dragMoved = false;
                SDL_CaptureMouse(false);
                if (cell.folder) {
                    launchpadOpenFolder(cell.folderId);
                } else if (cell.app) {
                    const AppInfo app = *cell.app;
                    closeGrid();
                    activateOrLaunch(app);
                }
            }
            clearDragPreview();
            gridDragOverDock_ = false;
            dirty_ = true;
        }
    }

    if (grid_.open) {
        const float previousOpacity = gridOpacity_.value();
        gridOpacity_.update(now);
        const bool folderWasAnimating = grid_.folderAnim.active();
        grid_.folderAnim.update(now);
        if (grid_.folderDrop.active &&
            now >= grid_.folderDrop.start + grid_.folderDrop.duration) {
            grid_.folderDrop.active = false;
            dirty_ = true;
        }
        computeGridMetrics();
        launchpadSearchResults();
        bool cellAnimating = false;
        for (const auto &[key, anim] : grid_.cellAnims) {
            (void)key;
            if (anim.init && now < anim.start + anim.duration) {
                cellAnimating = true;
                break;
            }
        }
        bool iconsPending = false;
        const int perPage = std::max(1, grid_.columns * grid_.rows);
        for (int slot = 0; slot < perPage; ++slot) {
            const LaunchpadCell cell = launchpadCellAt(grid_.page, slot);
            if (cell.appId.empty() && !cell.folder) {
                break;
            }
            if (!cell.folder && grid_.attemptedIcons.count(cell.appId) == 0) {
                iconsPending = true;
                break;
            }
        }
        if (gridOpacity_.value() != previousOpacity || now - grid_.openedAt < 0.6 ||
            iconsPending || folderWasAnimating || cellAnimating) {
            dirty_ = true;
        }
        if (!grid_.folderId.empty() && grid_.folderClosing &&
            !grid_.folderAnim.active()) {
            grid_.folderId.clear();
            grid_.folderClosing = false;
            grid_.folderAnim.setImmediate(0.0f);
            grid_.page = grid_.rootPage;
            dirty_ = true;
            forceRender_ = true;
        }
        if (grid_.closing && !gridOpacity_.active()) {
            grid_.open = false;
            grid_.closing = false;
            if (grid_.window) {
                if (dockWindow_) {
                    // Clear the owner before hiding: an owned window would be
                    // hidden together with its owner.
                    platform_->setWindowOwner(dockWindow_, nullptr);
                }
                SDL_HideWindow(grid_.window);
            }
        }
    }

    if (dragging_) {
        float gx = 0.0f;
        float gy = 0.0f;
        const SDL_MouseButtonFlags flags = SDL_GetGlobalMouseState(&gx, &gy);
        if ((flags & SDL_BUTTON_LMASK) == 0) {
            if (dragMoved_) {
                finishDockDrag();
            }
        } else if (dragMoved_) {
            int windowX = 0;
            int windowY = 0;
            SDL_GetWindowPosition(dockWindow_, &windowX, &windowY);
            mouseX_ = gx - static_cast<float>(windowX);
            mouseY_ = gy - static_cast<float>(windowY);
            dragX_ = mouseX_;
            dragY_ = mouseY_;
            updateDockDragPreview();
            dirty_ = true;
        }
    }
}

void Application::updateCursor() {
    if (now_ - lastCursorPoll_ < 0.03) {
        return;
    }
    lastCursorPoll_ = now_;
    if (menu_.open) {
        // A context menu freezes the magnification until it closes.
        return;
    }
    float gx = 0.0f;
    float gy = 0.0f;
    if (!platform_->globalCursor(gx, gy) || !dockWindow_) {
        return;
    }
    int wx = 0;
    int wy = 0;
    SDL_GetWindowPosition(dockWindow_, &wx, &wy);
    const float lx = gx - static_cast<float>(wx);
    const float ly = gy - static_cast<float>(wy);
    // "Inside" means over the dock bar (or the magnified icons), not anywhere
    // in the taller transparent window.
    SDL_FRect bar = layoutResult_.bar;
    bar.y += dockHideOffset();
    const float headroom =
        std::max(0.0f, settings_.effectiveLargeSize() - layoutResult_.baseSize) + 6.0f;
    bool inside = lx >= 0.0f && ly >= 0.0f && lx < windowWidth_ && ly < windowHeight_ &&
                  lx >= bar.x - 2.0f && lx < bar.x + bar.w + 2.0f &&
                  ly >= bar.y - headroom && ly < bar.y + bar.h + 2.0f;
    if (inside && stack_.open && gx >= stack_.x && gy >= stack_.y &&
        gx <= stack_.x + stack_.width && gy <= stack_.y + stack_.height) {
        // The pointer is over the open fan, not over a Dock tile: do not
        // magnify the Dock behind it.
        inside = false;
    }
    if (inside != mouseInside_) {
        mouseInside_ = inside;
        dirty_ = true;
    }
    if (inside) {
        if (std::abs(lx - mouseX_) > 0.5f || std::abs(ly - mouseY_) > 0.5f) {
            mouseX_ = lx;
            mouseY_ = ly;
            // Keep magnification alive even when another window (context menu,
            // popover) holds SDL's mouse focus.
            if (!dragging_) {
                hovered_ = hitTest(mouseX_, mouseY_);
            }
            dirty_ = true;
        }
    }
}

void Application::updateAutohide(double now) {
    if (!settings_.autohide()) {
        autohide_.setImmediate(0.0f);
        autohideHidden_ = false;
        autohideDeadline_ = now + 1000.0;
        return;
    }
    if (!dockWindow_) {
        return;
    }
    if (grid_.open || menu_.open || stack_.open) {
        // Launchpad covers the whole screen and a folder stack is a logical
        // extension of the Dock, so the pointer is not "inside" the Dock
        // window in either case. Keep the bar visible; an open stack closes
        // itself when the pointer leaves (see update()), and only then may the
        // Dock hide.
        autohideDeadline_ = now + settings_.autohideDelay();
        autohide_.setTarget(now, 0.0f, 0.18 * settings_.autohideTimeModifier());
        autohideHidden_ = false;
        if (autohide_.active()) {
            dirty_ = true;
        }
        return;
    }
    float gx = 0.0f;
    float gy = 0.0f;
    platform_->globalCursor(gx, gy);
    bool trigger = false;
    // Exactly the last physical row/column is the reveal trigger.
    if (settings_.orientation() == "bottom") {
        trigger = gy >= displayBounds_.y + displayBounds_.h - 1;
    } else if (settings_.orientation() == "left") {
        trigger = gx <= displayBounds_.x;
    } else {
        trigger = gx >= displayBounds_.x + displayBounds_.w - 1;
    }
    bool over = false;
    const bool fullyHidden = autohide_.value() >= 0.98f;
    if (fullyHidden) {
        // Hover the final pixel for 0.1 s; moving even one pixel up resets it.
        if (trigger) {
            if (autohideRevealAt_ <= 0.0) {
                autohideRevealAt_ = now + 0.1;
            }
            over = now >= autohideRevealAt_;
        } else {
            autohideRevealAt_ = 0.0;
        }
    } else {
        over = mouseInside_ || trigger;
        autohideRevealAt_ = 0.0;
    }
    if (over) {
        autohideDeadline_ = now + settings_.autohideDelay();
    }
    const bool hide = !over && now >= autohideDeadline_;
    const float duration = 0.18 * settings_.autohideTimeModifier();
    autohide_.setTarget(now, hide ? 1.0f : 0.0f, duration);
    if (hide != autohideHidden_) {
        RD_LOG_DEBUG("autohide hide=%d trigger=%d", hide ? 1 : 0, trigger ? 1 : 0);
    }
    autohideHidden_ = hide;
    if (autohide_.active()) {
        dirty_ = true;
    }
}

void Application::updateRunning(double now) {
    if (now - lastRunningPoll_ < 1.0) {
        return;
    }
    lastRunningPoll_ = now;
    const bool runningChanged = apps_->refreshRunningIfDue(now);
    if (!launching_.empty()) {
        for (auto it = launching_.begin(); it != launching_.end();) {
            const AppInfo *app = apps_->findById(*it);
            const auto bounce = bounce_.find(*it);
            const double elapsed = bounce != bounce_.end() ? now - bounce->second : 0.0;
            RD_LOG_DEBUG("launch wait %s app=%d running=%d elapsed=%.1f", it->c_str(),
                         app ? 1 : 0, app && apps_->isRunning(*app) ? 1 : 0, elapsed);
            // Launcher stubs and console hosts do not share the shortcut's
            // executable name; bind the real process the first time it shows
            // up so the running dot and the bounce match the app.
            if (app && !apps_->isRunning(*app) && elapsed >= 1.5) {
                if (auto watch = launchWatch_.find(*it);
                    watch != launchWatch_.end()) {
                    bindLaunchAlias(*app, watch->second);
                }
            }
            // Let the current hop finish so the icon never freezes mid-air.
            bool atRest = true;
            (void)atRest;
            // As soon as the process/window is detected, stop bouncing. The
            // one-second running poll can repeatedly sample the middle of a
            // hop, so waiting for an "at rest" phase could bounce forever.
            if ((app && apps_->isRunning(*app)) || !app || elapsed > 20.0) {
                RD_LOG_DEBUG("launch bounce stopped for %s", it->c_str());
                bounce_.erase(*it);
                launchWatch_.erase(*it);
                it = launching_.erase(it);
                dirty_ = true;
                rebuildVisibleItems();
                relayoutWindow();
            } else {
                ++it;
            }
        }
    }
    if (runningChanged) {
        const size_t before = visibleItems_.size();
        rebuildVisibleItems();
        if (visibleItems_.size() != before) {
            relayoutWindow();
        }
        dirty_ = true;
    }
}

void Application::bindLaunchAlias(const AppInfo &app, const LaunchWatch &watch) {
    static const auto stemOf = [](const std::string &path) {
        return lowerAscii(pathToUtf8(utf8ToPath(path).stem()));
    };
    const std::set<std::string> processes = platform_->runningProcessPaths();
    // If the shortcut's own executable is running, the regular process check
    // will report it; do not bind a coincidental window (cmd.exe under
    // Windows Terminal must stay tied to cmd.exe, not the terminal host).
    const std::string execStem = app.execPath.empty() ? std::string()
                                                      : stemOf(app.execPath);
    if (!execStem.empty()) {
        for (const std::string &path : processes) {
            if (stemOf(path) == execStem) {
                return;
            }
        }
    }

    std::vector<std::string> candidates;
    auto addCandidate = [&](const std::string &path) {
        if (path.empty() || path.find('\\') == std::string::npos ||
            path.find("revdock.exe") != std::string::npos ||
            path.find("\\explorer.exe") != std::string::npos ||
            path.find("\\startmenuexperiencehost.exe") != std::string::npos ||
            path.find("\\shellexperiencehost.exe") != std::string::npos ||
            path.find("\\searchhost.exe") != std::string::npos) {
            return;
        }
        if (std::find(candidates.begin(), candidates.end(), path) ==
            candidates.end()) {
            candidates.push_back(path);
        }
    };
    for (const std::string &path : platform_->runningExecutables()) {
        if (watch.windows.count(path) == 0) {
            addCandidate(path);
        }
    }
    if (candidates.empty()) {
        // Nothing new owns a window yet: clicking a launcher stub for an
        // already-running instance just brings it to the foreground.
        const std::string foreground = platform_->foregroundProcessPath();
        if (!foreground.empty() && foreground != watch.foreground) {
            addCandidate(foreground);
        }
    }
    if (candidates.empty()) {
        return;
    }

    std::string chosen;
    if (candidates.size() == 1) {
        chosen = candidates.front();
    } else {
        // Several new windows: prefer the one whose process name shares a
        // significant word with the application name (Word -> WINWORD).
        std::vector<std::string> tokens;
        std::string token;
        for (char ch : lowerAscii(app.name)) {
            if (std::isalnum(static_cast<unsigned char>(ch))) {
                token.push_back(ch);
            } else if (!token.empty()) {
                if (token.size() >= 3) {
                    tokens.push_back(token);
                }
                token.clear();
            }
        }
        if (token.size() >= 3) {
            tokens.push_back(token);
        }
        for (const std::string &candidate : candidates) {
            const std::string stem = stemOf(candidate);
            for (const std::string &word : tokens) {
                if (stem.find(word) != std::string::npos) {
                    chosen = candidate;
                    break;
                }
            }
            if (!chosen.empty()) {
                break;
            }
        }
    }
    if (!chosen.empty() && processes.count(chosen) > 0) {
        apps_->setProcessAlias(app.id, chosen);
    }
}

void Application::refreshBackdrop() {
    if (!dockWindow_ || !dockRenderer_) {
        return;
    }

    layoutResult_ = layout_.compute(layoutItems(), settings_, windowWidth_, windowHeight_,
                                    mouseX_, mouseY_, mouseInside_, magEnvelope_.value());
    // Always sample the fully-visible bar position. Sampling the hidden
    // (off-screen) position produced a black backdrop that flashed while the
    // Dock slid back in.
    SDL_FRect bar = layoutResult_.bar;
    const int pixelX = static_cast<int>(std::lround(bar.x * contentScale_));
    const int pixelY = static_cast<int>(std::lround(bar.y * contentScale_));
    const int pixelW = std::max(1, static_cast<int>(std::lround(bar.w * contentScale_)));
    const int pixelH = std::max(1, static_cast<int>(std::lround(bar.h * contentScale_)));

    // A full-resolution capture keeps the rounded alpha mask and the 1 px
    // border pixel-aligned. The blur is cheap; the screen read-back dominates.
    const int downscale = 1;
    const int targetW = std::max(1, pixelW / downscale);
    const int targetH = std::max(1, pixelH / downscale);

    std::vector<uint8_t> pixels;
    int capturedW = 0;
    int capturedH = 0;
    backdropDirty_ = false;
    const Uint64 captureStart = SDL_GetPerformanceCounter();
    const bool captured = platform_->captureWindowBackdrop(
        dockWindow_, pixelX, pixelY, pixelW, pixelH, pixels, capturedW, capturedH, downscale);
    const Uint64 captureDone = SDL_GetPerformanceCounter();
    if (captured) {
        // Cheap change detection: skip the blur and the texture upload when the
        // captured content is identical to the previous frame.
        uint64_t signature = 1469598103934665603ull;
        for (size_t i = 0; i + 3 < pixels.size(); i += 33) {
            signature ^= pixels[i];
            signature *= 1099511628211ull;
            signature ^= pixels[i + 1];
            signature *= 1099511628211ull;
        }
        float textureW = 0.0f;
        float textureH = 0.0f;
        const bool sameSize =
            backdrop_ && SDL_GetTextureSize(backdrop_, &textureW, &textureH) &&
            static_cast<int>(textureW) == capturedW && static_cast<int>(textureH) == capturedH;
        if (sameSize && signature == backdropSignature_) {
            backdropDirty_ = false;
            // The GDI desktop surface can lag DWM by one compose, so a single
            // identical capture does not mean the scene is static. Retry
            // quickly and only back off after several identical frames.
            if (backdropMissCount_ < 8) {
                ++backdropMissCount_;
            }
            const double retry = backdropMissCount_ >= 8
                                     ? 0.15
                                     : (backdropMissCount_ >= 3 ? 0.03 : 0.012);
            nextBackdropCapture_ = now_ + retry;
            return;
        }
        backdropMissCount_ = 0;
        backdropSignature_ = signature;
    }
    if (!captured) {
        float existingW = 0.0f;
        float existingH = 0.0f;
        if (backdrop_ && SDL_GetTextureSize(backdrop_, &existingW, &existingH) &&
            static_cast<int>(existingW) == targetW && static_cast<int>(existingH) == targetH) {
            // Keep the existing backdrop instead of reloading the wallpaper.
            nextBackdropCapture_ = now_ + 0.5;
            return;
        }
        // Fallback: blur the desktop wallpaper instead of a flat material.
        const std::string wallpaper = platform_->wallpaperPath();
        if (SDL_Surface *source = wallpaper.empty() ? nullptr : IMG_Load(wallpaper.c_str())) {
            SDL_Surface *scaled = SDL_CreateSurface(targetW, targetH, SDL_PIXELFORMAT_RGBA32);
            if (scaled) {
                SDL_BlitSurfaceScaled(source, nullptr, scaled, nullptr, SDL_SCALEMODE_LINEAR);
                pixels.resize(static_cast<size_t>(targetW) * targetH * 4);
                SDL_LockSurface(scaled);
                for (int row = 0; row < targetH; ++row) {
                    std::memcpy(pixels.data() + static_cast<size_t>(row) * targetW * 4,
                                static_cast<const uint8_t *>(scaled->pixels) +
                                    static_cast<size_t>(row) * scaled->pitch,
                                static_cast<size_t>(targetW) * 4);
                }
                SDL_UnlockSurface(scaled);
                SDL_DestroySurface(scaled);
                capturedW = targetW;
                capturedH = targetH;
            }
            SDL_DestroySurface(source);
        }
    }
    if (capturedW <= 0 || capturedH <= 0 ||
        pixels.size() < static_cast<size_t>(capturedW) * capturedH * 4) {
        nextBackdropCapture_ = now_ + 0.2;
        return;
    }
    boxBlurRGBA(pixels, capturedW, capturedH,
                std::max(2, static_cast<int>(std::lround(11.0f * contentScale_ / downscale))), 2);
    desaturateRGBA(pixels, capturedW, capturedH, 0.35f);

    // Composite the tint and a 1 px border into the same rounded shape. Baking
    // them guarantees the border sits exactly on the material's edge, including
    // the four corners.
    const float radiusPx = std::max(2.0f, bar.h * 0.24f * contentScale_);
    const bool dark = isDarkTheme(settings_);
    const SDL_Color tint = dark ? rgba(16, 16, 16, 255) : rgba(248, 248, 250, 255);
    const float tintAmount = dark ? 0.52f : 0.55f;
    const float borderAlpha = dark ? 0.34f : 0.30f;
    const float borderValue = dark ? 1.0f : 0.0f;
    const float halfW = capturedW * 0.5f;
    const float halfH = capturedH * 0.5f;
    const float innerW = std::max(0.0f, halfW - radiusPx);
    const float innerH = std::max(0.0f, halfH - radiusPx);
    std::vector<uint8_t> baked(pixels.size(), 0);
    for (int y = 0; y < capturedH; ++y) {
        for (int x = 0; x < capturedW; ++x) {
            const float qx = std::abs(x + 0.5f - halfW) - innerW;
            const float qy = std::abs(y + 0.5f - halfH) - innerH;
            const float outside = std::sqrt(std::max(qx, 0.0f) * std::max(qx, 0.0f) +
                                            std::max(qy, 0.0f) * std::max(qy, 0.0f));
            const float distance = outside + std::min(std::max(qx, qy), 0.0f) - radiusPx;
            const float coverage = std::clamp(0.5f - distance, 0.0f, 1.0f);
            if (coverage <= 0.0f) {
                continue;
            }
            const float insideDistance = std::max(0.0f, -distance);
            const float borderMix = std::clamp(1.0f - insideDistance, 0.0f, 1.0f);
            const size_t index = (static_cast<size_t>(y) * capturedW + x) * 4;
            for (int c = 0; c < 3; ++c) {
                const float base = pixels[index + c] / 255.0f;
                const float tintValue = (c == 0 ? tint.r : c == 1 ? tint.g : tint.b) / 255.0f;
                float value = base * (1.0f - tintAmount) + tintValue * tintAmount;
                value = value * (1.0f - borderMix * borderAlpha) +
                        borderValue * borderMix * borderAlpha;
                baked[index + c] =
                    static_cast<uint8_t>(std::clamp(value * 255.0f, 0.0f, 255.0f));
            }
            baked[index + 3] = static_cast<uint8_t>(coverage * 240.0f);
        }
    }
    pixels.swap(baked);

    float textureW = 0.0f;
    float textureH = 0.0f;
    const bool sameSize =
        backdrop_ && SDL_GetTextureSize(backdrop_, &textureW, &textureH) &&
        static_cast<int>(textureW) == capturedW && static_cast<int>(textureH) == capturedH;
    if (!sameSize) {
        if (backdrop_) {
            SDL_DestroyTexture(backdrop_);
            backdrop_ = nullptr;
        }
        backdrop_ = SDL_CreateTexture(dockRenderer_, SDL_PIXELFORMAT_RGBA32,
                                      SDL_TEXTUREACCESS_STATIC, capturedW, capturedH);
        if (!backdrop_) {
            nextBackdropCapture_ = now_ + 0.2;
            return;
        }
    }
    SDL_UpdateTexture(backdrop_, nullptr, pixels.data(), capturedW * 4);
    SDL_SetTextureBlendMode(backdrop_, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(backdrop_, SDL_SCALEMODE_LINEAR);
    if (verbose_) {
        const double costMs = (SDL_GetPerformanceCounter() - captureStart) * 1000.0 /
                              static_cast<double>(SDL_GetPerformanceFrequency());
        const double captureMs =
            (captureDone - captureStart) * 1000.0 /
            static_cast<double>(SDL_GetPerformanceFrequency());
        RD_LOG_DEBUG("backdrop %dx%d capture %.2f ms total %.2f ms", capturedW, capturedH,
                     captureMs, costMs);
    }
    // Keep up with a moving scene: capture every frame the blur changed.
    nextBackdropCapture_ = now_ + 0.012;
    dirty_ = true;
}

void Application::saveConfig(bool force) {
    if (!settings_.dirty() && !modelDirty_ && !launchpadDirty_) {
        return;
    }
    if (!force) {
        // Coalesce rapid edits: never write mid-drag or mid-swipe, and wait for
        // a short idle gap so a drag across many slots produces one write.
        if (dragging_ || grid_.pressed || fileDrag_ || reorderAnimating_ ||
            grid_.swipeAnimating || grid_.blankPressed) {
            return;
        }
        if (now_ - lastConfigDirty_ < 2.0) {
            return;
        }
        if (now_ - lastConfigWrite_ < 3.0) {
            return;
        }
    }
    nlohmann::json root;
    root["version"] = 1;
    root["settings"] = settings_.toJson();
    root["items"] = model_.toJson();
    if (grid_.layoutReady) {
        root["launchpad"] = grid_.layout.toJson();
    }
    const std::string text = root.dump(2);
    if (writeTextFileAtomic(paths_.configFile(), text)) {
        settings_.clearDirty();
        modelDirty_ = false;
        launchpadDirty_ = false;
        lastConfigWrite_ = now_;
        RD_LOG_DEBUG("Saved config to %s", paths_.configFile().string().c_str());
    } else {
        RD_LOG_WARN("Failed to save config to %s", paths_.configFile().string().c_str());
    }
}

void Application::loadConfig() {
    const std::string text = readTextFile(paths_.configFile());
    if (text.empty()) {
        return;
    }
    try {
        nlohmann::json root = nlohmann::json::parse(text);
        if (root.contains("settings")) {
            settings_.applyJson(root["settings"]);
        }
        if (root.contains("items")) {
            model_.fromJson(root["items"]);
        }
        if (const auto found = root.find("launchpad"); found != root.end() &&
                                                         found->is_object()) {
            grid_.layout = launchpad::Layout::fromJson(*found);
            grid_.layoutReady = true;
        }
    } catch (const std::exception &error) {
        RD_LOG_WARN("Failed to parse config: %s", error.what());
    }
}

void Application::markConfigDirty(double now) {
    lastConfigDirty_ = now;
    dirty_ = true;
}

const AppInfo *Application::appForItem(const DockItem &item) const {
    if (!item.isApplication()) {
        return nullptr;
    }
    if (const AppInfo *app = apps_->findById(item.id)) {
        return app;
    }
    if (!item.path.empty()) {
        return apps_->findByPath(item.path);
    }
    return nullptr;
}

bool Application::isItemRunning(const DockItem &item) const {
    const AppInfo *app = appForItem(item);
    return app && apps_->isRunning(*app);
}

void Application::launchItem(const DockItem &item) {
    if (item.type == ItemType::Finder) {
        platform_->openFileManager();
        return;
    }
    if (item.type == ItemType::MinimizedWindow) {
        restoreMinimizedItem(item);
        return;
    }
    if (const AppInfo *app = appForItem(item)) {
        activateOrLaunch(*app);
    } else {
        platform_->launchItem(item);
    }
}

void Application::activateOrLaunch(const AppInfo &app) {
    noteRecentApp(app.id);
    if (!app.aumid.empty()) {
        // Packaged apps are activated through their AUMID; they do not expose a
        // stable executable path for the running-process poll, so skip the
        // bounce and show the recent tile immediately.
        platform_->launchApplication(app);
        rebuildVisibleItems();
        relayoutWindow();
        return;
    }
    if (!platform_->activateApplication(app)) {
        // Watch for the real window/process before launching: launcher stubs
        // (Office's wordicon.exe) and console hosts (cmd.exe under Windows
        // Terminal) do not match the shortcut target.
        LaunchWatch watch;
        watch.windows = platform_->runningExecutables();
        watch.foreground = platform_->foregroundProcessPath();
        watch.startedAt = now_;
        if (!platform_->launchApplication(app)) {
            return;
        }
        launchWatch_[app.id] = std::move(watch);
        if (settings_.launchAnimation()) {
            // Bounce until the process shows up as running (or the safety
            // timeout expires).
            bounce_[app.id] = now_;
            launching_.insert(app.id);
        }
        // Show the launching application in the Dock immediately, even when it
        // is not pinned and the running poll has not seen it yet.
        rebuildVisibleItems();
        relayoutWindow();
    }
}

std::string Application::fallbackFontPath() const {
#ifdef _WIN32
    wchar_t windowsDir[MAX_PATH];
    if (GetWindowsDirectoryW(windowsDir, MAX_PATH) > 0) {
        const std::filesystem::path fonts = std::filesystem::path(windowsDir) / "Fonts";
        for (const char *name : {"msyh.ttc", "msyh.ttf", "segoeui.ttf", "arial.ttf"}) {
            const auto candidate = fonts / name;
            if (std::filesystem::exists(candidate)) {
                return candidate.string();
            }
        }
    }
#else
    for (const char *path : {
             "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
             "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
             "/usr/share/fonts/opentype/noto/NotoSansSC-Regular.otf",
             "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
         }) {
        if (std::filesystem::exists(path)) {
            return path;
        }
    }
#endif
    return {};
}

void Application::noteRecentApp(const std::string &id) {
    if (id.empty()) {
        return;
    }
    auto found = std::find(recentApps_.begin(), recentApps_.end(), id);
    if (found != recentApps_.end()) {
        recentApps_.erase(found);
    }
    recentApps_.insert(recentApps_.begin(), id);
    if (recentApps_.size() > 12) {
        recentApps_.resize(12);
    }
    settings_.setRecentApps(recentApps_);
    markConfigDirty(clock_.now());
}

void Application::rebuildVisibleItems() {
    // Three Dock regions separated by two hairlines:
    //   1. Finder/Launchpad + pinned applications,
    //   2. recently used applications (all open unpinned ones, padded with
    //      recently used closed apps to at least three entries),
    //   3. folders/files, minimized windows and the Trash.
    std::vector<DockItem> pinned;
    std::vector<DockItem> folders;
    DockItem trash;
    bool hasTrash = false;
    for (const auto &entry : model_.items()) {
        if (entry.type == ItemType::Separator) {
            // The two region separators are synthesized below; stored separator
            // items are kept only for older configs.
            continue;
        }
        if (entry.type == ItemType::Trash) {
            trash = entry;
            hasTrash = true;
        } else if (entry.type == ItemType::Application || entry.type == ItemType::AppGrid ||
                   entry.type == ItemType::Finder) {
            pinned.push_back(entry);
        } else {
            folders.push_back(entry);
        }
    }

    std::vector<DockItem> recent;
    if (!settings_.staticOnly()) {
        std::set<std::string> pinnedIds;
        for (const auto &entry : pinned) {
            pinnedIds.insert(entry.id);
        }
        std::set<std::string> added;
        auto addRecent = [&](const AppInfo &app) {
            if (pinnedIds.count(app.id) > 0 || !added.insert(app.id).second) {
                return;
            }
            DockItem item;
            item.type = ItemType::Application;
            item.id = app.id;
            item.path = app.sourcePath;
            item.label = app.name;
            item.iconPath = app.iconPath;
            item.pinned = false;
            recent.push_back(std::move(item));
        };
        // All open unpinned apps first, in recency order.
        for (const std::string &id : recentApps_) {
            if (pinnedIds.count(id) > 0) {
                continue;
            }
            if (const AppInfo *app = apps_->findById(id); app && apps_->isRunning(*app)) {
                addRecent(*app);
            }
        }
        for (const AppInfo &app : apps_->apps()) {
            if (apps_->isRunning(app)) {
                addRecent(app);
            }
        }
        for (const std::string &id : launching_) {
            if (const AppInfo *app = apps_->findById(id)) {
                addRecent(*app);
            }
        }
        // Fewer than three entries: pad the region with recently used apps
        // that are no longer running.
        if (settings_.showRecents()) {
            const size_t target = static_cast<size_t>(std::max(3, settings_.recentCount()));
            for (const std::string &id : recentApps_) {
                if (recent.size() >= target) {
                    break;
                }
                if (pinnedIds.count(id) > 0) {
                    continue;
                }
                if (const AppInfo *app = apps_->findById(id)) {
                    addRecent(*app);
                }
            }
        }
    }

    std::vector<DockItem> minimized;
    std::set<std::uintptr_t> liveWindows;
    bool sawRestoringHandle = false;
    if (!settings_.staticOnly() && platform_ && dockRenderer_) {
        for (const auto &window : platform_->minimizedWindows()) {
            liveWindows.insert(window.handle);
            if (window.handle == restoringHandle_) {
                // The tile is already flying back to its window; keep it out
                // of the bar so neighbouring icons close the gap immediately.
                sawRestoringHandle = true;
                continue;
            }
            // GPU windows occasionally capture as an all-black rectangle. Treat
            // a thumbnail with almost no lit pixels as a failed capture and use
            // the app icon instead.
            size_t litPixels = 0;
            size_t opaquePixels = 0;
            for (size_t p = 0; p + 3 < window.rgba.size(); p += 4) {
                if (window.rgba[p + 3] <= 8) {
                    continue;
                }
                ++opaquePixels;
                if (window.rgba[p] + window.rgba[p + 1] + window.rgba[p + 2] > 60) {
                    ++litPixels;
                }
            }
            const bool blankThumbnail =
                opaquePixels == 0 || litPixels * 50 < opaquePixels;
            if (windowThumbnails_.count(window.handle) == 0 && !window.rgba.empty() &&
                window.width > 0 && window.height > 0 && !blankThumbnail) {
                SDL_Texture *texture =
                    SDL_CreateTexture(dockRenderer_, SDL_PIXELFORMAT_RGBA32,
                                      SDL_TEXTUREACCESS_STATIC, window.width, window.height);
                if (texture) {
                    SDL_UpdateTexture(texture, nullptr, window.rgba.data(), window.width * 4);
                    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
                    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
                    windowThumbnails_[window.handle] = texture;
                }
            }
            DockItem item;
            item.type = ItemType::MinimizedWindow;
            item.id = "window:" + std::to_string(window.handle);
            item.path = window.execPath;
            item.label = window.title;
            item.windowHandle = window.handle;
            item.pinned = false;
            if (window.width > 0 && window.height > 0) {
                item.previewAspect = static_cast<float>(window.width) /
                                     static_cast<float>(window.height);
            }
            if (item.label.empty()) {
                if (const AppInfo *app = apps_->findByPath(window.execPath)) {
                    item.label = app->name;
                }
            }
            minimized.push_back(std::move(item));
        }
    }
    if (restoringHandle_ != 0 && !sawRestoringHandle) {
        // The window has come back; the tile stays hidden from now on.
        restoringHandle_ = 0;
        restoringClearAt_ = 0.0;
    }
    for (auto it = windowThumbnails_.begin(); it != windowThumbnails_.end();) {
        if (liveWindows.count(it->first) == 0) {
            if (it->second) {
                SDL_DestroyTexture(it->second);
            }
            it = windowThumbnails_.erase(it);
        } else {
            ++it;
        }
    }

    // The minimize animation inserts its tile before the platform's periodic
    // minimized-window poll catches up. Keep that placeholder until the poll
    // reports the same handle (then the poll owns it) or it becomes stale.
    if (hasPendingMinimized_) {
        bool reported = false;
        for (const auto &item : minimized) {
            if (item.windowHandle == pendingMinimized_.windowHandle) {
                reported = true;
                break;
            }
        }
        if (reported) {
            hasPendingMinimized_ = false;
        } else if (now_ - pendingMinimizedAt_ > 3.0) {
            hasPendingMinimized_ = false;
        } else {
            minimized.push_back(pendingMinimized_);
            liveWindows.insert(pendingMinimized_.windowHandle);
        }
    }

    auto makeSeparator = [](const char *id) {
        DockItem separator;
        separator.type = ItemType::Separator;
        separator.id = id;
        separator.pinned = false;
        return separator;
    };
    const bool rightSide = !folders.empty() || !minimized.empty() || hasTrash;

    visibleItems_.clear();
    visibleItems_.reserve(pinned.size() + recent.size() + folders.size() + minimized.size() + 3);
    visibleItems_.insert(visibleItems_.end(), pinned.begin(), pinned.end());
    if (!pinned.empty() && (!recent.empty() || rightSide)) {
        visibleItems_.push_back(makeSeparator("special:separator:pinned"));
    }
    visibleItems_.insert(visibleItems_.end(), recent.begin(), recent.end());
    if (!recent.empty() && rightSide) {
        visibleItems_.push_back(makeSeparator("special:separator:recent"));
    }
    visibleItems_.insert(visibleItems_.end(), folders.begin(), folders.end());
    visibleItems_.insert(visibleItems_.end(), minimized.begin(), minimized.end());
    if (hasTrash) {
        visibleItems_.push_back(std::move(trash));
    }

    std::vector<std::string> ids;
    ids.reserve(visibleItems_.size());
    for (const auto &item : visibleItems_) {
        ids.push_back(item.id);
    }
    if (ids != lastVisibleIds_) {
        if (!lastVisibleIds_.empty() && dockWindow_ && !layoutResult_.tiles.empty()) {
            int wx = 0;
            int wy = 0;
            SDL_GetWindowPosition(dockWindow_, &wx, &wy);
            const bool horizontal = settings_.orientation() == "bottom";
            const size_t count = std::min(layoutResult_.tiles.size(), lastVisibleIds_.size());
            for (size_t i = 0; i < count; ++i) {
                slotAnimFromScreen_[lastVisibleIds_[i]] =
                    horizontal ? static_cast<float>(wx) + layoutResult_.tiles[i].cell.x
                               : static_cast<float>(wy) + layoutResult_.tiles[i].cell.y;
            }
            slotAnimPending_ = true;
        }
        lastVisibleIds_ = std::move(ids);
    }
}


void Application::toggleDockItem(const AppInfo &app) {
    const int index = model_.indexOfApplication(app.id);
    if (index >= 0) {
        model_.removeAt(static_cast<size_t>(index));
    } else {
        DockItem item;
        item.type = ItemType::Application;
        item.id = app.id;
        item.path = app.sourcePath;
        item.label = app.name;
        item.iconPath = app.iconPath;
        int insertAt = static_cast<int>(model_.items().size());
        for (int i = static_cast<int>(model_.items().size()) - 1; i >= 0; --i) {
            if (model_.items()[i].type == ItemType::Trash ||
                model_.items()[i].type == ItemType::Separator) {
                insertAt = i;
            }
        }
        model_.addItem(std::move(item), insertAt);
    }
    modelDirty_ = true;
    markConfigDirty(now_);
    rebuildVisibleItems();
    relayoutWindow();
}

void Application::openMenuForItem(size_t index, float globalX, float globalY) {
    if (index >= visibleItems_.size()) {
        return;
    }
    const DockItem item = visibleItems_[index];
    float anchorX = globalX;
    float tipY = globalY;
    int windowX = 0;
    int windowY = 0;
    SDL_GetWindowPosition(dockWindow_, &windowX, &windowY);
    SDL_FRect bar = layoutResult_.bar;
    bar.y += dockHideOffset();
    if (item.type == ItemType::Separator) {
        // The separator menu is the Dock menu; anchor it above the bar so it
        // never covers the Dock itself.
        anchorX = static_cast<float>(windowX) + bar.x + bar.w * 0.5f;
        tipY = static_cast<float>(windowY) + bar.y - 4.0f;
        openMenu(dockMenuItems(item.id), anchorX, tipY);
        return;
    }

    const AppInfo *app = appForItem(item);
    const bool persistent = modelContainsId(item.id);
    const bool running = app && apps_->isRunning(*app);
    const bool responding = !app || !running || platform_->isApplicationResponding(*app);

    std::vector<MenuItem> items;
    auto add = [&](std::string label, MenuAction action, std::string target = {},
                   bool separator = false, bool enabled = true, bool checked = false) {
        MenuItem entry;
        entry.label = std::move(label);
        entry.action = action;
        entry.targetId = std::move(target);
        entry.separator = separator;
        entry.enabled = enabled;
        entry.checked = checked;
        items.push_back(std::move(entry));
        return static_cast<int>(items.size()) - 1;
    };
    auto addSeparator = [&]() { add("", MenuAction::None, "", true); };
    auto addHeader = [&](std::string label) {
        MenuItem entry;
        entry.label = std::move(label);
        entry.header = true;
        entry.enabled = false;
        items.push_back(std::move(entry));
    };
    auto addSubmenu = [&](std::string label, std::vector<MenuItem> submenu) {
        MenuItem entry;
        entry.label = std::move(label);
        entry.submenu = std::move(submenu);
        items.push_back(std::move(entry));
    };
    auto menuLeaf = [](std::string label, MenuAction action, std::string target = {},
                       std::string argument = {}, bool checked = false) {
        MenuItem entry;
        entry.label = std::move(label);
        entry.action = action;
        entry.targetId = std::move(target);
        entry.argument = std::move(argument);
        entry.checked = checked;
        return entry;
    };
    auto openLabel = [&](const DockItem &entry) {
        if (entry.type == ItemType::Application) {
            return std::string("Open");
        }
        if (entry.type == ItemType::Trash) {
            return std::string("Open Trash");
        }
        if (entry.type == ItemType::Finder) {
            return std::string("Open Finder");
        }
        if (entry.type == ItemType::AppGrid) {
            return grid_.open ? std::string("Close Launchpad") : std::string("Open Launchpad");
        }
        if (!entry.label.empty()) {
            return "Open \xE2\x80\x9C" + entry.label + "\xE2\x80\x9D";
        }
        return std::string("Open");
    };
    auto optionsSubmenu = [&](bool allowKeep) {
        std::vector<MenuItem> options;
        if (app && allowKeep && !persistent) {
            options.push_back(menuLeaf("Keep in Dock", MenuAction::AddToDock, app->id));
        }
        if (persistent) {
            options.push_back(
                menuLeaf("Remove from Dock", MenuAction::RemoveFromDock, item.id));
        }
        if (app && !app->sourcePath.empty() && app->aumid.empty()) {
            options.push_back(menuLeaf("Open at Login", MenuAction::OpenAtLogin, app->id, {},
                                       platform_->isOpenAtLogin(*app)));
        }
        if (!item.path.empty()) {
            options.push_back(
                menuLeaf(platform_->revealLabel(), MenuAction::RevealItem, item.id));
        }
        return options;
    };

    if (item.type == ItemType::MinimizedWindow) {
        add("Restore Window", MenuAction::OpenItem, item.id);
        add("Close Window", MenuAction::CloseWindow, item.id);
    } else if (item.type == ItemType::Trash) {
        add("Open Trash", MenuAction::OpenTrash);
        addSeparator();
        add("Empty Trash", MenuAction::EmptyTrash, "", false, !trashEmpty_);
    } else if (item.type == ItemType::AppGrid) {
        add(grid_.open ? "Close Launchpad" : "Open Launchpad", MenuAction::OpenGrid);
        addSeparator();
        add("Refresh Applications", MenuAction::RefreshApplications);
    } else if (item.type == ItemType::Finder) {
        add("Open Finder", MenuAction::OpenItem, item.id);
        addSeparator();
        add("Show Labels", MenuAction::ToggleLabels, "", false, true,
            settings_.showLabels());
    } else if (item.type == ItemType::Folder) {
        std::vector<MenuItem> sort;
        sort.push_back(menuLeaf("Name", MenuAction::SetStackArrangement, item.id, "1",
                                item.arrangement == 1));
        sort.push_back(menuLeaf("Date Added", MenuAction::SetStackArrangement, item.id, "2",
                                item.arrangement == 2));
        sort.push_back(menuLeaf("Date Modified", MenuAction::SetStackArrangement, item.id,
                                "3", item.arrangement == 3));
        sort.push_back(menuLeaf("Date Created", MenuAction::SetStackArrangement, item.id,
                                "4", item.arrangement == 4));
        sort.push_back(menuLeaf("Kind", MenuAction::SetStackArrangement, item.id, "5",
                                item.arrangement == 5));
        addSubmenu("Sort By", std::move(sort));
        std::vector<MenuItem> display;
        display.push_back(menuLeaf("Folder", MenuAction::SetFolderDisplay, item.id, "folder",
                                   !item.stack));
        display.push_back(
            menuLeaf("Stack", MenuAction::SetFolderDisplay, item.id, "stack", item.stack));
        addSubmenu("Display as", std::move(display));
        addSeparator();
        std::vector<MenuItem> view;
        view.push_back(menuLeaf("Fan", MenuAction::SetStackFan, item.id, {}, item.showAs == 1));
        view.push_back(menuLeaf("Grid", MenuAction::SetStackGrid, item.id, {}, item.showAs == 2));
        view.push_back(
            menuLeaf("Automatic", MenuAction::SetStackAuto, item.id, {}, item.showAs == 0));
        addSubmenu("View Content As", std::move(view));
        addSeparator();
        auto options = optionsSubmenu(false);
        if (!options.empty()) {
            addSubmenu("Options", std::move(options));
        }
        addSeparator();
        add(openLabel(item), MenuAction::OpenItem, item.id);
    } else if (item.type == ItemType::File || item.type == ItemType::Url) {
        auto options = optionsSubmenu(false);
        if (!options.empty()) {
            addSubmenu("Options", std::move(options));
            addSeparator();
        }
        add(openLabel(item), MenuAction::OpenItem, item.id);
    } else if (item.type == ItemType::Application) {
        bool addedAppItems = false;
        if (app) {
            const auto appEntries = platform_->appMenuItems(*app);
            bool addedTask = false;
            bool addedRecent = false;
            for (const auto &entry : appEntries) {
                if (entry.recent && !addedRecent) {
                    if (addedTask) {
                        addSeparator();
                    }
                    addHeader("Recent");
                    addedRecent = true;
                } else if (!entry.recent) {
                    addedTask = true;
                }
                MenuItem menuItem;
                menuItem.label = entry.label;
                menuItem.action = MenuAction::RunAppMenuItem;
                menuItem.targetId = app->id;
                menuItem.argument = entry.id;
                menuItem.secondary = entry.target;
                menuItem.detail = entry.detail;
                if (!entry.iconPath.empty()) {
                    menuItem.iconPath = entry.iconPath;
                } else if (!entry.target.empty()) {
                    if (isWebAddress(entry.target)) {
                        menuItem.iconPath =
                            (paths_.assetDir / "icons" / "url.png").string();
                    } else {
                        menuItem.iconPath = entry.target;
                    }
                }
                items.push_back(std::move(menuItem));
                addedAppItems = true;
            }
            if (addedAppItems) {
                addSeparator();
            }
        }
        if (!responding) {
            add("Application Not Responding", MenuAction::None, "", false, false);
            addSeparator();
        }
        auto options = optionsSubmenu(true);
        if (!options.empty()) {
            addSubmenu("Options", std::move(options));
        }
        if (running) {
            addSeparator();
            add("Show All Windows", MenuAction::ShowAllWindows, item.id);
            if (platform_->canHideApplications()) {
                if (platform_->isApplicationHidden(*app)) {
                    add("Show", MenuAction::ShowApplication, item.id);
                } else {
                    add("Hide", MenuAction::HideApplication, item.id);
                }
                add("Hide Others", MenuAction::HideOthers, item.id);
            }
            add("Quit", MenuAction::QuitApplication, item.id);
            if (!responding) {
                add("Force Quit", MenuAction::ForceQuitApplication, item.id);
            }
        } else {
            addSeparator();
            add(openLabel(item), MenuAction::OpenItem, item.id);
        }
    } else {
        add(openLabel(item), MenuAction::OpenItem, item.id);
    }

    if (index < layoutResult_.tiles.size()) {
        const auto &tile = layoutResult_.tiles[index];
        anchorX = static_cast<float>(windowX) + tile.icon.x + tile.icon.w * 0.5f;
        tipY = static_cast<float>(windowY) + tile.icon.y + dockHideOffset() - 2.0f;
    }
    openMenu(std::move(items), anchorX, tipY);
}

std::vector<Application::MenuItem> Application::dockMenuItems(const std::string &separatorId) const {
    std::vector<MenuItem> items;
    auto add = [&](std::string label, MenuAction action, std::string target = {},
                   bool separator = false, bool checked = false) {
        MenuItem entry;
        entry.label = std::move(label);
        entry.action = action;
        entry.targetId = std::move(target);
        entry.separator = separator;
        entry.checked = checked;
        items.push_back(std::move(entry));
    };
    auto leaf = [](std::string label, MenuAction action, std::string argument = {},
                   bool checked = false) {
        MenuItem entry;
        entry.label = std::move(label);
        entry.action = action;
        entry.argument = std::move(argument);
        entry.checked = checked;
        return entry;
    };
    if (!separatorId.empty() && modelContainsId(separatorId)) {
        add("Remove Separator", MenuAction::RemoveFromDock, separatorId);
        add("", MenuAction::None, "", true);
    }
    add(settings_.autohide() ? "Turn Hiding Off" : "Turn Hiding On",
        MenuAction::ToggleAutohide);
    add(settings_.magnification() ? "Turn Magnification Off" : "Turn Magnification On",
        MenuAction::ToggleMagnification);
    add("Position on Screen", MenuAction::None);
    items.back().submenu = {
        leaf("Left", MenuAction::SetOrientation, "left", settings_.orientation() == "left"),
        leaf("Bottom", MenuAction::SetOrientation, "bottom",
             settings_.orientation() == "bottom"),
        leaf("Right", MenuAction::SetOrientation, "right", settings_.orientation() == "right"),
    };
    add("Minimize Using", MenuAction::None);
    items.back().submenu = {
        leaf("Genie Effect", MenuAction::SetMineffect, "genie",
             settings_.mineffect() == "genie"),
        leaf("Scale Effect", MenuAction::SetMineffect, "scale",
             settings_.mineffect() == "scale"),
    };
    add("", MenuAction::None, "", true);
    add("Dock Preferences\xE2\x80\xA6", MenuAction::OpenSettings);
    add("", MenuAction::None, "", true);
    add("Quit RevDock", MenuAction::QuitDock);
    return items;
}

void Application::openMenuForBackground(float globalX, float globalY) {
    (void)globalY;
    int windowX = 0;
    int windowY = 0;
    SDL_GetWindowPosition(dockWindow_, &windowX, &windowY);
    SDL_FRect bar = layoutResult_.bar;
    bar.y += dockHideOffset();
    const float anchorX = std::clamp(globalX, static_cast<float>(windowX) + bar.x + 8.0f,
                                     static_cast<float>(windowX) + bar.x + bar.w - 8.0f);
    const float tipY = static_cast<float>(windowY) + bar.y - 4.0f;
    openMenu(dockMenuItems(""), anchorX, tipY);
}

SDL_Texture *Application::buildPopoverTexture(SDL_Window *captureWindow, SDL_Renderer *renderer,
                                              float x, float y, float width, float height,
                                              float radius, float arrowWidth, float arrowHeight,
                                              float tipX, bool dark, bool allowCapture,
                                              bool triangle, float blurRadius,
                                              float tintOverride) {
    const int w = std::max(1, static_cast<int>(std::lround(width * contentScale_)));
    const int h = std::max(1, static_cast<int>(std::lround(height * contentScale_)));
    const int arrowW = std::max(4, static_cast<int>(std::lround(arrowWidth * contentScale_)));
    const int arrowH = triangle
                           ? std::max(3, static_cast<int>(std::lround(arrowHeight * contentScale_)))
                           : 0;
    const int panelH = std::max(1, h - arrowH);
    const int radiusPx = std::max(2, static_cast<int>(std::lround(radius * contentScale_)));
    const float tipXPx = clampf(tipX * contentScale_, static_cast<float>(arrowW) * 0.5f,
                                static_cast<float>(w) - static_cast<float>(arrowW) * 0.5f);

    std::vector<uint8_t> pixels;
    int capturedW = 0;
    int capturedH = 0;
    const bool captured = allowCapture && platform_->captureWindowBackdrop(
        captureWindow, static_cast<int>(std::lround(x * contentScale_)),
        static_cast<int>(std::lround(y * contentScale_)), w, h, pixels, capturedW, capturedH);
    if (captured && (capturedW != w || capturedH != h)) {
        pixels.clear();
    }
    if (pixels.size() < static_cast<size_t>(w) * h * 4) {
        pixels.assign(static_cast<size_t>(w) * h * 4, 255);
        const uint8_t base = dark ? 36 : 238;
        for (size_t i = 0; i < pixels.size(); i += 4) {
            pixels[i + 0] = base;
            pixels[i + 1] = base;
            pixels[i + 2] = base;
            pixels[i + 3] = 255;
        }
    } else {
        boxBlurRGBA(pixels, w, h,
                    std::max(3, static_cast<int>(std::lround(blurRadius * contentScale_))), 2);
        desaturateRGBA(pixels, w, h, 0.55f);
    }

    const SDL_Color tint = dark ? rgba(20, 20, 20, 255) : rgba(248, 248, 248, 255);
    const SDL_Color border = dark ? rgba(255, 255, 255, 52) : rgba(0, 0, 0, 45);
    float tintAmount = captured ? (dark ? 0.80f : 0.72f) : 1.0f;
    if (captured && tintOverride >= 0.0f) {
        tintAmount = tintOverride;
    }
    std::vector<uint8_t> masked(static_cast<size_t>(w) * h * 4, 0);
    auto inside = [&](int px, int py) {
        if (px < 0 || py < 0 || px >= w || py >= h) {
            return false;
        }
        if (py < panelH && roundedContains(px + 0.5f, py + 0.5f, static_cast<float>(w),
                                           static_cast<float>(panelH), static_cast<float>(radiusPx))) {
            return true;
        }
        return triangle && triangleContains(px + 0.5f, py + 0.5f, tipXPx,
                                             static_cast<float>(panelH) - 1.0f,
                                             static_cast<float>(arrowW) * 0.5f,
                                             static_cast<float>(h) - 1.0f);
    };

    for (int py = 0; py < h; ++py) {
        for (int px = 0; px < w; ++px) {
            int samples = 0;
            for (int sy = 0; sy < 2; ++sy) {
                for (int sx = 0; sx < 2; ++sx) {
                    const float fx = px + (sx + 0.5f) * 0.5f;
                    const float fy = py + (sy + 0.5f) * 0.5f;
                    const bool panelHit = fy < panelH &&
                                          roundedContains(fx, fy, static_cast<float>(w),
                                                          static_cast<float>(panelH),
                                                          static_cast<float>(radiusPx));
                    const bool triangleHit =
                        triangle && triangleContains(fx, fy, tipXPx,
                                                     static_cast<float>(panelH) - 1.0f,
                                                     static_cast<float>(arrowW) * 0.5f,
                                                     static_cast<float>(h) - 1.0f);
                    samples += (panelHit || triangleHit) ? 1 : 0;
                }
            }
            if (samples == 0) {
                continue;
            }
            const size_t index = (static_cast<size_t>(py) * w + px) * 4;
            const float coverage = samples / 4.0f;
            const bool onBorder = !inside(px - 1, py) || !inside(px + 1, py) ||
                                  !inside(px, py - 1) || !inside(px, py + 1);
            // 1) Composite the frosted tint over the captured backdrop.
            float rgb[3];
            for (int c = 0; c < 3; ++c) {
                const float background = pixels[index + c] / 255.0f;
                const float tintValue =
                    (c == 0 ? tint.r : c == 1 ? tint.g : tint.b) / 255.0f;
                rgb[c] = background * (1.0f - tintAmount) + tintValue * tintAmount;
            }
            // 2) Blend the hairline border over it using the border's own alpha,
            //    so the edge stays a subtle 1px line instead of solid white.
            if (onBorder) {
                const float borderAlpha = border.a / 255.0f;
                for (int c = 0; c < 3; ++c) {
                    const float borderValue =
                        (c == 0 ? border.r : c == 1 ? border.g : border.b) / 255.0f;
                    rgb[c] = rgb[c] * (1.0f - borderAlpha) + borderValue * borderAlpha;
                }
            }
            for (int c = 0; c < 3; ++c) {
                masked[index + c] =
                    static_cast<uint8_t>(std::clamp(rgb[c] * 255.0f, 0.0f, 255.0f));
            }
            masked[index + 3] = static_cast<uint8_t>(coverage * 232.0f);
        }
    }

    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                             SDL_TEXTUREACCESS_STATIC, w, h);
    if (texture) {
        SDL_UpdateTexture(texture, nullptr, masked.data(), w * 4);
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    }
    return texture;
}

void Application::raiseDockLayers() {
    if (dockWindow_) {
        platform_->raiseAlwaysOnTop(dockWindow_);
    }
    if (stack_.open && stack_.window) {
        platform_->raiseAlwaysOnTop(stack_.window);
    }
    if (menu_.open && menu_.window) {
        // The menu must stay above the Dock and the Launchpad grid.
        platform_->raiseAlwaysOnTop(menu_.window);
    }
    if (submenu_.open && submenu_.window) {
        platform_->raiseAlwaysOnTop(submenu_.window);
    }
}

void Application::openMenu(std::vector<MenuItem> items, float anchorX, float tipY) {
    closeSubmenu();
    openMenuPanel(menu_, std::move(items), anchorX, tipY, true, 0.0f, 0.0f);
}

void Application::openMenuPanel(MenuState &state, std::vector<MenuItem> items, float anchorX,
                                float tipY, bool anchored, float left, float top) {
    if (!state.window) {
        createMenuWindow(state, anchored ? "RevDock Menu" : "RevDock Submenu");
    }
    if (!state.window || !state.text) {
        return;
    }
    state.items = std::move(items);
    state.hover = -1;
    state.fontPx = kMenuFontSize;
    state.hasChecks = false;
    state.hasIcons = false;
    for (const auto &item : state.items) {
        state.hasChecks = state.hasChecks || item.checked;
        state.hasIcons = state.hasIcons || !item.iconPath.empty();
    }

    float maxWidth = 140.0f;
    for (const auto &item : state.items) {
        if (item.separator) {
            continue;
        }
        if (item.header) {
            maxWidth = std::max(maxWidth, state.text->measure(item.label, 11.0f).x + 30.0f);
            continue;
        }
        float width = state.text->measure(item.label, state.fontPx).x + 32.0f;
        if (!item.detail.empty()) {
            width += state.text->measure(item.detail, 11.0f).x + 18.0f;
        }
        if (!item.submenu.empty()) {
            width += 16.0f;
        }
        maxWidth = std::max(maxWidth, width);
    }
    maxWidth += (state.hasChecks ? 12.0f : 0.0f) + (state.hasIcons ? 20.0f : 0.0f);
    state.width = clampf(maxWidth, 150.0f, 520.0f);
    state.panelHeight = kMenuPadding * 2.0f + menuContentHeight(state);
    state.arrowHeight = anchored ? 8.0f : 0.0f;
    state.height = state.panelHeight + state.arrowHeight;

    float x = left;
    float y = top;
    if (anchored) {
        x = anchorX - state.width * 0.5f;
        y = tipY - state.height - 2.0f;
        if (y < static_cast<float>(displayBounds_.y + 8)) {
            y = tipY + 8.0f;
        }
    }
    x = clampf(x, static_cast<float>(displayBounds_.x + 8),
               static_cast<float>(displayBounds_.x + displayBounds_.w) - state.width - 8.0f);
    y = clampf(y, static_cast<float>(displayBounds_.y + 8),
               static_cast<float>(displayBounds_.y + displayBounds_.h) - state.height - 8.0f);
    const float tipX = clampf(anchorX - x, state.arrowWidth * 0.5f,
                              state.width - state.arrowWidth * 0.5f);
    state.x = x;
    state.y = y;

    SDL_SetWindowSize(state.window, static_cast<int>(state.width),
                      static_cast<int>(state.height));
    SDL_SetWindowPosition(state.window, static_cast<int>(x), static_cast<int>(y));
    SDL_SyncWindow(state.window);

    if (state.backdrop) {
        SDL_DestroyTexture(state.backdrop);
        state.backdrop = nullptr;
    }
    state.backdrop = buildPopoverTexture(
        state.window, state.renderer, 0.0f, 0.0f, state.width, state.height, 9.0f,
        state.arrowWidth, state.arrowHeight, tipX, isDarkTheme(settings_), true, anchored);
    state.open = true;
    forceRender_ = true;
    renderMenuPanel(state);
    SDL_SetWindowAlwaysOnTop(state.window, true);
    SDL_ShowWindow(state.window);
    raiseDockLayers();
    SDL_RaiseWindow(state.window);
    platform_->raiseAlwaysOnTop(state.window);
    SDL_SyncWindow(state.window);
}

void Application::createMenuWindow(MenuState &state, const char *title) {
    state.window = SDL_CreateWindow(title, 200, 200,
        static_cast<SDL_WindowFlags>(SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP |
                                     SDL_WINDOW_TRANSPARENT | SDL_WINDOW_UTILITY |
                                     SDL_WINDOW_HIGH_PIXEL_DENSITY));
    if (!state.window) {
        RD_LOG_WARN("Menu window creation failed: %s", SDL_GetError());
        return;
    }
    state.renderer = SDL_CreateRenderer(state.window, nullptr);
    if (!state.renderer) {
        SDL_DestroyWindow(state.window);
        state.window = nullptr;
        return;
    }
    SDL_SetRenderVSync(state.renderer, 1);
    SDL_SetRenderDrawBlendMode(state.renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderScale(state.renderer, contentScale_, contentScale_);
    state.draw = std::make_unique<Renderer>(state.renderer, contentScale_);
    const std::filesystem::path regular = paths_.assetDir / "fonts" / "Inter" / "Inter-Regular.ttf";
    state.text = std::make_unique<TextRenderer>(state.renderer, regular.string(), fallbackFontPath());
    if (dockWindow_) {
        // Owned by the Dock so the popup always renders above the Dock and the
        // Launchpad grid, like a real macOS Dock menu.
        platform_->setWindowOwner(state.window, dockWindow_);
    }
    SDL_HideWindow(state.window);
}

void Application::closeSubmenu() {
    if (!submenu_.open && submenu_.items.empty()) {
        menu_.submenuParentRow = -1;
        menu_.submenuPendingRow = -1;
        return;
    }
    submenu_.open = false;
    submenu_.hover = -1;
    submenu_.items.clear();
    clearMenuIcons(submenu_);
    if (submenu_.backdrop) {
        SDL_DestroyTexture(submenu_.backdrop);
        submenu_.backdrop = nullptr;
    }
    if (submenu_.window) {
        SDL_HideWindow(submenu_.window);
    }
    menu_.submenuParentRow = -1;
    menu_.submenuPendingRow = -1;
    dirty_ = true;
}

void Application::closeMenu() {
    closeSubmenu();
    if (!menu_.open) {
        menu_.items.clear();
        clearMenuIcons(menu_);
        return;
    }
    menu_.open = false;
    menu_.hover = -1;
    menu_.items.clear();
    clearMenuIcons(menu_);
    if (menu_.backdrop) {
        SDL_DestroyTexture(menu_.backdrop);
        menu_.backdrop = nullptr;
    }
    if (menu_.window) {
        SDL_HideWindow(menu_.window);
    }
    dirty_ = true;
}

void Application::executeMenu(const MenuItem &item) {
    auto findVisible = [&](const std::string &id) -> const DockItem * {
        for (const auto &entry : visibleItems_) {
            if (entry.id == id) {
                return &entry;
            }
        }
        return nullptr;
    };
    auto findPersistent = [&](const std::string &id) -> DockItem * {
        for (auto &entry : model_.items()) {
            if (entry.id == id) {
                return &entry;
            }
        }
        return nullptr;
    };

    switch (item.action) {
    case MenuAction::OpenItem:
        if (const DockItem *target = findVisible(item.targetId)) {
            if (target->type == ItemType::Folder && target->stack) {
                float gx = 0.0f;
                float gy = 0.0f;
                platform_->globalCursor(gx, gy);
                openStack(*target, gx, gy);
            } else {
                launchItem(*target);
            }
        }
        break;
    case MenuAction::RevealItem:
        if (const DockItem *target = findVisible(item.targetId)) {
            platform_->revealPath(target->path);
        }
        break;
    case MenuAction::AddToDock:
        if (const AppInfo *app = apps_->findById(item.targetId)) {
            toggleDockItem(*app);
        }
        break;
    case MenuAction::RemoveFromDock: {
        if (findPersistent(item.targetId)) {
            for (size_t i = 0; i < model_.items().size(); ++i) {
                if (model_.items()[i].id == item.targetId) {
                    model_.removeAt(i);
                    modelDirty_ = true;
                    markConfigDirty(now_);
                    rebuildVisibleItems();
                    relayoutWindow();
                    break;
                }
            }
        }
        break;
    }
    case MenuAction::OpenTrash:
        platform_->openTrash();
        break;
    case MenuAction::EmptyTrash:
        platform_->emptyTrash();
        trashEmpty_ = true;
        dirty_ = true;
        break;
    case MenuAction::ToggleMagnification:
        settings_.setMagnification(!settings_.magnification());
        markConfigDirty(now_);
        relayoutWindow();
        break;
    case MenuAction::ToggleMacIcons:
        settings_.setMacIcons(!settings_.macIcons());
        if (icons_) {
            icons_->setMacStyle(settings_.macIcons());
        }
        if (grid_.icons) {
            grid_.icons->setMacStyle(settings_.macIcons());
        }
        markConfigDirty(now_);
        forceRender_ = true;
        dirty_ = true;
        break;
    case MenuAction::ToggleAutohide:
        settings_.setAutohide(!settings_.autohide());
        markConfigDirty(now_);
        break;
    case MenuAction::ToggleLabels:
        settings_.setShowLabels(!settings_.showLabels());
        markConfigDirty(now_);
        break;
    case MenuAction::RefreshApplications:
        apps_->refresh(true);
        if (grid_.icons) {
            grid_.icons->clear();
        }
        rebuildVisibleItems();
        relayoutWindow();
        dirty_ = true;
        break;
    case MenuAction::OpenGrid:
        if (grid_.open) {
            closeGrid();
        } else {
            openGrid();
        }
        break;
    case MenuAction::OpenSettings:
        openSettings();
        break;
    case MenuAction::QuitDock:
        running_ = false;
        break;
    case MenuAction::QuitApplication:
        if (const AppInfo *app = apps_->findById(item.targetId)) {
            platform_->quitApplication(*app);
        }
        break;
    case MenuAction::ForceQuitApplication:
        if (const AppInfo *app = apps_->findById(item.targetId)) {
            platform_->forceQuitApplication(*app);
        }
        break;
    case MenuAction::HideApplication:
        if (const AppInfo *app = apps_->findById(item.targetId)) {
            platform_->hideApplication(*app);
        }
        break;
    case MenuAction::HideOthers:
        if (const AppInfo *app = apps_->findById(item.targetId)) {
            platform_->hideOtherApplications(*app);
        }
        break;
    case MenuAction::ShowApplication:
        if (const AppInfo *app = apps_->findById(item.targetId)) {
            platform_->showAllWindows(*app);
        }
        break;
    case MenuAction::OpenAtLogin:
        if (const AppInfo *app = apps_->findById(item.targetId)) {
            const bool enabled = !platform_->isOpenAtLogin(*app);
            platform_->setOpenAtLogin(*app, enabled);
        }
        break;
    case MenuAction::RunAppMenuItem:
        if (const AppInfo *app = apps_->findById(item.targetId)) {
            AppMenuItem command;
            command.id = item.argument;
            command.label = item.label;
            command.target = item.secondary;
            command.iconPath = item.iconPath;
            platform_->runAppMenuItem(*app, command);
        }
        break;
    case MenuAction::SetOrientation:
        settings_.setOrientation(item.argument);
        markConfigDirty(now_);
        relayoutWindow();
        break;
    case MenuAction::SetMineffect:
        settings_.setMineffect(item.argument);
        markConfigDirty(now_);
        break;
    case MenuAction::CloseWindow:
        if (const DockItem *target = findVisible(item.targetId)) {
            platform_->closeWindow(target->windowHandle);
            rebuildVisibleItems();
            relayoutWindow();
        }
        break;
    case MenuAction::ShowAllWindows:
        if (const DockItem *target = findVisible(item.targetId)) {
            if (const AppInfo *app = appForItem(*target)) {
                platform_->showAllWindows(*app);
            }
        }
        break;
    case MenuAction::MinimizeApplication:
        if (const DockItem *target = findVisible(item.targetId)) {
            if (const AppInfo *app = appForItem(*target)) {
                platform_->minimizeApplication(*app);
            }
        }
        break;
    case MenuAction::SetFolderDisplay:
        for (auto &entry : model_.items()) {
            if (entry.id != item.targetId) {
                continue;
            }
            entry.stack = item.argument != "folder";
            if (!entry.stack && stack_.open && stack_.path == entry.path) {
                closeStack();
            }
            modelDirty_ = true;
            rebuildVisibleItems();
            relayoutWindow();
            markConfigDirty(now_);
            break;
        }
        break;
    case MenuAction::SetStackArrangement:
        for (auto &entry : model_.items()) {
            if (entry.id != item.targetId) {
                continue;
            }
            entry.arrangement = std::clamp(std::atoi(item.argument.c_str()), 1, 5);
            if (stack_.open && stack_.path == entry.path) {
                stack_.arrangement = entry.arrangement;
                refreshStackEntries();
            }
            modelDirty_ = true;
            rebuildVisibleItems();
            relayoutWindow();
            markConfigDirty(now_);
            break;
        }
        break;
    case MenuAction::SetStackAuto:
    case MenuAction::SetStackFan:
    case MenuAction::SetStackGrid:
        for (auto &entry : model_.items()) {
            if (entry.id != item.targetId) {
                continue;
            }
            entry.showAs = item.action == MenuAction::SetStackFan
                               ? 1
                               : (item.action == MenuAction::SetStackGrid ? 2 : 0);
            if (stack_.open && stack_.path == entry.path) {
                closeStack();
            }
            modelDirty_ = true;
            rebuildVisibleItems();
            relayoutWindow();
            markConfigDirty(now_);
            break;
        }
        break;
    case MenuAction::None:
        break;
    }
}

void Application::ensureGridResources() {
    if (grid_.window) {
        return;
    }
    grid_.window = SDL_CreateWindow("RevDock Applications", 800, 600,
        static_cast<SDL_WindowFlags>(SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP |
                                     SDL_WINDOW_TRANSPARENT | SDL_WINDOW_UTILITY |
                                     SDL_WINDOW_HIGH_PIXEL_DENSITY));
    if (!grid_.window) {
        RD_LOG_WARN("Grid window creation failed: %s", SDL_GetError());
        return;
    }
    grid_.renderer = SDL_CreateRenderer(grid_.window, nullptr);
    if (!grid_.renderer) {
        SDL_DestroyWindow(grid_.window);
        grid_.window = nullptr;
        return;
    }
    SDL_SetRenderVSync(grid_.renderer, 1);
    SDL_SetRenderDrawBlendMode(grid_.renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderScale(grid_.renderer, contentScale_, contentScale_);
    grid_.draw = std::make_unique<Renderer>(grid_.renderer, contentScale_);
    const std::filesystem::path regular = paths_.assetDir / "fonts" / "Inter" / "Inter-Regular.ttf";
    grid_.text = std::make_unique<TextRenderer>(grid_.renderer, regular.string(), fallbackFontPath());
    grid_.icons = std::make_unique<IconCache>(grid_.renderer, *platform_, *apps_, contentScale_);
    platform_->setTextCommitHandler(
        grid_.window, [this](const std::string &text) {
            handleLaunchpadTextCommit(text);
        });
    for (const auto &[name, target] :
         std::initializer_list<std::pair<const char *, SDL_Texture **>>{
             {"launchpad_search_bg.png", &grid_.searchBackground},
             {"launchpad_search_loupe.png", &grid_.searchLoupe},
             {"launchpad_search_close.png", &grid_.searchClose}}) {
        const std::string path = (paths_.assetDir / "icons" / name).string();
        SDL_Surface *surface = IMG_Load(path.c_str());
        if (!surface) {
            RD_LOG_WARN("Launchpad artwork missing: %s", path.c_str());
            continue;
        }
        *target = SDL_CreateTextureFromSurface(grid_.renderer, surface);
        SDL_DestroySurface(surface);
        if (*target) {
            SDL_SetTextureBlendMode(*target, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(*target, SDL_SCALEMODE_LINEAR);
        }
    }
    SDL_HideWindow(grid_.window);
}

void Application::openGrid() {
    ensureGridResources();
    if (!grid_.window) {
        RD_LOG_WARN("openGrid failed: %s", SDL_GetError());
        return;
    }
    closeStack(true);
    if (apps_->apps().empty()) {
        apps_->refresh(true);
    }
    grid_.columns = settings_.launchpadColumns();
    grid_.rows = settings_.launchpadRows();
    grid_.page = 0;
    grid_.rootPage = 0;
    grid_.hover = -1;
    grid_.blankPressed = false;
    grid_.pagerPressed = false;
    grid_.swiping = false;
    grid_.swipeAnimating = false;
    grid_.swipeOffset = 0.0f;
    grid_.wheelSwiping = false;
    grid_.wheelSwipeOffset = 0.0f;
    grid_.wheelSwipeAt = 0.0;
    grid_.wheelEdgeHoldAt = -1.0;
    grid_.wheelEdgeSuppressed = false;
    grid_.wheelLastAt = -100.0;
    grid_.fingers.clear();
    grid_.fingerSwiping = false;
    grid_.fingerSwipeOffset = 0.0f;
    grid_.pressed = false;
    grid_.dragMoved = false;
    grid_.closing = false;
    grid_.query.clear();
    grid_.composing.clear();
    grid_.searchFocused = false;
    grid_.searchKey.clear();
    grid_.searchResults.clear();
    grid_.pendingText.clear();
    grid_.pendingTextAt = -1.0;
    grid_.folderDrop.active = false;
    grid_.renaming = false;
    grid_.renameText.clear();
    grid_.renameFolderId.clear();
    grid_.folderId.clear();
    grid_.folderClosing = false;
    grid_.folderAnim.setImmediate(0.0f);
    launchpadResetDrag();
    grid_.firstFrame = true;
    const int perPage = std::max(1, grid_.columns * grid_.rows);
    if (!grid_.layoutReady) {
        // First run (no persisted launchpad section): mirror the Start Menu's
        // outermost folders.  UWP apps always stay standalone.
        std::vector<launchpad::InitialApp> entries;
        entries.reserve(apps_->apps().size());
        for (const AppInfo &app : apps_->apps()) {
            entries.push_back(
                {app.id, app.name, app.aumid.empty() ? app.menuGroup : std::string()});
        }
        grid_.layout.buildInitial(entries, perPage);
        grid_.layoutReady = true;
        launchpadMarkDirty();
    } else {
        std::vector<std::string> installed;
        installed.reserve(apps_->apps().size());
        for (const AppInfo &app : apps_->apps()) {
            installed.push_back(app.id);
        }
        const bool changed = grid_.layout.reconcile(installed, perPage);
        if (changed) {
            launchpadMarkDirty();
        }
    }
    {
        int cached = 0;
        int total = 0;
        for (int slot = 0; slot < perPage; ++slot) {
            const LaunchpadCell cell = launchpadCellAt(0, slot);
            if (cell.appId.empty()) {
                break;
            }
            ++total;
            if (grid_.attemptedIcons.count(cell.appId) > 0) {
                ++cached;
            }
        }
        RD_LOG_DEBUG("launchpad open: %d/%d first-page icons cached", cached, total);
    }
    grid_.open = true;
    grid_.openedAt = clock_.now();
    SDL_SetWindowPosition(grid_.window, displayBounds_.x, displayBounds_.y);
    SDL_SetWindowSize(grid_.window, displayBounds_.w, displayBounds_.h);
    SDL_SetWindowFocusable(grid_.window, true);
    gridOpacity_.setImmediate(0.0f);
    SDL_ShowWindow(grid_.window);
    SDL_RaiseWindow(grid_.window);
    platform_->focusWindow(grid_.window);
    // Text input must be enabled on a visible/focused window; enabling it
    // before ShowWindow leaves the IME and WM_CHAR path inert on Windows.
    SDL_StartTextInput(grid_.window);
    if (dockWindow_) {
        // Ownership keeps the Dock above the grid without activation races.
        platform_->setWindowOwner(dockWindow_, grid_.window);
        platform_->raiseAlwaysOnTop(dockWindow_);
    }
    gridOpacity_.setTarget(clock_.now(), 1.0f, 0.32);
    forceRender_ = true;
}

void Application::ensureLaunchpadIcons() {
    if (launchpadIconsLoaded_ || !dockRenderer_) {
        return;
    }
    launchpadIconsLoaded_ = true;
    // Finder and Launchpad use artwork with different transparent margins and
    // slightly different corner sizes. Trim and rescale both into one common
    // box so the two special tiles read as the same size/shape.
    auto normalizeIcon = [&](SDL_Surface *surface) -> SDL_Texture * {
        if (!surface) {
            return nullptr;
        }
        SDL_Surface *rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(surface);
        if (!rgba) {
            return nullptr;
        }
        int minX = rgba->w;
        int minY = rgba->h;
        int maxX = -1;
        int maxY = -1;
        int bodyMinX = rgba->w;
        int bodyMinY = rgba->h;
        int bodyMaxX = -1;
        int bodyMaxY = -1;
        if (SDL_LockSurface(rgba)) {
            const auto *pixels = static_cast<const uint8_t *>(rgba->pixels);
            for (int y = 0; y < rgba->h; ++y) {
                const uint8_t *row = pixels + static_cast<size_t>(y) * rgba->pitch;
                for (int x = 0; x < rgba->w; ++x) {
                    const uint8_t alpha = row[x * 4 + 3];
                    if (alpha > 8) {
                        minX = std::min(minX, x);
                        minY = std::min(minY, y);
                        maxX = std::max(maxX, x);
                        maxY = std::max(maxY, y);
                    }
                    // The solid body excludes the soft outer shadow, so the
                    // visible tiles match the 80.5% app-icon shape exactly.
                    if (alpha > 64) {
                        bodyMinX = std::min(bodyMinX, x);
                        bodyMinY = std::min(bodyMinY, y);
                        bodyMaxX = std::max(bodyMaxX, x);
                        bodyMaxY = std::max(bodyMaxY, y);
                    }
                }
            }
            SDL_UnlockSurface(rgba);
        }
        if (maxX <= minX || maxY <= minY) {
            SDL_DestroySurface(rgba);
            return nullptr;
        }
        if (bodyMaxX <= bodyMinX || bodyMaxY <= bodyMinY) {
            bodyMinX = minX;
            bodyMinY = minY;
            bodyMaxX = maxX;
            bodyMaxY = maxY;
        }
        constexpr int kCanvas = 512;
        constexpr float kContentRatio = 0.805f;
        const int content = static_cast<int>(std::lround(kCanvas * kContentRatio));
        const float bodyW = static_cast<float>(bodyMaxX - bodyMinX + 1);
        const float bodyH = static_cast<float>(bodyMaxY - bodyMinY + 1);
        const float sourceW = static_cast<float>(maxX - minX + 1);
        const float sourceH = static_cast<float>(maxY - minY + 1);
        const float fit = std::min(content / bodyW, content / bodyH);
        const int drawW = std::max(1, static_cast<int>(std::lround(sourceW * fit)));
        const int drawH = std::max(1, static_cast<int>(std::lround(sourceH * fit)));
        SDL_Surface *canvas = SDL_CreateSurface(kCanvas, kCanvas, SDL_PIXELFORMAT_RGBA32);
        if (!canvas) {
            SDL_DestroySurface(rgba);
            return nullptr;
        }
        SDL_FillSurfaceRect(canvas, nullptr, 0);
        const SDL_Rect sourceRect{minX, minY, maxX - minX + 1, maxY - minY + 1};
        const SDL_Rect targetRect{(kCanvas - drawW) / 2, (kCanvas - drawH) / 2, drawW, drawH};
        SDL_BlitSurfaceScaled(rgba, &sourceRect, canvas, &targetRect, SDL_SCALEMODE_LINEAR);
        SDL_DestroySurface(rgba);
        SDL_Texture *texture = SDL_CreateTextureFromSurface(dockRenderer_, canvas);
        SDL_DestroySurface(canvas);
        if (texture) {
            SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
        }
        return texture;
    };
    auto load = [&](const char *name) -> SDL_Texture * {
        const std::string path = (paths_.assetDir / "icons" / name).string();
        return normalizeIcon(loadIcnsSurface(path));
    };
    launchpadDark_ = load("launchpad_dark.icns");
    launchpadLight_ = load("launchpad_light.icns");
    if (!launchpadDark_ && !launchpadLight_) {
        RD_LOG_WARN("Launchpad icons missing under %s", paths_.assetDir.string().c_str());
    }
    for (const auto &[name, target] :
         std::initializer_list<std::pair<const char *, SDL_Texture **>>{
             {"folder_empty.png", &folderEmptyTexture_},
             {"folder_nonempty.png", &folderNonemptyTexture_}}) {
        const std::string folderPath = (paths_.assetDir / "icons" / name).string();
        SDL_Surface *surface = IMG_Load(folderPath.c_str());
        if (!surface) {
            RD_LOG_WARN("Folder artwork missing: %s", folderPath.c_str());
            continue;
        }
        *target = SDL_CreateTextureFromSurface(dockRenderer_, surface);
        SDL_DestroySurface(surface);
        if (*target) {
            SDL_SetTextureBlendMode(*target, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(*target, SDL_SCALEMODE_LINEAR);
        }
    }

    auto loadPng = [&](const char *name, bool whiteTemplate) -> SDL_Texture * {
        const std::string path = (paths_.assetDir / "icons" / name).string();
        SDL_Surface *raw = IMG_Load(path.c_str());
        if (!raw) {
            return nullptr;
        }
        SDL_Surface *surface = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(raw);
        if (!surface) {
            return nullptr;
        }
        if (whiteTemplate) {
            SDL_LockSurface(surface);
            auto *pixels = static_cast<uint8_t *>(surface->pixels);
            for (int y = 0; y < surface->h; ++y) {
                for (int x = 0; x < surface->w; ++x) {
                    uint8_t *pixel = pixels + static_cast<size_t>(y) * surface->pitch + x * 4;
                    if (pixel[3] > 0) {
                        pixel[0] = 255;
                        pixel[1] = 255;
                        pixel[2] = 255;
                    }
                }
            }
            SDL_UnlockSurface(surface);
        }
        SDL_Texture *texture = SDL_CreateTextureFromSurface(dockRenderer_, surface);
        SDL_DestroySurface(surface);
        if (texture) {
            SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
        }
        return texture;
    };
    trashEmptyLight_ = loadPng("trash_empty_light.png", false);
    trashEmptyDark_ = loadPng("trash_empty_dark.png", false);
    trashFullLight_ = loadPng("trash_full_light.png", false);
    trashFullDark_ = loadPng("trash_full_dark.png", false);
    pileArrowBlack_ = loadPng("pile_arrow.png", false);
    pileArrowWhite_ = loadPng("pile_arrow.png", true);
    finderIcon_ = normalizeIcon(IMG_Load((paths_.assetDir / "icons" / "finder.png").string().c_str()));
}

SDL_Texture *Application::buildDockMaterial(int width, int height, float radius, bool dark) {
    if (width <= 0 || height <= 0 || !dockRenderer_) {
        return nullptr;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4, 0);
    const float halfW = width * 0.5f;
    const float halfH = height * 0.5f;
    const float r = std::min(radius, std::min(halfW, halfH));
    const float innerW = std::max(0.0f, halfW - r);
    const float innerH = std::max(0.0f, halfH - r);
    const SDL_Color base = dark ? rgba(24, 24, 26, 255) : rgba(246, 246, 248, 255);
    const SDL_Color edge = dark ? rgba(255, 255, 255, 255) : rgba(0, 0, 0, 255);
    const float baseAlpha = dark ? 0.70f : 0.78f;
    const float borderAlpha = dark ? 0.22f : 0.16f;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float qx = std::abs(x + 0.5f - halfW) - innerW;
            const float qy = std::abs(y + 0.5f - halfH) - innerH;
            const float outside = std::sqrt(std::max(qx, 0.0f) * std::max(qx, 0.0f) +
                                            std::max(qy, 0.0f) * std::max(qy, 0.0f));
            const float distance = outside + std::min(std::max(qx, qy), 0.0f) - r;
            const float coverage = std::clamp(0.5f - distance, 0.0f, 1.0f);
            if (coverage <= 0.0f) {
                continue;
            }
            const float borderMix =
                std::clamp(1.0f - std::max(0.0f, -distance), 0.0f, 1.0f);
            const size_t index = (static_cast<size_t>(y) * width + x) * 4;
            for (int c = 0; c < 3; ++c) {
                const float b = (c == 0 ? base.r : c == 1 ? base.g : base.b) / 255.0f;
                const float e = (c == 0 ? edge.r : c == 1 ? edge.g : edge.b) / 255.0f;
                const float value = b * (1.0f - borderMix * borderAlpha) +
                                    e * borderMix * borderAlpha;
                pixels[index + c] = static_cast<uint8_t>(std::clamp(value * 255.0f, 0.0f, 255.0f));
            }
            const float alpha = baseAlpha + borderMix * (1.0f - baseAlpha);
            pixels[index + 3] = static_cast<uint8_t>(std::clamp(coverage * alpha * 255.0f, 0.0f, 255.0f));
        }
    }
    SDL_Texture *texture = SDL_CreateTexture(dockRenderer_, SDL_PIXELFORMAT_RGBA32,
                                             SDL_TEXTUREACCESS_STATIC, width, height);
    if (!texture) {
        return nullptr;
    }
    SDL_UpdateTexture(texture, nullptr, pixels.data(), width * 4);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    return texture;
}

void Application::ensureGridWallpaper() {
    if (!grid_.window || !grid_.renderer) {
        return;
    }
    const int expectedW = std::max(1, static_cast<int>(std::lround(displayBounds_.w * contentScale_)));
    const int expectedH = std::max(1, static_cast<int>(std::lround(displayBounds_.h * contentScale_)));
    if (grid_.wallpaper) {
        float w = 0.0f;
        float h = 0.0f;
        if (SDL_GetTextureSize(grid_.wallpaper, &w, &h) &&
            static_cast<int>(w) == expectedW && static_cast<int>(h) == expectedH) {
            return;
        }
        SDL_DestroyTexture(grid_.wallpaper);
        grid_.wallpaper = nullptr;
    }
    const std::string path = platform_->wallpaperPath();
    if (path.empty()) {
        return;
    }
    SDL_Surface *source = IMG_Load(path.c_str());
    if (!source) {
        return;
    }
    SDL_Surface *scaled = SDL_CreateSurface(expectedW, expectedH, SDL_PIXELFORMAT_RGBA32);
    if (!scaled) {
        SDL_DestroySurface(source);
        return;
    }
    SDL_BlitSurfaceScaled(source, nullptr, scaled, nullptr, SDL_SCALEMODE_LINEAR);
    SDL_DestroySurface(source);

    std::vector<uint8_t> pixels(static_cast<size_t>(expectedW) * expectedH * 4);
    SDL_LockSurface(scaled);
    for (int y = 0; y < expectedH; ++y) {
        std::memcpy(pixels.data() + static_cast<size_t>(y) * expectedW * 4,
                    static_cast<const uint8_t *>(scaled->pixels) + static_cast<size_t>(y) * scaled->pitch,
                    static_cast<size_t>(expectedW) * 4);
    }
    SDL_UnlockSurface(scaled);
    SDL_DestroySurface(scaled);
    boxBlurRGBA(pixels, expectedW, expectedH,
                std::max(4, static_cast<int>(std::lround(18.0f * contentScale_))), 2);

    grid_.wallpaper = SDL_CreateTexture(grid_.renderer, SDL_PIXELFORMAT_RGBA32,
                                        SDL_TEXTUREACCESS_STATIC, expectedW, expectedH);
    if (grid_.wallpaper) {
        SDL_UpdateTexture(grid_.wallpaper, nullptr, pixels.data(), expectedW * 4);
        SDL_SetTextureBlendMode(grid_.wallpaper, SDL_BLENDMODE_NONE);
    }
}

int Application::launchpadPageCount() const {
    const int perPage = std::max(1, grid_.columns * grid_.rows);
    if (!grid_.query.empty()) {
        const int total = static_cast<int>(grid_.searchResults.size());
        return std::max(1, (total + perPage - 1) / perPage);
    }
    if (!grid_.folderId.empty()) {
        const launchpad::Item *folder = grid_.layout.findFolder(grid_.folderId);
        const int total =
            folder ? static_cast<int>(folder->apps.size()) : 0;
        return std::max(1, (total + perPage - 1) / perPage);
    }
    return std::max(1, grid_.layout.pageCount());
}

int Application::gridPageCount() const { return launchpadPageCount(); }

Application::LaunchpadCell Application::launchpadCellAt(int page, int slot) const {
    LaunchpadCell cell;
    if (page < 0 || slot < 0) {
        return cell;
    }
    const int perPage = std::max(1, grid_.columns * grid_.rows);
    if (!grid_.query.empty()) {
        const int index = page * perPage + slot;
        if (index < 0 || index >= static_cast<int>(grid_.searchResults.size())) {
            return cell;
        }
        cell.appId = grid_.searchResults[index];
        if (const AppInfo *app = apps_->findById(cell.appId)) {
            cell.app = app;
            cell.title = app->name;
        }
        return cell;
    }
    if (!grid_.folderId.empty()) {
        const launchpad::Item *folder = grid_.layout.findFolder(grid_.folderId);
        if (!folder) {
            return cell;
        }
        const int index = page * perPage + slot;
        if (index < 0 || index >= static_cast<int>(folder->apps.size())) {
            return cell;
        }
        cell.appId = folder->apps[index];
        if (const AppInfo *app = apps_->findById(cell.appId)) {
            cell.app = app;
            cell.title = app->name;
        }
        return cell;
    }
    const launchpad::Item *item = grid_.layout.itemAt(page, slot);
    if (!item) {
        return cell;
    }
    cell.item = item;
    if (item->folder) {
        cell.folder = true;
        cell.folderId = item->folderId;
        cell.title = item->title;
        return cell;
    }
    cell.appId = item->appId;
    if (const AppInfo *app = apps_->findById(cell.appId)) {
        cell.app = app;
        cell.title = app->name;
    }
    return cell;
}

const std::vector<std::string> &Application::launchpadSearchResults() {
    if (grid_.searchKey == grid_.query) {
        return grid_.searchResults;
    }
    grid_.searchKey = grid_.query;
    grid_.searchResults.clear();
    if (grid_.query.empty()) {
        return grid_.searchResults;
    }
    struct Hit {
        std::string id;
        std::string name;
        int score = 0;
    };
    std::vector<Hit> hits;
    std::set<std::string> seen;
    for (const AppInfo &app : apps_->apps()) {
        const int score =
            launchpad::Layout::searchScore(app.name, grid_.query);
        if (score < 0 || app.id.empty() || !seen.insert(app.id).second) {
            continue;
        }
        hits.push_back({app.id, app.name, score});
    }
    std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
        if (a.score != b.score) {
            return a.score < b.score;
        }
        const std::string left = lowerAscii(a.name);
        const std::string right = lowerAscii(b.name);
        if (left != right) {
            return left < right;
        }
        return a.id < b.id;
    });
    grid_.searchResults.reserve(hits.size());
    for (const Hit &hit : hits) {
        grid_.searchResults.push_back(hit.id);
    }
    return grid_.searchResults;
}

void Application::computeGridMetrics() {
    const float width = static_cast<float>(displayBounds_.w);
    const float height = static_cast<float>(displayBounds_.h);
    grid_.columns = std::max(1, settings_.launchpadColumns());
    grid_.rows = std::max(1, settings_.launchpadRows());
    const int perPage = std::max(1, grid_.columns * grid_.rows);
    if (!grid_.folderId.empty()) {
        // The expanded folder is a fixed-width, content-height material panel
        // with its title above it.  Width never adapts; height follows the
        // number of rows the folder needs (capped at one page).
        const launchpad::Item *folder = grid_.layout.findFolder(grid_.folderId);
        const int total = folder ? static_cast<int>(folder->apps.size()) : 0;
        const int pages = std::max(1, (total + perPage - 1) / perPage);
        const int rowsUsed =
            std::clamp((total + grid_.columns - 1) / grid_.columns, 1, grid_.rows);
        const float panelWidth = width * 0.86f;
        const float padX = std::min(54.0f, panelWidth * 0.04f);
        const float iconSize = gridIconSize();
        const float cellWidth =
            (panelWidth - padX * 2.0f) / static_cast<float>(grid_.columns);
        const float cellHeight = iconSize + 38.0f;
        const float topPad = 26.0f;
        const float bottomPad = pages > 1 ? 38.0f : 26.0f;
        const float panelHeight =
            topPad + static_cast<float>(rowsUsed) * cellHeight + bottomPad;
        const float titleHeight = 56.0f;
        const float titleGap = 20.0f;
        const float groupHeight = titleHeight + titleGap + panelHeight;
        const float bottomReserve =
            settings_.orientation() == "bottom"
                ? std::max(90.0f, layoutResult_.barThickness + 40.0f)
                : 40.0f;
        const float maxTop = height - bottomReserve - groupHeight;
        float groupTop = (height - groupHeight) * 0.5f;
        groupTop = std::max(40.0f, groupTop);
        if (maxTop > 40.0f) {
            groupTop = std::min(groupTop, maxTop);
        }
        const float panelX = (width - panelWidth) * 0.5f;
        grid_.folderTitleRect =
            SDL_FRect{panelX, groupTop, panelWidth, titleHeight};
        grid_.folderPanel = SDL_FRect{panelX, groupTop + titleHeight + titleGap,
                                      panelWidth, panelHeight};
        grid_.gridRect = SDL_FRect{panelX + padX,
                                   grid_.folderPanel.y + topPad,
                                   panelWidth - padX * 2.0f,
                                   static_cast<float>(rowsUsed) * cellHeight};
        grid_.cellWidth = cellWidth;
        grid_.cellHeight = cellHeight;
        grid_.iconSize = iconSize;
    } else {
        const float margin = width < 900.0f ? 36.0f : 70.0f;
        // The search field (ECTextInputLayer) sits above the grid.  Reserve
        // room for it, for the page dots, and for the Dock: Launchpad keeps
        // the Dock visible above the last row on macOS, so the bottom row must
        // never slide under the bar.
        const float top = 88.0f;
        float bottom = 96.0f;
        if (settings_.orientation() == "bottom") {
            bottom = std::max(132.0f, layoutResult_.barThickness + 84.0f);
        }
        grid_.folderPanel = SDL_FRect{};
        grid_.folderTitleRect = SDL_FRect{};
        grid_.gridRect = SDL_FRect{margin, top, width - margin * 2.0f,
                                   height - top - bottom};
        grid_.cellWidth =
            grid_.gridRect.w / static_cast<float>(grid_.columns);
        grid_.cellHeight =
            grid_.gridRect.h / static_cast<float>(grid_.rows);
        const float cell = std::min(grid_.cellWidth, grid_.cellHeight);
        float iconSize = cell - (14.0f + 11.0f + 22.0f);
        iconSize = std::clamp(iconSize, 20.0f, 112.0f);
        iconSize = std::floor(iconSize / 8.0f) * 8.0f;
        grid_.iconSize = std::max(20.0f, iconSize);
    }
    const float searchWidth = std::min(260.0f, width * 0.28f);
    grid_.searchRect =
        SDL_FRect{(width - searchWidth) * 0.5f, 24.0f, searchWidth, 30.0f};
    grid_.searchCloseRect =
        SDL_FRect{grid_.searchRect.x + grid_.searchRect.w - 23.0f,
                  grid_.searchRect.y + 8.0f, 14.0f, 14.0f};
}

void Application::launchpadMarkDirty() {
    launchpadDirty_ = true;
    markConfigDirty(now_);
}

void Application::handleLaunchpadTextCommit(const std::string &text) {
    if (!grid_.open || text.empty()) {
        return;
    }
    std::string cleaned;
    cleaned.reserve(text.size());
    for (char ch : text) {
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            cleaned.push_back(' ');
        } else if (static_cast<unsigned char>(ch) >= 0x20) {
            cleaned.push_back(ch);
        }
    }
    if (cleaned.empty()) {
        return;
    }
    if (now_ - grid_.lastNativeAt < 0.25 &&
        cleaned == grid_.lastNativeText) {
        return;
    }
    if (grid_.renaming) {
        if (grid_.renameFresh) {
            grid_.renameText.clear();
            grid_.renameFresh = false;
        }
        grid_.renameText += cleaned;
        grid_.composing.clear();
        grid_.searchCaretAt = now_;
    } else if (grid_.folderId.empty()) {
        if (grid_.query.empty()) {
            grid_.rootPage = grid_.page;
        }
        grid_.query += cleaned;
        grid_.composing.clear();
        grid_.searchKey.clear();
        grid_.searchFocused = true;
        grid_.searchCaretAt = now_;
        grid_.page = 0;
        grid_.blankPressed = false;
        grid_.swiping = false;
    } else {
        return;
    }
    grid_.pendingText.clear();
    grid_.pendingTextAt = -1.0;
    grid_.lastNativeText = cleaned;
    grid_.lastNativeAt = now_;
    grid_.nativeCommitUntil = now_ + 0.35;
    dirty_ = true;
}

bool Application::launchpadOpenFolder(const std::string &folderId,
                                      bool keepDrag) {
    if (folderId.empty() || !grid_.layout.findFolder(folderId)) {
        return false;
    }
    if (grid_.folderId.empty()) {
        grid_.rootPage = grid_.page;
    }
    // Capture the folder tile's rounded body before switching to the expanded
    // layout; the panel animates from this rectangle (item 5).
    grid_.folderAnimFrom = SDL_FRect{};
    if (grid_.folderId.empty()) {
        computeGridMetrics();
        const int page = grid_.page;
        if (page >= 0 && page < grid_.layout.pageCount()) {
            const auto &items = grid_.layout.pages[page].items;
            for (int i = 0; i < static_cast<int>(items.size()); ++i) {
                if (!items[i].folder || items[i].folderId != folderId) {
                    continue;
                }
                const int col = i % grid_.columns;
                const int row = i / grid_.columns;
                const float centerX =
                    grid_.gridRect.x +
                    (static_cast<float>(col) + 0.5f) * grid_.cellWidth;
                const float centerY =
                    grid_.gridRect.y +
                    (static_cast<float>(row) + 0.5f) * grid_.cellHeight;
                const SDL_FRect icon{centerX - grid_.iconSize * 0.5f,
                                     centerY - grid_.iconSize * 0.5f - 8.0f,
                                     grid_.iconSize, grid_.iconSize};
                grid_.folderAnimFrom = insetRect(
                    icon, icon.w * 0.0975f, icon.h * 0.0975f);
                break;
            }
        }
    }
    if (!grid_.query.empty()) {
        grid_.query.clear();
        grid_.composing.clear();
        grid_.searchKey.clear();
        grid_.searchResults.clear();
    }
    launchpadCommitRename(false);
    grid_.folderId = folderId;
    grid_.folderClosing = false;
    grid_.page = 0;
    grid_.hover = -1;
    grid_.renaming = false;
    grid_.pendingText.clear();
    grid_.pendingTextAt = -1.0;
    grid_.folderAnim.setImmediate(0.0f);
    grid_.folderAnim.setTarget(clock_.now(), 1.0f, 0.22);
    grid_.cellAnims.clear();
    grid_.returnFrom.clear();
    grid_.folderAwayAt = -1.0;
    if (!keepDrag) {
        launchpadResetDrag();
    } else {
        grid_.dragTarget = GridState::DragTarget::None;
        grid_.dragTargetPage = -1;
        grid_.dragTargetSlot = -1;
        grid_.dragTargetAppId.clear();
        grid_.dragTargetFolderId.clear();
        grid_.dragHoverFolderId.clear();
        grid_.dragHoverAt = -1.0;
        grid_.dragTargetAt = -1.0;
    }
    dirty_ = true;
    forceRender_ = true;
    return true;
}

void Application::launchpadCloseFolder(bool immediate, bool keepDrag) {
    if (grid_.folderId.empty()) {
        return;
    }
    launchpadCommitRename(false);
    grid_.pendingText.clear();
    grid_.pendingTextAt = -1.0;
    if (!keepDrag) {
        launchpadResetDrag();
    } else {
        grid_.dragTarget = GridState::DragTarget::None;
        grid_.dragTargetPage = -1;
        grid_.dragTargetSlot = -1;
        grid_.dragTargetAppId.clear();
        grid_.dragTargetFolderId.clear();
    }
    grid_.cellAnims.clear();
    grid_.returnFrom.clear();
    grid_.folderAwayAt = -1.0;
    if (immediate) {
        grid_.folderId.clear();
        grid_.folderClosing = false;
        grid_.folderAnim.setImmediate(0.0f);
        grid_.page = grid_.rootPage;
    } else {
        grid_.folderClosing = true;
        grid_.folderAnim.setTarget(clock_.now(), 0.0f, 0.18);
    }
    dirty_ = true;
    forceRender_ = true;
}

void Application::launchpadBeginRename(bool fresh) {
    const launchpad::Item *folder = grid_.layout.findFolder(grid_.folderId);
    if (!folder) {
        return;
    }
    grid_.renaming = true;
    grid_.renameFresh = fresh;
    grid_.renameText = folder->title;
    grid_.renameFolderId = grid_.folderId;
    grid_.searchFocused = false;
    grid_.composing.clear();
    grid_.pendingText.clear();
    grid_.pendingTextAt = -1.0;
    SDL_StartTextInput(grid_.window);
    dirty_ = true;
    forceRender_ = true;
}

void Application::launchpadCommitRename(bool cancel) {
    if (!grid_.renaming) {
        return;
    }
    const std::string id = grid_.renameFolderId;
    std::string text = grid_.renameText;
    grid_.renaming = false;
    grid_.renameFresh = false;
    grid_.renameFolderId.clear();
    grid_.renameText.clear();
    grid_.composing.clear();
    grid_.pendingText.clear();
    grid_.pendingTextAt = -1.0;
    if (!cancel && !id.empty()) {
        const size_t first = text.find_first_not_of(" \t\r\n");
        const size_t last = text.find_last_not_of(" \t\r\n");
        text = first == std::string::npos ? std::string()
                                          : text.substr(first, last - first + 1);
        if (text.empty()) {
            text = "Untitled Folder";
        }
        const launchpad::Item *folder = grid_.layout.findFolder(id);
        if (folder && folder->title != text) {
            grid_.layout.renameFolder(id, text);
            launchpadMarkDirty();
        }
    }
    dirty_ = true;
}

void Application::launchpadResetDrag() {
    grid_.dragPage = -1;
    grid_.dragSlot = -1;
    grid_.dragAppId.clear();
    grid_.dragIsFolder = false;
    grid_.dragTarget = GridState::DragTarget::None;
    grid_.dragTargetPage = -1;
    grid_.dragTargetSlot = -1;
    grid_.dragTargetAppId.clear();
    grid_.dragTargetFolderId.clear();
    grid_.dragHoverFolderId.clear();
    grid_.dragOriginFolderId.clear();
    grid_.dragHoverAt = -1.0;
    grid_.dragTargetAt = -1.0;
    grid_.dragEdgeAt = -1.0;
    grid_.dragEdgeDirection = 0;
    grid_.folderAwayAt = -1.0;
}

namespace {
std::string newLaunchpadFolderId(launchpad::Layout &layout, double now) {
    const long long stamp =
        static_cast<long long>((now > 0.0 ? now : 1.0) * 1000.0);
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const std::string id =
            "folder-" + std::to_string(stamp) + "-" + std::to_string(attempt);
        if (!layout.findFolder(id)) {
            return id;
        }
    }
    return "folder-" + std::to_string(stamp);
}
} // namespace

void Application::launchpadUpdateDragTarget(float x, float y) {
    if (!grid_.pressed || !grid_.dragMoved || !grid_.query.empty() ||
        grid_.folderClosing) {
        return;
    }
    const float width = static_cast<float>(displayBounds_.w);
    const int perPage = std::max(1, grid_.columns * grid_.rows);
    const int page = grid_.page;
    const int pages = gridPageCount();

    // Dragging to the screen edge flips pages after a short dwell, matching
    // ECSBChangePageDragState.
    int edgeDirection = 0;
    if (x < 24.0f && page > 0) {
        edgeDirection = -1;
    } else if (x > width - 24.0f && page + 1 < pages) {
        edgeDirection = 1;
    }
    if (edgeDirection == 0) {
        grid_.dragEdgeAt = -1.0;
        grid_.dragEdgeDirection = 0;
    } else if (edgeDirection != grid_.dragEdgeDirection) {
        grid_.dragEdgeDirection = edgeDirection;
        grid_.dragEdgeAt = now_;
    } else if (grid_.dragEdgeAt > 0.0 && now_ - grid_.dragEdgeAt >= 0.55) {
        switchGridPage(edgeDirection);
        grid_.dragEdgeAt = now_;
        grid_.dragTarget = GridState::DragTarget::None;
        return;
    }

    const bool insideGrid =
        x >= grid_.gridRect.x - grid_.cellWidth * 0.35f &&
        x <= grid_.gridRect.x + grid_.gridRect.w + grid_.cellWidth * 0.35f &&
        y >= grid_.gridRect.y - grid_.cellHeight * 0.35f &&
        y <= grid_.gridRect.y + grid_.gridRect.h + grid_.cellHeight * 0.35f;
    if (!insideGrid) {
        // Keep the source slot open while the pointer is elsewhere; the gap is
        // part of the drag feedback and closes only on drop/cancel.
        if (grid_.dragTarget != GridState::DragTarget::Gap ||
            grid_.dragTargetPage != grid_.dragPage ||
            grid_.dragTargetSlot != grid_.dragSlot) {
            grid_.dragTarget = GridState::DragTarget::Gap;
            grid_.dragTargetPage = grid_.dragPage;
            grid_.dragTargetSlot = grid_.dragSlot;
            grid_.dragTargetAppId.clear();
            grid_.dragTargetFolderId.clear();
            dirty_ = true;
        }
        grid_.dragHoverFolderId.clear();
        grid_.dragHoverAt = -1.0;
        return;
    }

    int count = 0;
    if (!grid_.folderId.empty()) {
        if (const launchpad::Item *folder =
                grid_.layout.findFolder(grid_.folderId)) {
            count = static_cast<int>(folder->apps.size()) - page * perPage;
        }
    } else {
        if (page >= 0 && page < grid_.layout.pageCount()) {
            count = static_cast<int>(grid_.layout.pages[page].items.size());
        }
    }
    count = std::max(0, std::min(count, perPage));

    int col = static_cast<int>(
        std::floor((x - grid_.gridRect.x) / std::max(1.0f, grid_.cellWidth)));
    int row = static_cast<int>(
        std::floor((y - grid_.gridRect.y) / std::max(1.0f, grid_.cellHeight)));
    col = std::clamp(col, 0, grid_.columns - 1);
    row = std::clamp(row, 0, grid_.rows - 1);
    const int slot = row * grid_.columns + col;
    const float cellCenterX = grid_.gridRect.x +
                              (static_cast<float>(col) + 0.5f) * grid_.cellWidth;
    const float cellCenterY = grid_.gridRect.y +
                              (static_cast<float>(row) + 0.5f) * grid_.cellHeight;
    const SDL_FRect iconRect{
        cellCenterX - grid_.iconSize * 0.5f,
        cellCenterY - grid_.iconSize * 0.5f - 8.0f, grid_.iconSize,
        grid_.iconSize};

    GridState::DragTarget target = GridState::DragTarget::None;
    int targetSlot = -1;
    std::string targetApp;
    std::string targetFolder;

    if (slot < count && contains(iconRect, x, y)) {
        const LaunchpadCell cell = launchpadCellAt(page, slot);
        // macOS only reorders around a tile after the pointer crosses roughly
        // three quarters of the icon in the drag direction.  Inside the middle
        // half the tile simply dims (app/folder target) or does not react at
        // all (folder drag / in-folder reorder).
        const float rel = (x - iconRect.x) / std::max(1.0f, iconRect.w);
        const bool beforeIcon = rel < 0.25f;
        const bool afterIcon = rel > 0.75f;
        if (beforeIcon || afterIcon) {
            target = GridState::DragTarget::Gap;
            targetSlot = slot + (afterIcon ? 1 : 0);
        } else if (grid_.folderId.empty() && !grid_.dragIsFolder &&
                   cell.folder && !cell.folderId.empty()) {
            target = GridState::DragTarget::Folder;
            targetFolder = cell.folderId;
            targetSlot = slot;
        } else if (grid_.folderId.empty() && !grid_.dragIsFolder &&
                   !cell.appId.empty() && cell.appId != grid_.dragAppId) {
            target = GridState::DragTarget::App;
            targetApp = cell.appId;
            targetSlot = slot;
        } else if (cell.appId == grid_.dragAppId) {
            target = GridState::DragTarget::Gap;
            targetSlot = slot;
        } else {
            // Keep the previous gap while crossing the middle of a tile.
            target = grid_.dragTarget;
            targetSlot = grid_.dragTargetSlot;
            if (target == GridState::DragTarget::None) {
                target = GridState::DragTarget::Gap;
                targetSlot = slot;
            }
            if (target != GridState::DragTarget::Gap) {
                target = GridState::DragTarget::None;
            }
        }
    } else {
        target = GridState::DragTarget::Gap;
        targetSlot = slot;
        if (slot < count && x > cellCenterX) {
            ++targetSlot;
        }
        targetSlot = std::clamp(targetSlot, 0, count);
    }

    // Hovering a folder dims it twice, then opens it while the drag continues
    // (ECSBOpenGroupDragState).  Releasing before that drops straight into the
    // folder.
    if (target == GridState::DragTarget::Folder) {
        if (targetFolder != grid_.dragHoverFolderId) {
            grid_.dragHoverFolderId = targetFolder;
            grid_.dragHoverAt = now_;
        } else if (grid_.dragHoverAt > 0.0 &&
                   now_ - grid_.dragHoverAt >= 0.70) {
            grid_.dragHoverFolderId.clear();
            grid_.dragHoverAt = -1.0;
            launchpadOpenFolder(targetFolder, true);
            dirty_ = true;
            return;
        }
    } else {
        grid_.dragHoverFolderId.clear();
        grid_.dragHoverAt = -1.0;
    }

    if (grid_.dragTarget != target ||
        grid_.dragTargetPage != page ||
        grid_.dragTargetSlot != targetSlot ||
        grid_.dragTargetAppId != targetApp ||
        grid_.dragTargetFolderId != targetFolder) {
        grid_.dragTarget = target;
        grid_.dragTargetPage = page;
        grid_.dragTargetSlot = targetSlot;
        grid_.dragTargetAppId = std::move(targetApp);
        grid_.dragTargetFolderId = std::move(targetFolder);
        grid_.dragTargetAt = now_;
        dirty_ = true;
    }
}

void Application::launchpadFinishDrag(float localX, float localY,
                                      bool overDock) {
    if (!grid_.pressed || !grid_.dragMoved) {
        launchpadResetDrag();
        return;
    }
    const int perPage = std::max(1, grid_.columns * grid_.rows);
    const std::string appId = grid_.dragAppId;
    const bool draggedFolder = grid_.dragIsFolder;
    const int fromPage = grid_.dragPage;
    const int fromSlot = grid_.dragSlot;
    const auto target = grid_.dragTarget;
    const int targetPage = grid_.dragTargetPage;
    const int targetSlot = grid_.dragTargetSlot;
    const std::string targetApp = grid_.dragTargetAppId;
    const std::string targetFolder = grid_.dragTargetFolderId;
    const std::string openFolderId = grid_.folderId;
    const std::string originFolder = grid_.dragOriginFolderId;
    bool changed = false;
    bool wantReturn = false;
    bool closeBeforeReturn = false;

    if (!overDock && grid_.query.empty()) {
        if (!openFolderId.empty()) {
            const bool insidePanel =
                localX >= grid_.folderPanel.x &&
                localX <= grid_.folderPanel.x + grid_.folderPanel.w &&
                localY >= grid_.folderPanel.y &&
                localY <= grid_.folderPanel.y + grid_.folderPanel.h;
            if (target == GridState::DragTarget::Gap && targetPage >= 0 &&
                insidePanel) {
                const launchpad::Item *folder =
                    grid_.layout.findFolder(openFolderId);
                if (folder) {
                    const auto found = std::find(folder->apps.begin(),
                                                 folder->apps.end(), appId);
                    int insert = targetPage * perPage + targetSlot;
                    if (found != folder->apps.end()) {
                        int fromIndex =
                            static_cast<int>(found - folder->apps.begin());
                        if (fromIndex < insert) {
                            --insert;
                        }
                        insert = std::clamp(
                            insert, 0, static_cast<int>(folder->apps.size()));
                        if (!(targetPage == fromPage &&
                              (insert == fromIndex ||
                               insert == fromIndex + 1))) {
                            changed = grid_.layout.moveFolderApp(
                                openFolderId, fromIndex, insert);
                        }
                    } else if (!appId.empty()) {
                        // A folder was auto-opened while dragging; dropping
                        // inside it moves the root application into it.
                        changed = grid_.layout.moveAppIntoFolderAt(
                            appId, openFolderId, insert, perPage);
                    }
                }
            } else {
                // Released outside the folder rectangle: the dragged icon
                // returns to where it came from.  If the folder was auto-
                // opened for a root drag, close it first so the return flight
                // is visible on the root page.
                wantReturn = true;
                closeBeforeReturn = !originFolder.empty() ? false : true;
                if (originFolder == openFolderId) {
                    closeBeforeReturn = false;
                }
            }
        } else if (!originFolder.empty()) {
            // The folder was auto-collapsed after the pointer waited outside;
            // a release on the page now moves the application out of it.
            if (target == GridState::DragTarget::Gap && targetPage >= 0) {
                changed = grid_.layout.moveAppOutOfFolder(originFolder, appId,
                                                          perPage);
            } else if (target == GridState::DragTarget::Folder &&
                       !targetFolder.empty()) {
                changed = grid_.layout.moveAppIntoFolder(appId, targetFolder,
                                                         perPage);
            } else if (target == GridState::DragTarget::App &&
                       !targetApp.empty() && !draggedFolder) {
                const std::string folderId =
                    newLaunchpadFolderId(grid_.layout, now_);
                changed = grid_.layout.makeFolderFromApps(
                    appId, targetApp, folderId, "Untitled Folder", perPage);
            } else {
                wantReturn = false;
            }
        } else if (target == GridState::DragTarget::App &&
                   !targetApp.empty() && !draggedFolder) {
            const std::string folderId =
                newLaunchpadFolderId(grid_.layout, now_);
            changed = grid_.layout.makeFolderFromApps(
                appId, targetApp, folderId, "Untitled Folder", perPage);
            if (changed) {
                launchpadOpenFolder(folderId);
                launchpadBeginRename(true);
            }
        } else if (target == GridState::DragTarget::Folder &&
                   !targetFolder.empty() && !draggedFolder) {
            const float iconSize = grid_.iconSize;
            const SDL_FRect from{grid_.dragX - iconSize * 0.5f,
                                 grid_.dragY - iconSize * 0.5f - 8.0f,
                                 iconSize, iconSize};
            changed = grid_.layout.moveAppIntoFolder(appId, targetFolder,
                                                     perPage);
            if (changed) {
                // Shrink the released app into the folder tile instead of
                // making it vanish.
                for (int p = 0; p < grid_.layout.pageCount(); ++p) {
                    const auto &items = grid_.layout.pages[p].items;
                    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
                        if (!items[i].folder ||
                            items[i].folderId != targetFolder) {
                            continue;
                        }
                        const int col = i % grid_.columns;
                        const int row = i / grid_.columns;
                        const float centerX =
                            grid_.gridRect.x +
                            (static_cast<float>(col) + 0.5f) * grid_.cellWidth;
                        const float centerY =
                            grid_.gridRect.y +
                            (static_cast<float>(row) + 0.5f) * grid_.cellHeight;
                        const SDL_FRect icon{
                            centerX - iconSize * 0.5f,
                            centerY - iconSize * 0.5f - 8.0f, iconSize,
                            iconSize};
                        const SDL_FRect body =
                            insetRect(icon, icon.w * 0.0975f,
                                      icon.h * 0.0975f);
                        grid_.folderDrop.active = true;
                        grid_.folderDrop.appId = appId;
                        grid_.folderDrop.from = from;
                        grid_.folderDrop.to =
                            insetRect(body, body.w * 0.16f, body.h * 0.16f);
                        grid_.folderDrop.start = now_;
                        grid_.folderDrop.duration = 0.24;
                        break;
                    }
                    if (grid_.folderDrop.active) {
                        break;
                    }
                }
            }
        } else if (target == GridState::DragTarget::Gap) {
            const bool sameSlot =
                targetPage == fromPage &&
                (targetSlot == fromSlot || targetSlot == fromSlot + 1);
            if (!sameSlot) {
                changed = grid_.layout.moveItem(fromPage, fromSlot, targetPage,
                                                targetSlot, perPage);
            }
        }
    }

    if (!changed && !wantReturn && grid_.query.empty()) {
        // Blank release outside any target is a cancel, not a silent snap.
        wantReturn = true;
    }

    if (changed) {
        launchpadMarkDirty();
        const int pages = gridPageCount();
        grid_.page = std::clamp(grid_.page, 0, pages - 1);
        if (grid_.folderId.empty()) {
            grid_.rootPage = grid_.page;
        }
    }

    if (wantReturn) {
        std::string key;
        if (draggedFolder) {
            const LaunchpadCell cell = launchpadCellAt(fromPage, fromSlot);
            if (cell.folder) {
                key = "f:" + cell.folderId;
            }
        } else if (!appId.empty()) {
            key = "a:" + appId;
        }
        if (closeBeforeReturn) {
            launchpadCloseFolder(false);
        } else {
            launchpadResetDrag();
        }
        if (!key.empty()) {
            grid_.returnFrom[key] = SDL_FPoint{localX, localY};
            grid_.cellAnims.erase(key);
        }
    } else {
        launchpadResetDrag();
    }
    grid_.pressed = false;
    grid_.dragMoved = false;
    gridDragOverDock_ = false;
    clearDragPreview();
    SDL_CaptureMouse(false);
    dirty_ = true;
    forceRender_ = true;
}

void Application::drawLaunchpadSearchField(SDL_Color labelColor) {
    if (!grid_.draw || !grid_.text || grid_.folderId.size() > 0) {
        return;
    }
    const SDL_FRect field = grid_.searchRect;
    const float opacity = grid_.draw->opacity();
    auto drawNineSlice = [&](SDL_Texture *texture, const SDL_FRect &dst) {
        if (!texture) {
            return;
        }
        float tw = 0.0f;
        float th = 0.0f;
        SDL_GetTextureSize(texture, &tw, &th);
        if (tw <= 0.0f || th <= 0.0f) {
            return;
        }
        const float cap = std::min(10.0f, tw * 0.45f);
        const float scale = dst.h / th;
        const float capWidth = cap * scale;
        const SDL_FRect srcLeft{0.0f, 0.0f, cap, th};
        const SDL_FRect srcCenter{cap, 0.0f, tw - cap * 2.0f, th};
        const SDL_FRect srcRight{tw - cap, 0.0f, cap, th};
        const SDL_FRect dstLeft{dst.x, dst.y, capWidth, dst.h};
        const SDL_FRect dstCenter{dst.x + capWidth, dst.y,
                                  dst.w - capWidth * 2.0f, dst.h};
        const SDL_FRect dstRight{dst.x + dst.w - capWidth, dst.y, capWidth,
                                 dst.h};
        SDL_SetTextureAlphaMod(
            texture, static_cast<uint8_t>(std::clamp(255.0f * opacity, 0.0f,
                                                     255.0f)));
        SDL_RenderTexture(grid_.renderer, texture, &srcLeft, &dstLeft);
        SDL_RenderTexture(grid_.renderer, texture, &srcCenter, &dstCenter);
        SDL_RenderTexture(grid_.renderer, texture, &srcRight, &dstRight);
    };
    if (grid_.searchBackground) {
        drawNineSlice(grid_.searchBackground, field);
    } else {
        grid_.draw->roundedRect(field, field.h * 0.5f, rgba(30, 30, 36, 210));
    }

    if (grid_.searchLoupe) {
        grid_.draw->texture(
            grid_.searchLoupe,
            SDL_FRect{field.x + 12.0f, field.y + (field.h - 14.0f) * 0.5f,
                      14.0f, 14.0f},
            rgba(235, 235, 240, 235));
    }
    const float textX = field.x + 34.0f;
    const float textWidth = field.w - 34.0f -
                            (grid_.query.empty() ? 14.0f : 38.0f);
    std::string visible = grid_.query;
    if (!grid_.composing.empty()) {
        visible += grid_.composing;
    }
    if (visible.empty()) {
        grid_.text->drawTextEllipsizedLeft(
            "Search", SDL_FRect{textX, field.y, textWidth, field.h},
            13.0f, rgba(labelColor.r, labelColor.g, labelColor.b, 120));
    } else {
        grid_.text->drawTextEllipsizedLeft(
            visible, SDL_FRect{textX, field.y, textWidth, field.h}, 13.0f,
            labelColor);
    }
    if (grid_.searchFocused) {
        const bool caretOn =
            std::fmod(now_ - grid_.searchCaretAt, 1.0) < 0.55;
        if (caretOn) {
            const float measured =
                std::min(textWidth,
                         grid_.text->measure(visible, 13.0f).x);
            grid_.draw->fillRect(
                SDL_FRect{textX + measured + 1.0f,
                          field.y + (field.h - 16.0f) * 0.5f, 1.5f, 16.0f},
                rgba(labelColor.r, labelColor.g, labelColor.b, 220));
        }
    }
    if (!grid_.query.empty() && grid_.searchClose) {
        grid_.draw->texture(grid_.searchClose, grid_.searchCloseRect,
                            rgba(255, 255, 255, 225));
    }
    SDL_Rect inputArea{static_cast<int>(std::lround(field.x)),
                       static_cast<int>(std::lround(field.y)),
                       static_cast<int>(std::lround(field.w)),
                       static_cast<int>(std::lround(field.h))};
    SDL_SetTextInputArea(grid_.window, &inputArea, 0);
}

void Application::drawLaunchpadFolderTile(const launchpad::Item &folder,
                                          const SDL_FRect &icon,
                                          int iconPx) {
    const bool folderTarget =
        grid_.dragTarget == GridState::DragTarget::Folder &&
        grid_.dragTargetFolderId == folder.folderId;
    float dim = 1.0f;
    if (folderTarget && grid_.dragHoverAt > 0.0) {
        const double elapsed = now_ - grid_.dragHoverAt;
        const bool firstPulse = elapsed >= 0.18 && elapsed < 0.30;
        const bool secondPulse = elapsed >= 0.42 && elapsed < 0.54;
        if (firstPulse || secondPulse) {
            dim = 0.45f;
        }
    }
    const float baseOpacity = grid_.draw->opacity();
    if (dim < 1.0f) {
        grid_.draw->setOpacity(baseOpacity * dim);
    }
    // Match the application icon's visible squircle exactly: the icon mask is
    // 80.5% of the 112 px tile with a 22.37% corner radius.  The folder's light
    // frosted base uses the same body rect and radius (no border).
    const SDL_FRect body =
        insetRect(icon, icon.w * 0.0975f, icon.h * 0.0975f);
    const float radius = body.w * 0.2237f;
    // A plain light shape identical to the app-icon squircle, with the same
    // baked soft shadow.  Transparency matches the expanded folder body (96);
    // no blur capture and no border.
    if (SDL_Texture *plate = grid_.icons->shapeTexture(
            static_cast<int>(icon.w), rgba(238, 238, 244, 96))) {
        grid_.draw->texture(plate, icon);
    } else {
        grid_.draw->roundedRect(body, radius, rgba(238, 238, 244, 96));
    }
    const float mini = body.w * 0.20f;
    const float centers[3] = {0.27f, 0.50f, 0.73f};
    for (int i = 0; i < 9 && i < static_cast<int>(folder.apps.size()); ++i) {
        const AppInfo *app = apps_->findById(folder.apps[i]);
        if (!app) {
            continue;
        }
        const int col = i % 3;
        const int row = i / 3;
        const float cx = body.x + body.w * centers[col];
        const float cy = body.y + body.h * centers[row];
        const SDL_FRect miniRect{cx - mini * 0.5f, cy - mini * 0.5f, mini,
                                 mini};
        if (SDL_Texture *texture =
                grid_.icons->textureForApp(*app, std::max(8, iconPx / 5))) {
            grid_.draw->texture(texture, miniRect);
        } else {
            grid_.draw->placeholderIcon(miniRect, rgba(70, 72, 84, 245),
                                        rgba(110, 114, 132, 245));
        }
    }
    if (dim < 1.0f) {
        grid_.draw->setOpacity(baseOpacity);
    }
}

void Application::drawLaunchpadFolderPanel() {
    if (grid_.folderId.empty()) {
        return;
    }
    const float anim = grid_.folderAnim.value();
    const SDL_FRect panel = grid_.folderPanel;
    SDL_FRect scaled = panel;
    float radius = 26.0f;
    if (grid_.folderAnimFrom.w > 0.0f) {
        // Expand from the folder tile's rounded body towards the panel.
        const SDL_FRect &from = grid_.folderAnimFrom;
        scaled = SDL_FRect{lerpf(from.x, panel.x, anim),
                           lerpf(from.y, panel.y, anim),
                           lerpf(from.w, panel.w, anim),
                           lerpf(from.h, panel.h, anim)};
        radius = lerpf(from.w * 0.2237f, 26.0f, anim);
    } else {
        const float scale = 0.92f + 0.08f * anim;
        scaled = SDL_FRect{panel.x + (panel.w - panel.w * scale) * 0.5f,
                           panel.y + (panel.h - panel.h * scale) * 0.5f,
                           panel.w * scale, panel.h * scale};
    }
    const float cx = scaled.x + scaled.w * 0.5f;
    const float opacity = grid_.draw->opacity();
    grid_.draw->setOpacity(opacity);
    // The panel begins as the folder tile itself (same rect, radius, opaque
    // light fill and faint shadow) and turns into the translucent expanded
    // base while it grows.  The finished panel has no border or shadow.
    const uint8_t fillAlpha = 96;
    const uint8_t shadowAlpha =
        static_cast<uint8_t>(std::lround(120.0f * (1.0f - anim)));
    if (shadowAlpha > 0) {
        grid_.draw->shadow(scaled, radius, rgba(0, 0, 0, shadowAlpha), 3.0f);
    }
    if (grid_.folderAnim.active() || grid_.folderClosing) {
        grid_.draw->roundedRect(scaled, radius,
                                rgba(238, 238, 244, fillAlpha));
    } else {
        // Settled panel: bake it at device resolution so the right/bottom
        // edges are a single clean anti-aliased arc with no slice seams.
        grid_.draw->roundedRectExact(scaled, radius,
                                     rgba(238, 238, 244, fillAlpha));
    }

    const std::string title = [&]() {
        const launchpad::Item *folder =
            grid_.layout.findFolder(grid_.folderId);
        return folder ? folder->title : std::string();
    }();
    const float titleOpacity =
        gridOpacity_.value() * clampf((anim - 0.5f) / 0.5f, 0.0f, 1.0f);
    grid_.text->setOpacity(titleOpacity);
    if (grid_.renaming) {
        const float fontSize = 44.0f;
        std::string display = grid_.renameText;
        if (!grid_.composing.empty()) {
            display += grid_.composing;
        }
        const float measured = grid_.text->measure(display, fontSize).x;
        const float boxWidth = std::clamp(measured + 48.0f, 220.0f, 820.0f);
        const SDL_FRect box{cx - boxWidth * 0.5f, grid_.folderTitleRect.y,
                            boxWidth, grid_.folderTitleRect.h};
        grid_.draw->roundedRect(box, 12.0f, rgba(18, 18, 22, 205));
        grid_.text->drawTextEllipsizedLeft(
            display, SDL_FRect{box.x + 18.0f, box.y, box.w - 30.0f, box.h},
            fontSize, rgba(248, 248, 252, 245));
        const bool caretOn =
            std::fmod(now_ - grid_.searchCaretAt, 1.0) < 0.55;
        if (caretOn) {
            const float caretX =
                box.x + 18.0f +
                std::min(box.w - 36.0f,
                         grid_.text->measure(display, fontSize).x);
            grid_.draw->fillRect(
                SDL_FRect{caretX + 1.0f, box.y + 14.0f, 2.0f, 30.0f},
                rgba(248, 248, 252, 230));
        }
        SDL_Rect inputArea{static_cast<int>(std::lround(box.x)),
                           static_cast<int>(std::lround(box.y)),
                           static_cast<int>(std::lround(box.w)),
                           static_cast<int>(std::lround(box.h))};
        SDL_SetTextInputArea(grid_.window, &inputArea, 0);
    } else if (!title.empty()) {
        SDL_FRect titleRect = grid_.folderTitleRect;
        titleRect.x += 30.0f;
        titleRect.w -= 60.0f;
        grid_.text->drawTextEllipsized(
            title, SDL_FRect{titleRect.x + 1.0f, titleRect.y + 1.0f,
                             titleRect.w, titleRect.h},
            44.0f, rgba(0, 0, 0, 120));
        grid_.text->drawTextEllipsized(title, titleRect, 44.0f,
                                       rgba(250, 250, 253, 245));
    }
    const int perPage = std::max(1, grid_.columns * grid_.rows);
    const launchpad::Item *folder =
        grid_.layout.findFolder(grid_.folderId);
    const int total = folder ? static_cast<int>(folder->apps.size()) : 0;
    const int pages = std::max(1, (total + perPage - 1) / perPage);
    if (pages > 1) {
        const float spacing = 16.0f;
        const float activeSize = 9.0f;
        const float inactiveSize = 7.0f;
        const float dotY = scaled.y + scaled.h - 26.0f;
        const float startX =
            cx - static_cast<float>(pages) * spacing * 0.5f + spacing * 0.5f;
        grid_.dotSpacing = spacing;
        grid_.dotActiveSize = activeSize;
        grid_.dotInactiveSize = inactiveSize;
        grid_.dotY = dotY;
        grid_.dotStartX = startX;
        for (int i = 0; i < pages; ++i) {
            const bool active = i == grid_.page;
            const float size = active ? activeSize : inactiveSize;
            grid_.draw->circle(
                SDL_FRect{startX + static_cast<float>(i) * spacing -
                              size * 0.5f,
                          dotY + (activeSize - size) * 0.5f, size, size},
                active ? rgba(32, 32, 36, 225)
                       : rgba(32, 32, 36, 90));
        }
    }
    grid_.text->setOpacity(gridOpacity_.value());
    grid_.draw->setOpacity(opacity);
}

void Application::switchGridPage(int direction) {
    switchGridPageTo(grid_.page + direction);
}

void Application::switchGridPageTo(int targetPage) {
    const int pages = gridPageCount();
    const int target = std::clamp(targetPage, 0, pages - 1);
    if (target == grid_.page) {
        return;
    }
    const int direction = target > grid_.page ? 1 : -1;
    grid_.page = target;
    grid_.hover = -1;
    grid_.cellAnims.clear();
    grid_.returnFrom.clear();
    const float width = static_cast<float>(displayBounds_.w);
    // The new page enters from the side, like a fast swipe.
    startGridSwipeAnimation(direction > 0 ? width : -width, 0.0f, 0.18);
    dirty_ = true;
}

int Application::gridDotAt(float x, float y) const {
    if (!grid_.query.empty()) {
        return -1;
    }
    const int pages = gridPageCount();
    if (pages <= 1 || y < grid_.dotY - 3.0f ||
        y > grid_.dotY + grid_.dotActiveSize + 4.0f) {
        return -1;
    }
    for (int i = 0; i < pages; ++i) {
        const float cx = grid_.dotStartX + static_cast<float>(i) * grid_.dotSpacing;
        if (std::abs(x - cx) <= grid_.dotSpacing * 0.5f) {
            return i;
        }
    }
    return -1;
}

float Application::gridIconSize() const {
    const float width = static_cast<float>(displayBounds_.w);
    const float height = static_cast<float>(displayBounds_.h);
    const float margin = width < 900.0f ? 36.0f : 70.0f;
    const float top = 88.0f;
    float bottom = 96.0f;
    if (settings_.orientation() == "bottom") {
        bottom = std::max(132.0f, layoutResult_.barThickness + 84.0f);
    }
    const float gridWidth = width - margin * 2.0f;
    const float gridHeight = height - top - bottom;
    const int columns = std::max(1, settings_.launchpadColumns());
    const int rows = std::max(1, settings_.launchpadRows());
    const float cell = std::min(gridWidth / static_cast<float>(columns),
                                gridHeight / static_cast<float>(rows));
    // ECSBSpringboard::calculateIconSize...: subtract the label height and
    // spacing, then round down to a multiple of 8.
    float iconSize = cell - (14.0f + 11.0f + 22.0f);
    iconSize = std::clamp(iconSize, 20.0f, 112.0f);
    iconSize = std::floor(iconSize / 8.0f) * 8.0f;
    return std::max(20.0f, iconSize);
}

void Application::startGridSwipeAnimation(float from, float to, double duration, bool easeOut) {
    grid_.swipeFrom = from;
    grid_.swipeTo = to;
    grid_.swipeOffset = from;
    grid_.swipeFromTime = clock_.now();
    grid_.swipeDuration = std::max(0.01, duration);
    grid_.swipeEaseOut = easeOut;
    grid_.swipeAnimating = true;
    dirty_ = true;
}

void Application::finishGridSwipe() {
    if (!grid_.blankPressed) {
        return;
    }
    const bool wasSwiping = grid_.swiping;
    const float offset = grid_.swipeOffset;
    grid_.blankPressed = false;
    grid_.swiping = false;
    if (!wasSwiping) {
        // A plain click on the wallpaper dismisses Launchpad, while a click on
        // empty space inside an expanded folder just keeps the folder open.
        if (grid_.folderId.empty()) {
            closeGrid();
        }
        return;
    }
    settleGridSwipe(offset, 0.18f);
}

void Application::settleGridSwipe(float offset, float thresholdFraction) {
    const float width = static_cast<float>(displayBounds_.w);
    const int pages = gridPageCount();
    const float threshold = width * thresholdFraction;
    if (offset <= -threshold && grid_.page + 1 < pages) {
        grid_.page++;
        startGridSwipeAnimation(offset + width, 0.0f, 0.18);
    } else if (offset >= threshold && grid_.page > 0) {
        grid_.page--;
        startGridSwipeAnimation(offset - width, 0.0f, 0.18);
    } else {
        // Overscroll at the first/last page rebounds almost immediately; a
        // below-threshold mid-page swipe uses a slightly softer snap.
        const bool atEdge = (offset > 0.0f && grid_.page <= 0) ||
                            (offset < 0.0f && grid_.page >= pages - 1);
        startGridSwipeAnimation(offset, 0.0f, atEdge ? 0.12 : 0.11, true);
    }
    grid_.hover = -1;
    dirty_ = true;
}

bool Application::handleGridWheel(float x, float y, SDL_MouseWheelDirection direction) {
    if (!grid_.open || grid_.closing || grid_.blankPressed || grid_.fingerSwiping) {
        return false;
    }
    float dx = x;
    float dy = y;
    if (direction == SDL_MOUSEWHEEL_FLIPPED) {
        dx = -dx;
        dy = -dy;
    }
    const float width = static_cast<float>(displayBounds_.w);
    if (std::abs(dx) > std::abs(dy) && dx != 0.0f) {
        const int pages = gridPageCount();
        grid_.wheelLastAt = now_;
        // Windows reports the wheel delta opposite to the finger movement
        // direction on precision touchpads; invert so the page follows the
        // fingers like a wallpaper drag. Precision touchpads send tiny deltas
        // (1/120 of a notch per event), scaled so a deliberate ~8 cm swipe
        // covers one page.
        const float delta = -dx * width * settings_.launchpadSwipeScale();
        const float edgeCap = width * 0.10f;
        const bool atFirst = grid_.page <= 0;
        const bool atLast = grid_.page >= pages - 1;
        const bool outwardStart = delta > 0.0f && atFirst;
        const bool outwardEnd = delta < 0.0f && atLast;
        const bool inward = (atFirst && delta < 0.0f) || (atLast && delta > 0.0f);

        // While suppressed, swallow outward events until the user reverses
        // direction (or the stream goes quiet). Otherwise touchpad inertia
        // keeps re-stretching the rubber band, which looks like the page
        // twitching several times in the swipe direction.
        if (grid_.wheelEdgeSuppressed) {
            if (inward || (!outwardStart && !outwardEnd)) {
                grid_.wheelEdgeSuppressed = false;
            } else {
                return true;
            }
        }

        if (!grid_.wheelSwiping) {
            if (grid_.swipeAnimating) {
                // Finish the previous page slide instantly; the page index
                // (and the page dots) already point at the new page, so the
                // next swipe should start from the settled position instead of
                // having to cover the leftover animation offset.
                grid_.swipeOffset = grid_.swipeTo;
                grid_.swipeAnimating = false;
            }
            grid_.wheelSwiping = true;
            grid_.wheelSwipeOffset = grid_.swipeOffset;
            grid_.wheelSmoothAt = now_;
            grid_.wheelEdgeHoldAt = -1.0;
            grid_.hover = -1;
        }

        if (outwardStart || outwardEnd) {
            if (grid_.wheelEdgeHoldAt < 0.0) {
                grid_.wheelEdgeHoldAt = now_;
            }
            if (now_ - grid_.wheelEdgeHoldAt > 0.30) {
                // Holding against the edge for this long means the gesture is
                // over even if the driver keeps streaming release/inertia
                // events. Snap back now and ignore the rest of the burst.
                grid_.wheelEdgeSuppressed = true;
                grid_.wheelEdgeHoldAt = -1.0;
                grid_.wheelSwiping = false;
                const float offset = grid_.swipeOffset;
                grid_.wheelSwipeOffset = 0.0f;
                settleGridSwipe(offset, settings_.launchpadSwipeThreshold());
                return true;
            }
            if (outwardStart) {
                grid_.wheelSwipeOffset = std::min(grid_.wheelSwipeOffset + delta, edgeCap);
            } else {
                grid_.wheelSwipeOffset = std::max(grid_.wheelSwipeOffset + delta, -edgeCap);
            }
        } else {
            grid_.wheelEdgeHoldAt = -1.0;
            grid_.wheelSwipeOffset =
                clampf(grid_.wheelSwipeOffset + delta, -width, width);
        }
        // Commit a page as soon as the swipe has travelled far enough, instead
        // of waiting for the gesture to end. Consecutive swipes therefore page
        // immediately one after another, and the page dots move with the
        // gesture instead of lagging behind it.
        const float threshold = width * settings_.launchpadSwipeThreshold();
        if (grid_.wheelSwipeOffset <= -threshold && grid_.page + 1 < pages) {
            grid_.page++;
            startGridSwipeAnimation(grid_.swipeOffset + width, 0.0f, 0.18);
            grid_.wheelSwipeOffset = 0.0f;
            dirty_ = true;
        } else if (grid_.wheelSwipeOffset >= threshold && grid_.page > 0) {
            grid_.page--;
            startGridSwipeAnimation(grid_.swipeOffset - width, 0.0f, 0.18);
            grid_.wheelSwipeOffset = 0.0f;
            dirty_ = true;
        }
        grid_.wheelSwipeAt = now_;
        dirty_ = true;
        return true;
    }
    if (grid_.wheelSwiping) {
        return true;
    }
    if (dy > 0.0f) {
        switchGridPage(-1);
        return true;
    }
    if (dy < 0.0f) {
        switchGridPage(1);
        return true;
    }
    return false;
}

void Application::closeGrid() {
    if (!grid_.open || grid_.closing) {
        return;
    }
    grid_.closing = true;
    grid_.pressed = false;
    grid_.dragMoved = false;
    launchpadResetDrag();
    grid_.renaming = false;
    grid_.pendingText.clear();
    grid_.pendingTextAt = -1.0;
    grid_.folderDrop.active = false;
    grid_.blankPressed = false;
    grid_.pagerPressed = false;
    grid_.swiping = false;
    grid_.swipeAnimating = false;
    grid_.swipeOffset = 0.0f;
    grid_.wheelSwiping = false;
    grid_.wheelSwipeOffset = 0.0f;
    grid_.wheelEdgeHoldAt = -1.0;
    grid_.wheelEdgeSuppressed = false;
    grid_.wheelLastAt = -100.0;
    grid_.fingers.clear();
    grid_.fingerSwiping = false;
    grid_.fingerSwipeOffset = 0.0f;
    if (gridDragOverDock_) {
        clearDragPreview();
        gridDragOverDock_ = false;
    }
    if (grid_.window) {
        SDL_StopTextInput(grid_.window);
    }
    SDL_CaptureMouse(false);
    gridOpacity_.setTarget(clock_.now(), 0.0f, 0.15);
    dirty_ = true;
}

void Application::renderDock() {
    if (!renderer_ || !text_ || !icons_) {
        return;
    }
    const std::vector<DockItem> items = layoutItems();
    const float envelope = magEnvelope_.value();
    const bool magnify = gridDragOverDock_ ||
                         (settings_.magnification() && (mouseInside_ || envelope > 0.001f));
    const float magnifyX = gridDragOverDock_ ? dragX_ : mouseX_;
    const float magnifyY = gridDragOverDock_ ? dragY_ : mouseY_;
    layoutResult_ = layout_.compute(items, settings_, windowWidth_, windowHeight_, magnifyX, magnifyY,
                                    magnify, magEnvelope_.value());
    // Only the displacement caused by a drag preview is animated. Magnification
    // and normal layout changes are applied immediately, so fast pointer sweeps
    // never leave tiles lagging behind (which used to break the spacing).
    std::vector<float> drawX(layoutResult_.tiles.size(), 0.0f);
    std::vector<float> drawY(layoutResult_.tiles.size(), 0.0f);
    const double frameDt = std::clamp(now_ - lastFrameTime_, 0.0, 0.05);
    lastFrameTime_ = now_;
    const float blend = 1.0f - std::exp(-static_cast<float>(frameDt) * 26.0f);
    std::map<std::string, float> nextShift;
    reorderAnimating_ = false;
    const bool horizontalDock = settings_.orientation() == "bottom";
    // Seed the "make room" slide against the freshly computed layout. Screen
    // coordinates are used because relayoutWindow may also have moved/resized
    // the Dock window when a tile was inserted or removed.
    if (slotAnimPending_ && dockWindow_) {
        int wx = 0;
        int wy = 0;
        SDL_GetWindowPosition(dockWindow_, &wx, &wy);
        for (size_t i = 0; i < layoutResult_.tiles.size() && i < items.size(); ++i) {
            auto found = slotAnimFromScreen_.find(items[i].id);
            if (found == slotAnimFromScreen_.end()) {
                continue;
            }
            const float screenPos =
                horizontalDock ? static_cast<float>(wx) + layoutResult_.tiles[i].cell.x
                               : static_cast<float>(wy) + layoutResult_.tiles[i].cell.y;
            slotAnim_[items[i].id] = SlotAnim{found->second - screenPos, now_};
        }
        slotAnimFromScreen_.clear();
        slotAnimPending_ = false;
    }
    LayoutResult baseLayout;
    if (dockDragPreview_) {
        baseLayout = layout_.compute(visibleItems_, settings_, windowWidth_, windowHeight_,
                                     magnifyX, magnifyY, magnify, magEnvelope_.value());
    }
    for (size_t i = 0; i < layoutResult_.tiles.size() && i < items.size(); ++i) {
        const DockItem &entry = items[i];
        const float target = layoutResult_.tiles[i].cell.x;
        float base = target;
        drawY[i] = layoutResult_.tiles[i].cell.y;
        if (dockDragPreview_) {
            for (size_t j = 0; j < baseLayout.tiles.size() && j < visibleItems_.size(); ++j) {
                if (visibleItems_[j].id == entry.id) {
                    base = baseLayout.tiles[j].cell.x;
                    break;
                }
            }
        }
        float shift = 0.0f;
        if (dockDragPreview_ && !isDragSlot(entry)) {
            const float wanted = target - base;
            auto found = shiftAnim_.find(entry.id);
            if (found != shiftAnim_.end()) {
                shift = found->second + (wanted - found->second) * blend;
                if (std::abs(wanted - shift) < 0.35f) {
                    shift = wanted;
                } else {
                    reorderAnimating_ = true;
                }
            } else {
                shift = wanted;
            }
            nextShift[entry.id] = shift;
        }
        if (!dockDragPreview_ && !isDragSlot(entry)) {
            auto found = slotAnim_.find(entry.id);
            if (found != slotAnim_.end()) {
                const double progress = (now_ - found->second.start) / 0.20;
                if (progress >= 1.0) {
                    slotAnim_.erase(found);
                } else {
                    const float offset = found->second.offset *
                                         (1.0f - easeInOut(static_cast<float>(progress)));
                    if (horizontalDock) {
                        base += offset;
                    } else {
                        drawY[i] += offset;
                    }
                    reorderAnimating_ = true;
                }
            }
        }
        if (!isDragSlot(entry)) {
            drawX[i] = base + shift;
        }
    }
    for (auto it = slotAnim_.begin(); it != slotAnim_.end();) {
        if (now_ - it->second.start > 0.6) {
            it = slotAnim_.erase(it);
        } else {
            ++it;
        }
    }
    if (dockDragPreview_) {
        shiftAnim_.swap(nextShift);
    } else if (!shiftAnim_.empty()) {
        shiftAnim_.clear();
    }

    const float offset = dockHideOffset();
    renderer_->beginFrame();

    const bool dark = isDarkTheme(settings_);
    SDL_FRect bar = layoutResult_.bar;
    bar.y += offset;

    std::vector<SDL_Rect> hitRects;
    auto addHitRect = [&](float x, float y, float w, float h) {
        SDL_Rect rect{
            static_cast<int>(std::lround(x * contentScale_)),
            static_cast<int>(std::lround(y * contentScale_)),
            static_cast<int>(std::lround(w * contentScale_)),
            static_cast<int>(std::lround(h * contentScale_)),
        };
        if (rect.w > 0 && rect.h > 0 &&
            rect.x < static_cast<int>(windowWidth_ * contentScale_) &&
            rect.y < static_cast<int>(windowHeight_ * contentScale_) &&
            rect.x + rect.w > 0 && rect.y + rect.h > 0) {
            hitRects.push_back(rect);
        }
    };
    addHitRect(bar.x - 1.0f, bar.y - 1.0f, bar.w + 2.0f, bar.h + 2.0f);
    for (const auto &tile : layoutResult_.tiles) {
        addHitRect(tile.cell.x, tile.cell.y + offset, tile.cell.w, tile.cell.h);
    }
    platform_->setHitTestRects(dockWindow_, hitRects);
    // Launchpad covers the whole screen. Even though the Dock is topmost, some
    // input can be routed to the grid; explicitly make the bar area of the grid
    // click-through so Dock clicks/menus always work while Launchpad is open.
    if (grid_.window) {
        std::vector<SDL_Rect> gridPass;
        if (grid_.open) {
            int wx = 0;
            int wy = 0;
            int gx = 0;
            int gy = 0;
            SDL_GetWindowPosition(dockWindow_, &wx, &wy);
            SDL_GetWindowPosition(grid_.window, &gx, &gy);
            SDL_Rect rect{
                static_cast<int>(std::lround((static_cast<float>(wx) + bar.x -
                                              static_cast<float>(gx)) * contentScale_)),
                static_cast<int>(std::lround((static_cast<float>(wy) + bar.y -
                                              static_cast<float>(gy)) * contentScale_)),
                static_cast<int>(std::lround(bar.w * contentScale_)),
                static_cast<int>(std::lround(bar.h * contentScale_)),
            };
            if (rect.w > 0 && rect.h > 0) {
                gridPass.push_back(rect);
            }
        }
        platform_->setPassThroughRects(grid_.window, gridPass);
    }
    const float radius = std::min(bar.h * 0.24f, 22.0f);
    if (backdrop_ && !grid_.open) {
        renderer_->texture(backdrop_, bar);
    } else {
        // Launchpad (or a missing capture): the same rounded shape and border as
        // the baked backdrop, but translucent instead of blurred.
        const int materialW = std::max(1, static_cast<int>(std::lround(bar.w * contentScale_)));
        const int materialH = std::max(1, static_cast<int>(std::lround(bar.h * contentScale_)));
        const int radiusPx = static_cast<int>(std::lround(radius * contentScale_));
        const std::string key = std::to_string(materialW) + "x" + std::to_string(materialH) +
                                "x" + std::to_string(radiusPx) + (dark ? "d" : "l");
        if (!dockMaterial_ || key != dockMaterialKey_) {
            if (dockMaterial_) {
                SDL_DestroyTexture(dockMaterial_);
                dockMaterial_ = nullptr;
            }
            dockMaterial_ = buildDockMaterial(materialW, materialH, radius * contentScale_, dark);
            dockMaterialKey_ = key;
        }
        if (dockMaterial_) {
            renderer_->texture(dockMaterial_, bar);
        } else {
            renderer_->roundedRect(bar, radius, materialColor(dark));
        }
    }

    for (size_t i = 0; i < layoutResult_.tiles.size(); ++i) {
        TileGeometry geometry = layoutResult_.tiles[i];
        geometry.cell.y += offset;
        geometry.icon.y += offset;
        geometry.label.y += offset;
        geometry.indicator.y += offset;
        if (i >= items.size()) {
            break;
        }
        const DockItem &item = items[i];
        if (isDragSlot(item)) {
            continue;
        }
        if (i < drawX.size()) {
            const float dx = drawX[i] - geometry.cell.x;
            if (std::abs(dx) > 0.01f) {
                geometry.cell.x += dx;
                geometry.icon.x += dx;
                geometry.label.x += dx;
                geometry.indicator.x += dx;
            }
        }
        if (i < drawY.size()) {
            const float dy = drawY[i] + offset - geometry.cell.y;
            if (std::abs(dy) > 0.01f) {
                geometry.cell.y += dy;
                geometry.icon.y += dy;
                geometry.label.y += dy;
                geometry.indicator.y += dy;
            }
        }
        if (geometry.separator) {
            renderer_->fillRect(geometry.icon,
                                dark ? rgba(255, 255, 255, 55) : rgba(0, 0, 0, 40));
            continue;
        }
        if (const auto it = bounce_.find(item.id); it != bounce_.end()) {
            const double elapsed = now_ - it->second;
            const bool stillLaunching = launching_.count(item.id) > 0;
            if (!settings_.reduceMotion() && (stillLaunching || elapsed < 1.0)) {
                float lift = 0.0f;
                if (stillLaunching) {
                    // Reverse-engineered bounce: folded parabolic amplitude with a
                    // 300 ms half period (one hop every 600 ms).
                    const double period = 0.3;
                    double phase = std::fmod(elapsed + period, period * 2.0);
                    phase = std::min(phase, period * 2.0 - phase);
                    const float amp =
                        1.0f - static_cast<float>((phase * phase) / (period * period));
                    lift = amp * 22.0f;
                } else {
                    const float t = static_cast<float>(elapsed);
                    lift = std::abs(std::sin(t * kPi * 2.6f)) * 22.0f * (1.0f - t);
                }
                geometry.icon.y -= lift;
                geometry.cell.y -= lift;
                geometry.label.y -= lift;
            } else if (!stillLaunching) {
                bounce_.erase(it);
            }
        }
        renderTile(item, geometry, layoutResult_);
        // A context menu replaces the tooltip, exactly like macOS does.
        if (static_cast<int>(i) == hovered_ && !menu_.open && !stack_.open && !dragging_) {
            drawLabel(item, geometry);
        }
        if ((item.isApplication() || item.type == ItemType::Finder) &&
            settings_.showIndicators()) {
            bool running = false;
            if (item.type == ItemType::Finder) {
                running = platform_->fileManagerRunning();
            } else if (const AppInfo *app = appForItem(item)) {
                running = apps_->isRunning(*app);
            }
            if (running) {
                SDL_Color color = dark ? rgba(255, 255, 255, 230) : rgba(20, 20, 25, 220);
                renderer_->circle(geometry.indicator, color);
            }
        }
    }

    auto drawDragIcon = [&](const DockItem &item, float size) {
        TileGeometry geo;
        geo.icon = SDL_FRect{dragX_ - size * 0.5f, dragY_ - size * 0.5f, size, size};
        geo.cell = geo.icon;
        geo.label = SDL_FRect{geo.icon.x, geo.icon.y - 20.0f, size, 18.0f};
        geo.indicator = SDL_FRect{geo.icon.x, geo.icon.y + size, 4.0f, 4.0f};
        geo.scale = size / std::max(1.0f, layoutResult_.baseSize);
        geo.separator = item.type == ItemType::Separator;
        renderTile(item, geo, layoutResult_);
    };
    if (dragging_ && dragMoved_ && dragged_ >= 0 &&
        dragged_ < static_cast<int>(visibleItems_.size())) {
        // The dragged tile follows the pointer; its old slot is the preview gap.
        drawDragIcon(visibleItems_[dragged_], layoutResult_.baseSize);
    }
    if (gridDragOverDock_ && grid_.pressed && !grid_.dragAppId.empty()) {
        if (const AppInfo *app = apps_->findById(grid_.dragAppId)) {
            DockItem item;
            item.type = ItemType::Application;
            item.id = app->id;
            item.path = app->sourcePath;
            item.label = app->name;
            item.iconPath = app->iconPath;
            drawDragIcon(item, layoutResult_.baseSize * 1.08f);
        }
    }

    renderer_->endFrame();
}

Application::FolderPreview &Application::folderPreviewFor(const std::string &path) {
    FolderPreview &preview = folderPreviews_[path];
    if (now_ - preview.refreshedAt < 1.0) {
        return preview;
    }
    preview.refreshedAt = now_;
    preview.entries.clear();
    preview.hasEntries = false;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(
             utf8ToPath(path),
             std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        const std::string name = pathToUtf8(it->path().filename());
        if (name.empty() || isShellMetadataFile(name) ||
            (!settings_.showHidden() && name[0] == '.')) {
            continue;
        }
        preview.hasEntries = true;
        if (preview.entries.size() >= 4) {
            continue;
        }
        const bool directory = it->is_directory(ec);
        DockItem entry;
        entry.type = directory ? ItemType::Folder : ItemType::File;
        const std::string entryPath = pathToUtf8(it->path());
        entry.id =
            std::string(directory ? "preview-folder:" : "preview-file:") + entryPath;
        entry.path = entryPath;
        entry.label = name;
        preview.entries.push_back(std::move(entry));
    }
    std::sort(preview.entries.begin(), preview.entries.end(),
              [](const DockItem &a, const DockItem &b) {
                  return lowerAscii(a.label) < lowerAscii(b.label);
              });
    return preview;
}

void Application::drawFolderStackIcon(const DockItem &item, const SDL_FRect &icon,
                                      SDL_Color tint, int logicalSize) {
    FolderPreview &preview = folderPreviewFor(item.path);
    auto drawFitted = [&](SDL_Texture *texture, const SDL_FRect &box, SDL_Color color) {
        if (!texture) {
            return;
        }
        float tw = 0.0f;
        float th = 0.0f;
        SDL_GetTextureSize(texture, &tw, &th);
        if (tw <= 0.0f || th <= 0.0f) {
            renderer_->texture(texture, box, color);
            return;
        }
        const float fit = std::min(box.w / tw, box.h / th);
        const SDL_FRect dst{box.x + (box.w - tw * fit) * 0.5f,
                            box.y + (box.h - th * fit) * 0.5f, tw * fit, th * fit};
        renderer_->texture(texture, dst, color);
    };

    const int count =
        std::min(3, static_cast<int>(preview.entries.size()));
    if (count == 0) {
        SDL_Texture *folder =
            preview.hasEntries ? folderNonemptyTexture_ : folderEmptyTexture_;
        drawFitted(folder, icon, tint);
        return;
    }

    // Back layers peek out to the lower-right, the front item stays aligned
    // with the Dock tile. This mirrors a macOS stack (the top item plus the
    // edges of the files behind it) without adding a folder plate.
    for (int layer = count - 1; layer >= 0; --layer) {
        const float scale = 1.0f - 0.09f * static_cast<float>(layer);
        const float shiftX = icon.w * 0.075f * static_cast<float>(layer);
        const float shiftY = icon.h * 0.060f * static_cast<float>(layer);
        SDL_FRect box{icon.x + shiftX + (icon.w - icon.w * scale) * 0.5f,
                      icon.y + shiftY + (icon.h - icon.h * scale) * 0.5f,
                      icon.w * scale, icon.h * scale};
        SDL_Color color = tint;
        if (layer > 0) {
            color = rgba(static_cast<uint8_t>(180 + (2 - layer) * 25),
                         static_cast<uint8_t>(182 + (2 - layer) * 25),
                         static_cast<uint8_t>(190 + (2 - layer) * 22), tint.a);
        }
        const DockItem &entry = preview.entries[static_cast<size_t>(layer)];
        if (entry.type == ItemType::Folder) {
            // A folder inside a stack keeps the updated folder artwork.
            drawFitted(folderNonemptyTexture_, box, color);
            continue;
        }
        SDL_Texture *texture = icons_->textureFor(entry, logicalSize);
        if (texture) {
            drawFitted(texture, box, color);
        } else {
            renderer_->placeholderIcon(box, rgba(70, 72, 84, 245),
                                       rgba(110, 114, 132, 245));
        }
    }
}

void Application::renderTile(const DockItem &item, const TileGeometry &geometry,
                             const LayoutResult &layout) {
    (void)layout;
    const bool dark = isDarkTheme(settings_);
    if (!launchpadIconsLoaded_) {
        ensureLaunchpadIcons();
    }
    if (item.type == ItemType::Finder) {
        if (finderIcon_) {
            renderer_->texture(finderIcon_, geometry.icon);
        } else {
            renderer_->folderIcon(geometry.icon, rgba(80, 160, 235, 245),
                                  rgba(110, 185, 250, 245));
        }
        return;
    }
    if (item.type == ItemType::AppGrid) {
        SDL_Texture *launchpad = dark ? launchpadDark_ : launchpadLight_;
        if (launchpad) {
            renderer_->texture(launchpad, geometry.icon);
            return;
        }
        renderer_->roundedRect(geometry.icon, geometry.icon.w * 0.22f,
                               rgba(226, 229, 236, 245));
        renderer_->roundedRect(SDL_FRect{geometry.icon.x - 1.0f, geometry.icon.y - 1.0f,
                                         geometry.icon.w + 2.0f, geometry.icon.h + 2.0f},
                               geometry.icon.w * 0.23f, rgba(255, 255, 255, 90));
        renderer_->gridIcon(insetRect(geometry.icon, geometry.icon.w * 0.2f,
                                      geometry.icon.h * 0.2f),
                            rgba(48, 52, 66, 235));
        return;
    }
    if (item.type == ItemType::Trash) {
        const bool dropTarget = !dropFolderId_.empty() && item.id == dropFolderId_;
        SDL_Texture *trash =
            dark ? (trashEmpty_ ? trashEmptyDark_ : trashFullDark_)
                 : (trashEmpty_ ? trashEmptyLight_ : trashFullLight_);
        if (trash) {
            renderer_->texture(trash, geometry.icon,
                               dropTarget ? rgba(125, 125, 130, 255)
                                          : rgba(255, 255, 255, 255));
        } else {
            renderer_->trashIcon(geometry.icon,
                                 dark ? rgba(196, 200, 210, 230) : rgba(90, 94, 104, 235),
                                 dark ? rgba(230, 234, 242, 235) : rgba(70, 74, 84, 235));
        }
        if (dropTarget) {
            const float side = geometry.icon.w * 0.805f;
            renderer_->roundedRect(
                SDL_FRect{geometry.icon.x + (geometry.icon.w - side) * 0.5f,
                          geometry.icon.y + (geometry.icon.h - side) * 0.5f, side, side},
                side * 0.2237f, rgba(0, 0, 0, 40));
        }
        return;
    }
    if (item.type == ItemType::Separator) {
        return;
    }
    if (item.type == ItemType::MinimizedWindow) {
        // A minimized window is a mini screenshot, not a squircle icon: its
        // slot already follows the window's aspect ratio, and the owning app's
        // processed icon sits in the bottom-right corner, flush right.
        SDL_FRect square{geometry.icon.x - 1.0f, geometry.icon.y - 1.0f,
                         geometry.icon.w + 2.0f, geometry.icon.h + 2.0f};
        const AppInfo *owner = apps_->findByPath(item.path);
        SDL_Texture *thumb = nullptr;
        auto found = windowThumbnails_.find(item.windowHandle);
        if (found != windowThumbnails_.end()) {
            thumb = found->second;
        }
        const SDL_FRect windowRect =
            thumb ? minimizedPreviewRect(geometry.icon, item.previewAspect) : square;
        if (thumb) {
            renderer_->shadow(windowRect, 4.0f, rgba(0, 0, 0, dark ? 130 : 80), 5.0f);
            renderer_->texture(thumb, windowRect);
        } else if (owner) {
            SDL_Texture *appIcon =
                icons_->textureForApp(*owner, static_cast<int>(std::lround(square.w)));
            if (appIcon) {
                renderer_->texture(appIcon, square);
            }
        }
        if (!thumb && !owner) {
            renderer_->placeholderIcon(square, dark ? rgba(70, 72, 84, 245)
                                                    : rgba(218, 220, 228, 245),
                                       dark ? rgba(110, 114, 132, 245)
                                            : rgba(180, 184, 196, 245));
        }
        if (owner && thumb) {
            const float badge = clampf(windowRect.h * 0.44f, 14.0f, windowRect.h);
            SDL_Texture *badgeTexture =
                icons_->textureForApp(*owner, static_cast<int>(std::lround(badge)));
            if (badgeTexture) {
                const SDL_FRect badgeRect{windowRect.x + windowRect.w - badge,
                                          windowRect.y + windowRect.h - badge, badge, badge};
                renderer_->shadow(badgeRect, badge * 0.22f, rgba(0, 0, 0, 90), 3.0f);
                renderer_->texture(badgeTexture, badgeRect);
            }
        }
        return;
    }

    const int logicalSize = static_cast<int>(std::lround(geometry.icon.w));
    if (item.type == ItemType::Folder) {
        const bool dropTarget = !dropFolderId_.empty() && item.id == dropFolderId_;
        const SDL_Color dimTint = dropTarget ? rgba(125, 125, 130, 255)
                                             : rgba(255, 255, 255, 255);
        if (stack_.open && !stack_.itemId.empty() && item.id == stack_.itemId) {
            // The open stack's Dock tile becomes the macOS pile arrow.
            SDL_Texture *arrow = dark ? pileArrowWhite_ : pileArrowBlack_;
            if (arrow) {
                renderer_->texture(arrow, geometry.icon, dimTint);
                return;
            }
            const SDL_FRect &r = geometry.icon;
            const SDL_Color fill = dark ? rgba(128, 128, 134, 242) : rgba(198, 198, 203, 242);
            renderer_->roundedRect(r, r.w * 0.22f, fill);
            const float cx = r.x + r.w * 0.5f;
            const float cy = r.y + r.h * 0.47f;
            const float half = r.w * 0.18f;
            const float drop = r.w * 0.13f;
            const float thick = std::max(2.0f, r.w * 0.055f);
            const SDL_Color ink = dark ? rgba(240, 240, 244, 246) : rgba(66, 66, 72, 246);
            renderer_->thickLine(cx - half, cy - drop * 0.65f, cx, cy + drop * 0.75f, thick,
                                 ink);
            renderer_->thickLine(cx, cy + drop * 0.75f, cx + half, cy - drop * 0.65f, thick,
                                 ink);
            return;
        }
        FolderPreview &preview = folderPreviewFor(item.path);
        if (item.stack && preview.hasEntries) {
            drawFolderStackIcon(item, geometry.icon, dimTint, logicalSize);
            return;
        }
        SDL_Texture *folder =
            preview.hasEntries ? folderNonemptyTexture_ : folderEmptyTexture_;
        if (folder) {
            float tw = 0.0f;
            float th = 0.0f;
            SDL_GetTextureSize(folder, &tw, &th);
            if (tw > 0.0f && th > 0.0f) {
                const float fit =
                    std::min(geometry.icon.w / tw, geometry.icon.h / th);
                const SDL_FRect dst{
                    geometry.icon.x + (geometry.icon.w - tw * fit) * 0.5f,
                    geometry.icon.y + (geometry.icon.h - th * fit) * 0.5f,
                    tw * fit, th * fit};
                renderer_->texture(folder, dst, dimTint);
                return;
            }
        }
        renderer_->folderIcon(geometry.icon, rgba(80, 160, 235, 245), rgba(110, 185, 250, 245));
        return;
    }

    SDL_Texture *texture = icons_->textureFor(item, logicalSize);
    const bool dragTarget =
        !dragHoverId_.empty() && item.id == dragHoverId_ && item.isApplication();
    if (texture) {
        SDL_FRect iconRect = geometry.icon;
        if (item.type == ItemType::File || item.type == ItemType::Url) {
            // Plain files keep their original icon proportions instead of being
            // stretched into the square app-icon box.
            float tw = 0.0f;
            float th = 0.0f;
            SDL_GetTextureSize(texture, &tw, &th);
            if (tw > 0.0f && th > 0.0f) {
                const float fit = std::min(geometry.icon.w / tw, geometry.icon.h / th);
                iconRect = SDL_FRect{geometry.icon.x + (geometry.icon.w - tw * fit) * 0.5f,
                                     geometry.icon.y + (geometry.icon.h - th * fit) * 0.5f,
                                     tw * fit, th * fit};
            }
        }
        // Tinting dims exactly the non-transparent icon pixels; no extra dark
        // plate is drawn around the artwork.
        renderer_->texture(texture, iconRect,
                           dragTarget ? rgba(115, 115, 120, 255) : rgba(255, 255, 255, 255));
    } else {
        renderer_->placeholderIcon(geometry.icon, dark ? rgba(70, 72, 84, 245)
                                                       : rgba(218, 220, 228, 245),
                                   dark ? rgba(110, 114, 132, 245)
                                        : rgba(180, 184, 196, 245));
        if (dragTarget) {
            const float side = geometry.icon.w * 0.805f;
            renderer_->roundedRect(
                SDL_FRect{geometry.icon.x + (geometry.icon.w - side) * 0.5f,
                          geometry.icon.y + (geometry.icon.h - side) * 0.5f, side, side},
                side * 0.2237f, rgba(0, 0, 0, 80));
        }
    }
    if (!flashId_.empty() && item.id == flashId_) {
        const float t = clampf(static_cast<float>((now_ - flashAt_) / 0.5), 0.0f, 1.0f);
        const float alpha = std::max(0.0f, std::sin(t * 4.0f * kPi)) * 110.0f;
        const float side = geometry.icon.w * 0.805f;
        renderer_->roundedRect(
            SDL_FRect{geometry.icon.x + (geometry.icon.w - side) * 0.5f,
                      geometry.icon.y + (geometry.icon.h - side) * 0.5f, side, side},
            side * 0.2237f, rgba(255, 255, 255, static_cast<uint8_t>(alpha)));
    }
}

void Application::drawLabel(const DockItem &item, const TileGeometry &geometry) {
    if (!settings_.showLabels() || item.type == ItemType::Separator) {
        return;
    }
    const bool dark = isDarkTheme(settings_);
    const float fontPx = std::max(11.0f, settings_.tileSize() * 0.23f);
    const float textHeight = std::max(fontPx + 4.0f, text_->lineHeight(fontPx));
    const float maxWidth = std::min(260.0f, windowWidth_ - 16.0f);
    std::string text = item.label;
    if (item.isApplication()) {
        // Apps are always named after the application, never the current
        // window/document title.
        if (const AppInfo *app = appForItem(item)) {
            text = app->name;
        }
    } else if (item.type == ItemType::AppGrid) {
        text = "Launchpad";
    } else if (item.type == ItemType::Trash) {
        text = "Trash";
    } else if (item.type == ItemType::Finder) {
        text = "Finder";
    }
    const float measured = text_->measure(text, fontPx).x;
    const float pillWidth = clampf(measured + 18.0f, 36.0f, maxWidth);
    const float pillHeight = textHeight + 8.0f;
    const float arrowWidth = 12.0f;
    const float arrowHeight = 7.0f;
    const float centerX = geometry.icon.x + geometry.icon.w * 0.5f;
    SDL_FRect pill{clampf(centerX - pillWidth * 0.5f, 4.0f, windowWidth_ - pillWidth - 4.0f),
                   geometry.icon.y - pillHeight - arrowHeight - 4.0f, pillWidth, pillHeight};
    const float totalHeight = pillHeight + arrowHeight;
    const float tipX = clampf(centerX - pill.x, arrowWidth * 0.5f,
                              pillWidth - arrowWidth * 0.5f);
    const std::string key = text + "#" + std::to_string(static_cast<int>(pill.x)) + "," +
                            std::to_string(static_cast<int>(pill.y)) + "," +
                            std::to_string(static_cast<int>(pillWidth)) + "," +
                            std::to_string(dark ? 1 : 0) +
                            (grid_.open ? "#flat" : "#blur");
    if (key != tooltipKey_ || now_ >= tooltipRefreshAt_) {
        if (tooltipBackdrop_) {
            SDL_DestroyTexture(tooltipBackdrop_);
            tooltipBackdrop_ = nullptr;
        }
        tooltipBackdrop_ = buildPopoverTexture(dockWindow_, dockRenderer_, pill.x, pill.y,
                                               pillWidth, totalHeight, 7.0f, arrowWidth,
                                               arrowHeight, tipX, dark, !grid_.open);
        tooltipKey_ = key;
        tooltipRefreshAt_ = now_ + 0.5;
    }
    if (tooltipBackdrop_) {
        renderer_->texture(tooltipBackdrop_,
                           SDL_FRect{pill.x, pill.y, pillWidth, totalHeight});
    } else {
        renderer_->roundedRect(pill, 6.0f,
                               dark ? rgba(30, 30, 36, 225) : rgba(248, 248, 251, 232));
    }
    text_->drawTextEllipsized(text, SDL_FRect{pill.x + 7.0f, pill.y + 4.0f,
                                              pill.w - 14.0f, pill.h - 8.0f},
                              fontPx, primaryText(dark));
}

float Application::menuContentHeight(const MenuState &state) const {
    float total = 0.0f;
    for (const auto &item : state.items) {
        total += item.separator ? kMenuSeparatorHeight
                                : (item.header ? 20.0f : kMenuRowHeight);
    }
    return total;
}

float Application::menuRowTop(const MenuState &state, int index) const {
    float top = kMenuPadding;
    for (int i = 0; i < index && i < static_cast<int>(state.items.size()); ++i) {
        const auto &item = state.items[i];
        top += item.separator ? kMenuSeparatorHeight : (item.header ? 20.0f : kMenuRowHeight);
    }
    return top;
}

int Application::menuRowAt(const MenuState &state, float y) const {
    float top = kMenuPadding;
    for (int i = 0; i < static_cast<int>(state.items.size()); ++i) {
        const auto &item = state.items[i];
        const float height =
            item.separator ? kMenuSeparatorHeight : (item.header ? 20.0f : kMenuRowHeight);
        if (y >= top && y < top + height) {
            return i;
        }
        top += height;
    }
    return -1;
}

void Application::clearMenuIcons(MenuState &state) {
    for (auto &[key, texture] : state.icons) {
        (void)key;
        if (texture) {
            SDL_DestroyTexture(texture);
        }
    }
    state.icons.clear();
}

SDL_Texture *Application::menuIconTexture(MenuState &state, const MenuItem &item) {
    if (item.iconPath.empty() || !state.renderer) {
        return nullptr;
    }
    auto found = state.icons.find(item.iconPath);
    if (found != state.icons.end()) {
        return found->second;
    }
    const int pixelSize = std::max(16, static_cast<int>(std::lround(32.0f * contentScale_)));
    SDL_Surface *surface = platform_->loadIconPath(item.iconPath, pixelSize);
    if (!surface) {
        state.icons[item.iconPath] = nullptr;
        return nullptr;
    }
    SDL_Texture *texture = SDL_CreateTextureFromSurface(state.renderer, surface);
    SDL_DestroySurface(surface);
    if (texture) {
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    }
    state.icons[item.iconPath] = texture;
    return texture;
}

void Application::renderMenu() {
    renderMenuPanel(menu_);
    if (submenu_.open) {
        renderMenuPanel(submenu_);
    }
}

void Application::renderMenuPanel(MenuState &state) {
    if (!state.open || !state.draw || !state.text) {
        return;
    }
    const bool dark = isDarkTheme(settings_);
    state.draw->beginFrame();
    // Keep an opaque panel underneath the frosted capture. It guarantees a
    // readable menu even if a compositor refuses to capture the screen (for
    // example when screen recording is blocked by a capture-protected window).
    state.draw->roundedRect(SDL_FRect{0.0f, 0.0f, state.width, state.panelHeight}, 9.0f,
                            dark ? rgba(30, 30, 36, 246) : rgba(248, 248, 251, 248));
    if (state.backdrop) {
        state.draw->texture(state.backdrop, SDL_FRect{0.0f, 0.0f, state.width, state.height});
    }

    const float textX = 14.0f + (state.hasChecks ? 12.0f : 0.0f) +
                        (state.hasIcons ? 20.0f : 0.0f);
    const float iconX = state.hasChecks ? 24.0f : 10.0f;
    for (size_t i = 0; i < state.items.size(); ++i) {
        const auto &item = state.items[i];
        const float y = menuRowTop(state, static_cast<int>(i));
        if (item.separator) {
            state.draw->fillRect(SDL_FRect{8.0f, y + kMenuSeparatorHeight * 0.5f,
                                           state.width - 16.0f, 1.0f},
                                 dark ? rgba(255, 255, 255, 35) : rgba(0, 0, 0, 30));
            continue;
        }
        if (item.header) {
            state.text->drawText(item.label, 14.0f, y + 2.0f, 11.0f,
                                 secondaryText(dark));
            continue;
        }
        const bool hovered = static_cast<int>(i) == state.hover && item.enabled;
        if (hovered) {
            state.draw->roundedRect(
                SDL_FRect{4.0f, y + 1.0f, state.width - 8.0f, kMenuRowHeight - 2.0f}, 5.0f,
                rgba(0, 122, 255, 232));
        }
        SDL_Color color = item.enabled ? (hovered ? rgba(255, 255, 255, 255) : primaryText(dark))
                                       : secondaryText(dark);
        SDL_Color detailColor = hovered ? rgba(255, 255, 255, 215) : secondaryText(dark);
        if (item.checked) {
            state.draw->thickLine(8.0f, y + 12.0f, 11.0f, y + 15.0f, 1.8f, color);
            state.draw->thickLine(11.0f, y + 15.0f, 16.0f, y + 8.0f, 1.8f, color);
        }
        if (state.hasIcons) {
            SDL_Texture *icon = menuIconTexture(state, item);
            if (icon) {
                state.draw->texture(icon,
                                    SDL_FRect{iconX, y + (kMenuRowHeight - 16.0f) * 0.5f,
                                              16.0f, 16.0f});
            }
        }
        const bool hasSubmenu = !item.submenu.empty();
        float rightGutter = 16.0f + (hasSubmenu ? 14.0f : 0.0f);
        float detailWidth = 0.0f;
        if (!item.detail.empty()) {
            detailWidth = std::min(180.0f, state.text->measure(item.detail, 11.0f).x);
            rightGutter += detailWidth + 12.0f;
        }
        const float labelRight = state.width - rightGutter;
        state.text->drawTextEllipsizedLeft(
            item.label, SDL_FRect{textX, y + (kMenuRowHeight - 18.0f) * 0.5f,
                                  std::max(10.0f, labelRight - textX), 18.0f},
            state.fontPx, color);
        if (detailWidth > 0.0f) {
            const float detailX = state.width - 16.0f - (hasSubmenu ? 14.0f : 0.0f) -
                                  detailWidth;
            state.text->drawTextEllipsizedRight(
                item.detail, SDL_FRect{detailX, y + (kMenuRowHeight - 16.0f) * 0.5f,
                                       detailWidth, 16.0f},
                11.0f, detailColor);
        }
        if (hasSubmenu) {
            const float cx = state.width - 13.0f;
            const float cy = y + kMenuRowHeight * 0.5f;
            state.draw->thickLine(cx - 2.0f, cy - 3.5f, cx + 2.0f, cy, 1.4f, color);
            state.draw->thickLine(cx + 2.0f, cy, cx - 2.0f, cy + 3.5f, 1.4f, color);
        }
    }
    state.draw->endFrame();
}

void Application::renderGrid() {
    if (!grid_.open || !grid_.draw || !grid_.text || !grid_.icons) {
        return;
    }
    grid_.draw->setOpacity(gridOpacity_.value());
    grid_.text->setOpacity(gridOpacity_.value());
    grid_.draw->beginFrame();
    SDL_FRect full{0.0f, 0.0f, static_cast<float>(displayBounds_.w),
                   static_cast<float>(displayBounds_.h)};
    if (grid_.wallpaper) {
        grid_.draw->texture(grid_.wallpaper, full);
        grid_.draw->fillRect(full, rgba(10, 10, 16, 105));
    } else {
        grid_.draw->fillRect(full, rgba(8, 8, 12, 172));
    }

    computeGridMetrics();
    launchpadSearchResults();
    const bool folderOpen = !grid_.folderId.empty();
    const bool searching = !grid_.query.empty();
    const float folderAnim = grid_.folderAnim.value();
    const int perPage = std::max(1, grid_.columns * grid_.rows);
    const int pageCount = gridPageCount();
    grid_.page = std::clamp(grid_.page, 0, pageCount - 1);
    // Shell icon extraction costs 10-20 ms each.  During the folder morph,
    // load at most one per frame so the 220 ms animation stays smooth; the
    // idle preloader usually has them cached already.
    int iconBudget =
        grid_.firstFrame ? 0 : (grid_.folderAnim.active() ? 1 : 6);
    const float iconSize = grid_.iconSize;
    // App labels inside the light frosted folder panel use dark text; on the
    // wallpaper they stay white like the rest of Launchpad.
    const SDL_Color labelColor =
        folderOpen ? rgba(30, 30, 34, 235) : rgba(245, 245, 248, 235);

    if (!folderOpen) {
        drawLaunchpadSearchField(labelColor);
    }

    auto cellKey = [](const LaunchpadCell &cell) {
        return cell.folder ? "f:" + cell.folderId : "a:" + cell.appId;
    };
    auto animPosition = [&](const GridState::CellAnim &anim) {
        if (!anim.init || anim.duration <= 0.0) {
            return std::pair<float, float>{anim.toX, anim.toY};
        }
        const float t = clampf(
            static_cast<float>((now_ - anim.start) / anim.duration), 0.0f,
            1.0f);
        const float eased = easeInOut(t);
        return std::pair<float, float>{lerpf(anim.fromX, anim.toX, eased),
                                       lerpf(anim.fromY, anim.toY, eased)};
    };

    auto drawPage = [&](int pageIndex, float dx, bool allowLoad,
                        bool withGap) {
        std::vector<LaunchpadCell> cells;
        cells.reserve(static_cast<size_t>(perPage));
        for (int i = 0; i < perPage; ++i) {
            const LaunchpadCell cell = launchpadCellAt(pageIndex, i);
            if (cell.appId.empty() && !cell.folder) {
                break;
            }
            cells.push_back(cell);
        }
        const bool dragging = grid_.pressed && grid_.dragMoved;
        const bool sourceHere =
            dragging && grid_.dragOriginFolderId == grid_.folderId &&
            grid_.dragPage == pageIndex && grid_.dragSlot >= 0 &&
            grid_.dragSlot < static_cast<int>(cells.size());
        // The lifted item is never drawn twice: its original cell is removed
        // even when the current target is another app/folder.  A gap is then
        // inserted only for Gap targets.
        const bool gapOnThisPage =
            withGap && dragging &&
            grid_.dragTarget == GridState::DragTarget::Gap &&
            grid_.dragTargetPage == pageIndex;
        if (sourceHere) {
            const int sourceSlot = grid_.dragSlot;
            cells.erase(cells.begin() + sourceSlot);
            if (gapOnThisPage) {
                int gap = grid_.dragTargetSlot;
                if (sourceSlot < gap) {
                    --gap;
                }
                gap = std::clamp(gap, 0, static_cast<int>(cells.size()));
                cells.insert(cells.begin() + gap, LaunchpadCell{});
            } else {
                // App/folder targets keep the original hole: other icons do
                // not rush aside until the pointer crosses the 3/4 threshold.
                const int gap =
                    std::clamp(sourceSlot, 0, static_cast<int>(cells.size()));
                cells.insert(cells.begin() + gap, LaunchpadCell{});
            }
        } else if (gapOnThisPage) {
            const int gap = std::clamp(grid_.dragTargetSlot, 0,
                                       static_cast<int>(cells.size()));
            cells.insert(cells.begin() + gap, LaunchpadCell{});
        }
        for (int i = 0; i < static_cast<int>(cells.size()) && i < perPage; ++i) {
            const LaunchpadCell &cell = cells[static_cast<size_t>(i)];
            if (cell.appId.empty() && !cell.folder) {
                continue;
            }
            const int col = i % grid_.columns;
            const int row = i / grid_.columns;
            const float targetX = grid_.gridRect.x +
                                  (static_cast<float>(col) + 0.5f) *
                                      grid_.cellWidth;
            const float targetY =
                grid_.gridRect.y +
                (static_cast<float>(row) + 0.5f) * grid_.cellHeight;
            float localX = targetX;
            float localY = targetY;
            if (pageIndex == grid_.page) {
                const std::string key = cellKey(cell);
                GridState::CellAnim &anim = grid_.cellAnims[key];
                if (!anim.init) {
                    anim.init = true;
                    anim.toX = targetX;
                    anim.toY = targetY;
                    anim.fromX = targetX;
                    anim.fromY = targetY;
                    const auto pending = grid_.returnFrom.find(key);
                    if (pending != grid_.returnFrom.end()) {
                        anim.fromX = pending->second.x;
                        anim.fromY = pending->second.y;
                        anim.start = now_;
                        anim.duration = 0.22;
                        grid_.returnFrom.erase(pending);
                    } else {
                        anim.start = -100.0;
                        anim.duration = 0.18;
                    }
                } else if (std::abs(anim.toX - targetX) > 0.4f ||
                           std::abs(anim.toY - targetY) > 0.4f) {
                    const auto current = animPosition(anim);
                    anim.fromX = current.first;
                    anim.fromY = current.second;
                    anim.toX = targetX;
                    anim.toY = targetY;
                    anim.start = now_;
                    anim.duration = 0.18;
                }
                const auto current = animPosition(anim);
                localX = current.first;
                localY = current.second;
            }
            const float cx = localX + dx;
            const float cy = localY;
            const SDL_FRect icon{cx - iconSize * 0.5f,
                                 cy - iconSize * 0.5f - 8.0f, iconSize,
                                 iconSize};
            const bool appTarget =
                grid_.dragTarget == GridState::DragTarget::App &&
                !cell.appId.empty() &&
                cell.appId == grid_.dragTargetAppId;
            const float baseOpacity = grid_.draw->opacity();
            if (appTarget && grid_.dragTargetAt > 0.0 &&
                now_ - grid_.dragTargetAt >= 0.12) {
                grid_.draw->setOpacity(baseOpacity * 0.45f);
            }
            if (cell.folder && cell.item) {
                drawLaunchpadFolderTile(*cell.item, icon,
                                        static_cast<int>(iconSize));
            } else if (cell.app) {
                const int px = static_cast<int>(iconSize);
                const bool attempted =
                    grid_.attemptedIcons.count(cell.app->id) > 0;
                if (!attempted && (!allowLoad || iconBudget <= 0)) {
                    grid_.draw->placeholderIcon(
                        icon, rgba(70, 72, 84, 245), rgba(110, 114, 132, 245));
                } else {
                    if (!attempted) {
                        grid_.attemptedIcons.insert(cell.app->id);
                        --iconBudget;
                    }
                    if (SDL_Texture *texture =
                            grid_.icons->textureForApp(*cell.app, px)) {
                        grid_.draw->texture(texture, icon);
                    } else {
                        grid_.draw->placeholderIcon(icon,
                                                    rgba(70, 72, 84, 245),
                                                    rgba(110, 114, 132, 245));
                    }
                }
            } else {
                grid_.draw->placeholderIcon(icon, rgba(70, 72, 84, 245),
                                            rgba(110, 114, 132, 245));
            }
            if (appTarget && grid_.dragTargetAt > 0.0 &&
                now_ - grid_.dragTargetAt >= 0.12) {
                grid_.draw->setOpacity(baseOpacity);
            }
            const float labelWidth =
                std::max(icon.w + 34.0f, grid_.cellWidth - 14.0f);
            std::string text;
            if (cell.folder) {
                text = cell.title;
            } else if (cell.app) {
                text = cell.app->name;
            } else {
                text = cell.title;
            }
            if (!text.empty()) {
                grid_.text->drawTextEllipsized(
                    text,
                    SDL_FRect{cx - labelWidth * 0.5f,
                              icon.y + icon.h - 7.0f, labelWidth, 24.0f},
                    13.5f, labelColor);
            }
        }
    };

    if (folderOpen && folderAnim > 0.001f) {
        grid_.draw->fillRect(
            full, rgba(4, 4, 8, static_cast<uint8_t>(110.0f * folderAnim)));
        drawLaunchpadFolderPanel();
        const float contentOpacity =
            gridOpacity_.value() *
            clampf((folderAnim - 0.45f) / 0.55f, 0.0f, 1.0f);
        grid_.draw->setOpacity(contentOpacity);
        grid_.text->setOpacity(contentOpacity);
        if (grid_.swipeOffset < -0.5f && grid_.page + 1 < pageCount) {
            drawPage(grid_.page + 1, grid_.swipeOffset + full.w, true, true);
        } else if (grid_.swipeOffset > 0.5f && grid_.page > 0) {
            drawPage(grid_.page - 1, grid_.swipeOffset - full.w, true, true);
        }
        drawPage(grid_.page, grid_.swipeOffset, true, true);
        if (!gridDragOverDock_ && grid_.pressed && grid_.dragMoved) {
            const LaunchpadCell cell =
                launchpadCellAt(grid_.dragPage, grid_.dragSlot);
            SDL_FRect ghost{grid_.dragX - iconSize * 0.5f,
                            grid_.dragY - iconSize * 0.5f - 8.0f, iconSize,
                            iconSize};
            const AppInfo *draggedApp = grid_.dragAppId.empty()
                                            ? nullptr
                                            : apps_->findById(grid_.dragAppId);
            if (draggedApp) {
                if (SDL_Texture *texture = grid_.icons->textureForApp(
                        *draggedApp, static_cast<int>(iconSize))) {
                    grid_.draw->texture(texture, ghost);
                }
            } else if (cell.folder && cell.item) {
                drawLaunchpadFolderTile(*cell.item, ghost,
                                        static_cast<int>(iconSize));
            } else if (cell.app) {
                if (SDL_Texture *texture = grid_.icons->textureForApp(
                        *cell.app, static_cast<int>(iconSize))) {
                    grid_.draw->texture(texture, ghost);
                }
            }
        }
        grid_.draw->setOpacity(gridOpacity_.value());
        grid_.text->setOpacity(gridOpacity_.value());
    } else {
        // Current page plus the neighbours during a horizontal swipe.
        if (grid_.swipeOffset < -0.5f && grid_.page + 1 < pageCount) {
            drawPage(grid_.page + 1, grid_.swipeOffset + full.w, true, true);
        } else if (grid_.swipeOffset > 0.5f && grid_.page > 0) {
            drawPage(grid_.page - 1, grid_.swipeOffset - full.w, true, true);
        }
        drawPage(grid_.page, grid_.swipeOffset, true, true);

        if (!gridDragOverDock_ && grid_.pressed && grid_.dragMoved) {
            const LaunchpadCell cell =
                launchpadCellAt(grid_.dragPage, grid_.dragSlot);
            SDL_FRect ghost{grid_.dragX - iconSize * 0.5f,
                            grid_.dragY - iconSize * 0.5f - 8.0f, iconSize,
                            iconSize};
            grid_.draw->setOpacity(gridOpacity_.value() * 0.88f);
            if (cell.folder && cell.item) {
                drawLaunchpadFolderTile(*cell.item, ghost,
                                        static_cast<int>(iconSize));
            } else if (cell.app) {
                if (SDL_Texture *texture =
                        grid_.icons->textureForApp(*cell.app,
                                                   static_cast<int>(iconSize))) {
                    grid_.draw->texture(texture, ghost);
                }
            }
            grid_.draw->setOpacity(gridOpacity_.value());
        }

        if (grid_.folderDrop.active) {
            const AppInfo *app = apps_->findById(grid_.folderDrop.appId);
            if (app) {
                const float t = clampf(
                    static_cast<float>((now_ - grid_.folderDrop.start) /
                                       grid_.folderDrop.duration),
                    0.0f, 1.0f);
                const float eased = easeInOut(t);
                const SDL_FRect &from = grid_.folderDrop.from;
                const SDL_FRect &to = grid_.folderDrop.to;
                const SDL_FRect rect{
                    lerpf(from.x, to.x, eased), lerpf(from.y, to.y, eased),
                    lerpf(from.w, to.w, eased), lerpf(from.h, to.h, eased)};
                grid_.draw->setOpacity(gridOpacity_.value() *
                                       lerpf(0.92f, 0.08f, eased));
                if (SDL_Texture *texture = grid_.icons->textureForApp(
                        *app, static_cast<int>(iconSize))) {
                    grid_.draw->texture(texture, rect);
                }
                grid_.draw->setOpacity(gridOpacity_.value());
            }
        }

        // Page indicator (ECPagerControlLayer): the current page is a slightly
        // larger opaque dot, the others are smaller and translucent. Search
        // results hide the pager (SBSearchPage).
        grid_.dotSpacing = 16.0f;
        grid_.dotActiveSize = 9.0f;
        grid_.dotInactiveSize = 7.0f;
        grid_.dotY = grid_.gridRect.y + grid_.gridRect.h + 9.0f;
        grid_.dotStartX = full.w * 0.5f -
                          static_cast<float>(pageCount) *
                              grid_.dotSpacing * 0.5f +
                          grid_.dotSpacing * 0.5f;
        if (!searching && pageCount > 1) {
            for (int i = 0; i < pageCount; ++i) {
                const bool active = i == grid_.page;
                const float size =
                    active ? grid_.dotActiveSize : grid_.dotInactiveSize;
                const float cx =
                    grid_.dotStartX + static_cast<float>(i) * grid_.dotSpacing;
                SDL_FRect dot{cx - size * 0.5f,
                              grid_.dotY + (grid_.dotActiveSize - size) * 0.5f,
                              size, size};
                grid_.draw->circle(
                    dot, active ? rgba(245, 245, 248, 240)
                                : rgba(245, 245, 248, 95));
            }
        }
    }
    grid_.draw->endFrame();
    if (grid_.firstFrame) {
        RD_LOG_DEBUG("launchpad first frame %.1f ms after open",
                     (now_ - grid_.openedAt) * 1000.0);
    }
    grid_.firstFrame = false;
}

void Application::createStackWindow() {
    if (stack_.window) {
        return;
    }
    stack_.window = SDL_CreateWindow("RevDock Stack", 420, 320,
        static_cast<SDL_WindowFlags>(SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP |
                                     SDL_WINDOW_TRANSPARENT | SDL_WINDOW_UTILITY |
                                     SDL_WINDOW_HIGH_PIXEL_DENSITY));
    if (!stack_.window) {
        RD_LOG_WARN("Stack window creation failed: %s", SDL_GetError());
        return;
    }
    stack_.renderer = SDL_CreateRenderer(stack_.window, nullptr);
    if (!stack_.renderer) {
        SDL_DestroyWindow(stack_.window);
        stack_.window = nullptr;
        return;
    }
    SDL_SetRenderVSync(stack_.renderer, 1);
    SDL_SetRenderDrawBlendMode(stack_.renderer, SDL_BLENDMODE_BLEND);
    const float scale = SDL_GetWindowDisplayScale(stack_.window);
    SDL_SetRenderScale(stack_.renderer, scale > 0.0f ? scale : 1.0f,
                       scale > 0.0f ? scale : 1.0f);
    stack_.draw = std::make_unique<Renderer>(stack_.renderer, scale > 0.0f ? scale : 1.0f);
    const std::filesystem::path regular = paths_.assetDir / "fonts" / "Inter" / "Inter-Regular.ttf";
    stack_.text = std::make_unique<TextRenderer>(stack_.renderer, regular.string(), fallbackFontPath());
    stack_.icons = std::make_unique<IconCache>(stack_.renderer, *platform_, *apps_,
                                               scale > 0.0f ? scale : 1.0f);
    // Stack entries show the shell's own icons; the macOS conversion is only
    // for Dock/Launchpad application tiles.
    stack_.icons->setMacStyle(false);
    for (const auto &[name, target] :
         std::initializer_list<std::pair<const char *, SDL_Texture **>>{
             {"folder_empty.png", &stack_.folderEmpty},
             {"folder_nonempty.png", &stack_.folderNonempty}}) {
        const std::string path = (paths_.assetDir / "icons" / name).string();
        SDL_Surface *surface = IMG_Load(path.c_str());
        if (!surface) {
            continue;
        }
        *target = SDL_CreateTextureFromSurface(stack_.renderer, surface);
        SDL_DestroySurface(surface);
        if (*target) {
            SDL_SetTextureBlendMode(*target, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(*target, SDL_SCALEMODE_LINEAR);
        }
    }
    // The "Open in Finder" arrow belongs to this renderer, not the Dock's.
    auto loadStackPng = [&](const char *name, bool whiteTemplate) -> SDL_Texture * {
        const std::string path = (paths_.assetDir / "icons" / name).string();
        SDL_Surface *raw = IMG_Load(path.c_str());
        if (!raw) {
            return nullptr;
        }
        SDL_Surface *surface = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(raw);
        if (!surface) {
            return nullptr;
        }
        if (whiteTemplate) {
            SDL_LockSurface(surface);
            auto *pixels = static_cast<uint8_t *>(surface->pixels);
            for (int y = 0; y < surface->h; ++y) {
                for (int x = 0; x < surface->w; ++x) {
                    uint8_t *pixel = pixels + static_cast<size_t>(y) * surface->pitch + x * 4;
                    if (pixel[3] > 0) {
                        pixel[0] = 255;
                        pixel[1] = 255;
                        pixel[2] = 255;
                    }
                }
            }
            SDL_UnlockSurface(surface);
        }
        SDL_Texture *texture = SDL_CreateTextureFromSurface(stack_.renderer, surface);
        SDL_DestroySurface(surface);
        if (texture) {
            SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
        }
        return texture;
    };
    openInFinderBlack_ = loadStackPng("open_in_finder.png", false);
    openInFinderWhite_ = loadStackPng("open_in_finder.png", true);
    SDL_HideWindow(stack_.window);
}

void Application::refreshStackEntries() {
    stack_.entries.clear();
    std::error_code ec;
    struct Entry {
        DockItem item;
        std::filesystem::file_time_type modified{};
        std::string kind;
        bool directory = false;
    };
    std::vector<Entry> found;
    for (auto it = std::filesystem::directory_iterator(
             utf8ToPath(stack_.path),
             std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        const std::string name = pathToUtf8(it->path().filename());
        if (name.empty() || isShellMetadataFile(name) ||
            (!settings_.showHidden() && name[0] == '.')) {
            continue;
        }
        Entry entry;
        entry.directory = it->is_directory(ec);
        entry.item.type = entry.directory ? ItemType::Folder : ItemType::File;
        const std::string entryPath = pathToUtf8(it->path());
        entry.item.id =
            std::string(entry.item.type == ItemType::Folder ? "stack-folder:" : "stack-file:") +
            entryPath;
        entry.item.path = entryPath;
        entry.item.label = name;
        if (!entry.directory) {
            entry.kind = lowerAscii(it->path().extension().string());
        }
        std::error_code timeError;
        entry.modified = std::filesystem::last_write_time(it->path(), timeError);
        if (timeError) {
            entry.modified = std::filesystem::file_time_type{};
        }
        found.push_back(std::move(entry));
    }
    const int arrangement = stack_.arrangement;
    std::sort(found.begin(), found.end(), [arrangement](const Entry &a, const Entry &b) {
        // 1 name, 2 date added, 3 date modified, 4 date created, 5 kind.
        // Windows only exposes one reliable date (last write), so the date
        // arrangements all sort newest first like macOS' "Date Added".
        if (arrangement >= 2 && arrangement <= 4) {
            if (a.modified != b.modified) {
                return a.modified > b.modified;
            }
        } else if (arrangement == 5) {
            if (a.kind != b.kind) {
                return a.kind < b.kind;
            }
        }
        return lowerAscii(a.item.label) < lowerAscii(b.item.label);
    });
    if (found.size() > 300) {
        found.resize(300);
    }
    stack_.entries.reserve(found.size());
    for (auto &entry : found) {
        stack_.entries.push_back(std::move(entry.item));
    }
}

void Application::openStack(const DockItem &item, float globalX, float globalY) {
    closeMenu();
    closeGrid();
    createStackWindow();
    if (!stack_.window) {
        return;
    }
    if (stack_.closing) {
        stack_.pendingOpenPath.clear();
        finishClosingStack(false);
    }
    if (stack_.open && stack_.path == item.path) {
        closeStack();
        return;
    }
    if (!stack_.open && stack_.path == item.path &&
        clock_.now() - stack_.dismissedAt < 0.30) {
        // The mouse-up that follows a Dock click arrives after the stack was
        // dismissed; do not re-open it for that same click.
        return;
    }
    stack_.path = item.path;
    stack_.title = item.label.empty() ? pathToUtf8(utf8ToPath(item.path).filename())
                                      : item.label;
    stack_.itemId = item.id;
    stack_.arrangement = item.arrangement;
    stack_.page = 0;
    stack_.hover = -1;
    stack_.hoverAt = clock_.now();
    stackAwayAt_ = 0.0;
    refreshStackEntries();

    // Anchor the stack to the clicked tile instead of the raw cursor position;
    // the cursor may sit anywhere inside a magnified icon.
    float anchorX = globalX;
    float anchorBottom = globalY - 18.0f;
    for (int i = 0; i < static_cast<int>(visibleItems_.size()); ++i) {
        if (visibleItems_[i].id != item.id) {
            continue;
        }
        if (i < static_cast<int>(layoutResult_.tiles.size()) && dockWindow_) {
            int wx = 0;
            int wy = 0;
            SDL_GetWindowPosition(dockWindow_, &wx, &wy);
            const TileGeometry &tile = layoutResult_.tiles[i];
            anchorX = static_cast<float>(wx) + tile.icon.x + tile.icon.w * 0.5f;
            anchorBottom = static_cast<float>(wy) + tile.icon.y + dockHideOffset() - 8.0f;
        }
        break;
    }

    const int count = static_cast<int>(stack_.entries.size());
    const float availableHeight = static_cast<float>(displayBounds_.h) - 140.0f;
    // Include the "Open in Finder" header row when checking whether the fan
    // fits on screen.
    stack_.mode = stack::modeFor(item.showAs, static_cast<size_t>(count + 1), availableHeight);
    if (stackGridMaterial_) {
        SDL_DestroyTexture(stackGridMaterial_);
        stackGridMaterial_ = nullptr;
        stackGridMaterialKey_.clear();
    }

    if (stack_.mode == stack::Mode::Fan) {
        // One extra row at the top is the "Open in Finder" action.
        stack_.fanCount = count + 1;
        stack_.fanItemSize = stack::kFanItemSize;
        stack_.fanSpacing = stack::kFanSpacing;
        const float leftPadding = 20.0f;
        const float rightPadding = 20.0f;
        const float topPadding = 16.0f;
        const float bottomPadding = 8.0f;
        const float labelGap = 10.0f;
        const float curve = stack::fanSagitta(stack_.fanCount, stack_.fanSpacing);
        const float availableLabel = anchorX - stack_.fanItemSize * 0.5f - 40.0f;
        stack_.fanLabelWidth = clampf(availableLabel, 90.0f, stack::kFanLabelWidth);
        stack_.width = leftPadding + stack_.fanLabelWidth + labelGap + stack_.fanItemSize +
                       curve + rightPadding;
        stack_.height = topPadding + static_cast<float>(stack_.fanCount) * stack_.fanSpacing +
                        bottomPadding;
        stack_.fanAnchorX = leftPadding + stack_.fanLabelWidth + labelGap +
                            stack_.fanItemSize * 0.5f;
        stack_.fanBaseY = stack_.height - bottomPadding;
        stack_.iconSize = stack_.fanItemSize;
        stack_.gridRect = SDL_FRect{};
    } else {
        stack_.columns = std::clamp(count, 1, 6);
        stack_.rows = std::clamp((count + stack_.columns - 1) / std::max(1, stack_.columns), 1, 4);
        stack_.iconSize = 52.0f;
        stack_.cellWidth = 86.0f;
        stack_.cellHeight = 92.0f;
        stack_.width = static_cast<float>(stack_.columns) * stack_.cellWidth + 24.0f;
        stack_.height = static_cast<float>(stack_.rows) * stack_.cellHeight + 62.0f;
        stack_.gridRect =
            SDL_FRect{12.0f, 42.0f, stack_.width - 24.0f, stack_.height - 54.0f};
    }

    float x = stack_.mode == stack::Mode::Fan ? anchorX - stack_.fanAnchorX
                                              : anchorX - stack_.width * 0.5f;
    float y = anchorBottom - stack_.height;
    if (y < static_cast<float>(displayBounds_.y + 8)) {
        y = anchorBottom + 24.0f;
    }
    x = clampf(x, static_cast<float>(displayBounds_.x + 8),
               static_cast<float>(displayBounds_.x + displayBounds_.w) - stack_.width - 8.0f);
    y = clampf(y, static_cast<float>(displayBounds_.y + 8),
               static_cast<float>(displayBounds_.y + displayBounds_.h) - stack_.height - 8.0f);
    stack_.x = x;
    stack_.y = y;
    stack_.openedAt = clock_.now();
    SDL_SetWindowSize(stack_.window, static_cast<int>(stack_.width),
                      static_cast<int>(stack_.height));
    SDL_SetWindowPosition(stack_.window, static_cast<int>(x), static_cast<int>(y));
    stack_.open = true;
    // The click that opened the stack may still be down when the outside-click
    // poll runs; start from "down" so it cannot dismiss itself immediately.
    stackWasDown_ = true;
    SDL_SetWindowFocusable(stack_.window, true);
    SDL_ShowWindow(stack_.window);
    SDL_RaiseWindow(stack_.window);
    forceRender_ = true;
    dirty_ = true;
}

void Application::updateStackAnchor() {
    if (!stack_.open || !stack_.window || !dockWindow_) {
        return;
    }
    int index = -1;
    for (int i = 0; i < static_cast<int>(visibleItems_.size()); ++i) {
        if (visibleItems_[i].id == stack_.itemId) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        return;
    }
    // Keep the open fan glued to its Dock tile while neighbouring icons
    // magnify and shift the tile around.
    const bool magnify = settings_.magnification() &&
                         (mouseInside_ || magEnvelope_.value() > 0.001f);
    layoutResult_ = layout_.compute(visibleItems_, settings_, windowWidth_, windowHeight_,
                                    mouseX_, mouseY_, magnify, magEnvelope_.value());
    if (index >= static_cast<int>(layoutResult_.tiles.size())) {
        return;
    }
    int wx = 0;
    int wy = 0;
    SDL_GetWindowPosition(dockWindow_, &wx, &wy);
    const TileGeometry &tile = layoutResult_.tiles[index];
    const float anchorX = static_cast<float>(wx) + tile.icon.x + tile.icon.w * 0.5f;
    const float anchorBottom = static_cast<float>(wy) + tile.icon.y + dockHideOffset() - 8.0f;
    float x = stack_.mode == stack::Mode::Fan ? anchorX - stack_.fanAnchorX
                                              : anchorX - stack_.width * 0.5f;
    float y = anchorBottom - stack_.height;
    if (y < static_cast<float>(displayBounds_.y + 8)) {
        y = anchorBottom + 24.0f;
    }
    x = clampf(x, static_cast<float>(displayBounds_.x + 8),
               static_cast<float>(displayBounds_.x + displayBounds_.w) - stack_.width - 8.0f);
    y = clampf(y, static_cast<float>(displayBounds_.y + 8),
               static_cast<float>(displayBounds_.y + displayBounds_.h) - stack_.height - 8.0f);
    if (std::abs(x - stack_.x) > 0.5f || std::abs(y - stack_.y) > 0.5f) {
        stack_.x = x;
        stack_.y = y;
        SDL_SetWindowPosition(stack_.window, static_cast<int>(std::lround(x)),
                              static_cast<int>(std::lround(y)));
    }
}

void Application::closeStack(bool immediate) {
    if (!stack_.open) {
        return;
    }
    if (immediate) {
        finishClosingStack();
        return;
    }
    if (stack_.closing) {
        return;
    }
    stack_.closing = true;
    stack_.closedAt = clock_.now();
    stack_.hover = -1;
    dirty_ = true;
}

void Application::finishClosingStack(bool launchPending) {
    if (!stack_.open) {
        return;
    }
    stack_.open = false;
    stack_.closing = false;
    stack_.entries.clear();
    stack_.hover = -1;
    stack_.page = 0;
    stack_.fanCount = 0;
    stack_.itemId.clear();
    stack_.pressed = false;
    stack_.dragging = false;
    stack_.dragIndex = -1;
    stackAwayAt_ = 0.0;
    if (stack_.window) {
        SDL_HideWindow(stack_.window);
    }
    const std::string pending = launchPending ? stack_.pendingOpenPath : std::string{};
    stack_.pendingOpenPath.clear();
    if (!pending.empty()) {
        platform_->openPath(pending);
    }
    dirty_ = true;
}

void Application::handleStackEvent(const SDL_Event &event) {
    if (stack_.closing) {
        return;
    }
    if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        closeStack();
        return;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
        closeStack();
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_WHEEL) {
        if (stack_.mode == stack::Mode::Fan) {
            return;
        }
        const int perPage = std::max(1, stack_.columns * stack_.rows);
        const int pageCount =
            std::max(1, (static_cast<int>(stack_.entries.size()) + perPage - 1) / perPage);
        if (event.wheel.y > 0) {
            stack_.page = std::max(0, stack_.page - 1);
        } else {
            stack_.page = std::min(pageCount - 1, stack_.page + 1);
        }
        stack_.hover = -1;
        dirty_ = true;
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        int hover = -1;
        if (stack_.mode == stack::Mode::Fan) {
            hover = stack::fanIndexAt(event.motion.x, event.motion.y, stack_.fanCount,
                                      stack_.fanAnchorX, stack_.fanBaseY,
                                      stack_.fanItemSize, stack_.fanSpacing);
        } else {
            const int perPage = stack_.columns * stack_.rows;
            for (int i = 0; i < perPage; ++i) {
                const int index = stack_.page * perPage + i;
                if (index >= static_cast<int>(stack_.entries.size())) {
                    break;
                }
                const int col = i % stack_.columns;
                const int row = i / stack_.columns;
                SDL_FRect cell{stack_.gridRect.x + static_cast<float>(col) * stack_.cellWidth,
                               stack_.gridRect.y + static_cast<float>(row) * stack_.cellHeight,
                               stack_.cellWidth, stack_.cellHeight};
                if (contains(cell, event.motion.x, event.motion.y)) {
                    hover = index;
                    break;
                }
            }
        }
        if (hover != stack_.hover) {
            stack_.hover = hover;
            stack_.hoverAt = clock_.now();
            dirty_ = true;
        }
        if (stack_.pressed && !stack_.dragging && stack_.dragIndex >= 0 &&
            stack_.dragIndex < static_cast<int>(stack_.entries.size())) {
            if (std::abs(event.motion.x - stack_.dragStartX) > 5.0f ||
                std::abs(event.motion.y - stack_.dragStartY) > 5.0f) {
                fileDragPath_ = stack_.entries[stack_.dragIndex].path;
                fileDrag_ = true;
                stack_.pressed = false;
                stack_.dragging = true;
                stack_.dragIndex = -1;
                dragHoverId_.clear();
                dropFolderId_.clear();
                dragHoverActivated_ = false;
                // Unfold back down while the file is dragged away; the native
                // OLE hand-off waits until this 160 ms collapse has finished.
                closeStack();
                return;
            }
        }
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
        int index = -1;
        if (stack_.mode == stack::Mode::Fan) {
            index = stack::fanIndexAt(event.button.x, event.button.y, stack_.fanCount,
                                      stack_.fanAnchorX, stack_.fanBaseY,
                                      stack_.fanItemSize, stack_.fanSpacing);
        } else {
            const int perPage = stack_.columns * stack_.rows;
            const int col = static_cast<int>((event.button.x - stack_.gridRect.x) / stack_.cellWidth);
            const int row = static_cast<int>((event.button.y - stack_.gridRect.y) / stack_.cellHeight);
            if (col >= 0 && col < stack_.columns && row >= 0 && row < stack_.rows) {
                index = stack_.page * perPage + row * stack_.columns + col;
            }
        }
        stack_.pressed = true;
        stack_.dragging = false;
        stack_.dragIndex = index;
        stack_.dragStartX = event.button.x;
        stack_.dragStartY = event.button.y;
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
        if (stack_.dragging) {
            stack_.dragging = false;
            stack_.pressed = false;
            stack_.dragIndex = -1;
            return;
        }
        if (!stack_.pressed) {
            return;
        }
        stack_.pressed = false;
        int index = -1;
        if (stack_.mode == stack::Mode::Fan) {
            index = stack::fanIndexAt(event.button.x, event.button.y, stack_.fanCount,
                                      stack_.fanAnchorX, stack_.fanBaseY,
                                      stack_.fanItemSize, stack_.fanSpacing);
        } else {
            const int perPage = stack_.columns * stack_.rows;
            const int col = static_cast<int>((event.button.x - stack_.gridRect.x) / stack_.cellWidth);
            const int row = static_cast<int>((event.button.y - stack_.gridRect.y) / stack_.cellHeight);
            if (col >= 0 && col < stack_.columns && row >= 0 && row < stack_.rows) {
                index = stack_.page * perPage + row * stack_.columns + col;
            }
        }
        stack_.dragIndex = -1;
        if (index >= 0 && index < static_cast<int>(stack_.entries.size())) {
            stack_.pendingOpenPath = stack_.entries[index].path;
            closeStack();
        } else if (index == static_cast<int>(stack_.entries.size())) {
            // Top row: open the folder itself (Finder/Explorer).
            stack_.pendingOpenPath = stack_.path;
            closeStack();
        } else {
            // Clicking the transparent gap dismisses the stack.
            closeStack();
        }
    }
}

void Application::renderStack() {
    if (!stack_.open || !stack_.draw || !stack_.text || !stack_.icons) {
        return;
    }
    const bool dark = isDarkTheme(settings_);
    const float openProgress = clampf(static_cast<float>((now_ - stack_.openedAt) / 0.16), 0.0f, 1.0f);
    float fade = easeInOut(openProgress);
    float spread = std::max(0.001f, fade);
    if (stack_.closing) {
        const float t = clampf(static_cast<float>((now_ - stack_.closedAt) / 0.16), 0.0f, 1.0f);
        const float e = easeInOut(t);
        fade = 1.0f - e;
        spread = std::max(0.001f, 1.0f - e);
    }
    stack_.draw->beginFrame();
    stack_.draw->setOpacity(fade);
    stack_.text->setOpacity(fade);

    if (stack_.mode == stack::Mode::Fan) {
        const int files = std::min(stack_.fanCount, static_cast<int>(stack_.entries.size()));
        const int total = files + 1; // + the "Open in Finder" header at the top
        const float itemSize = stack_.fanItemSize;
        const float iconSize = itemSize - 4.0f;
        // Higher items are farther along the arc; during the open animation
        // the spacing grows from zero so the fan unfolds upwards from the tile.
        const SDL_Color pillBorder = dark ? rgba(255, 255, 255, 52) : rgba(0, 0, 0, 45);
        const SDL_Color pillFill = dark ? rgba(24, 24, 26, 232) : rgba(246, 246, 248, 236);
        for (int i = 0; i < total; ++i) {
            const bool header = i == files;
            const stack::FanSlot slot =
                stack::fanSlot(i, total, stack_.fanAnchorX, stack_.fanBaseY, itemSize,
                               stack_.fanSpacing * spread);
            // Only the unfold animation scales icons; hovering a fan item must
            // not grow its icon.
            const float scale = 0.88f + 0.12f * fade;
            const float cx = slot.centerX;
            const float cy = slot.centerY;
            const float angle = slot.angle;
            const float drawIcon = iconSize * scale;
            SDL_FRect icon{cx - drawIcon * 0.5f, cy - drawIcon * 0.5f, drawIcon, drawIcon};

            if (!header) {
                const DockItem &entry = stack_.entries[i];
                // Raw shell icons: no macOS squircle/shadow conversion here.
                SDL_Texture *texture = stack_.icons->textureFor(
                    entry, static_cast<int>(std::lround(iconSize)));
                if (texture) {
                    float textureW = 0.0f;
                    float textureH = 0.0f;
                    SDL_GetTextureSize(texture, &textureW, &textureH);
                    if (textureW > 0.0f && textureH > 0.0f) {
                        // Fit into a common box, preserving each icon's aspect;
                        // small icons are scaled up so none looks oversized.
                        const float fit = std::min(drawIcon / textureW, drawIcon / textureH);
                        SDL_FRect dst{cx - textureW * fit * 0.5f,
                                      cy - textureH * fit * 0.5f, textureW * fit,
                                      textureH * fit};
                        stack_.draw->texture(texture, dst, rgba(255, 255, 255, 255), angle,
                                             SDL_FPoint{dst.w * 0.5f, dst.h * 0.5f});
                    } else {
                        stack_.draw->texture(texture, icon, rgba(255, 255, 255, 255), angle,
                                             SDL_FPoint{icon.w * 0.5f, icon.h * 0.5f});
                    }
                } else {
                    stack_.draw->placeholderIcon(icon, rgba(70, 72, 84, 245),
                                                 rgba(110, 114, 132, 245));
                }
            } else {
                // "Open in Finder": the macOS arrow resource, no plate.
                SDL_Texture *arrow = dark ? openInFinderWhite_ : openInFinderBlack_;
                if (arrow) {
                    float textureW = 0.0f;
                    float textureH = 0.0f;
                    SDL_GetTextureSize(arrow, &textureW, &textureH);
                    // The artwork has ~36% transparent padding; scale it so
                    // the visible circle matches a file icon's box.
                    const float box = drawIcon / 0.64f;
                    const float fit = std::min(box / textureW, box / textureH);
                    SDL_FRect dst{cx - textureW * fit * 0.5f, cy - textureH * fit * 0.5f,
                                  textureW * fit, textureH * fit};
                    stack_.draw->texture(arrow, dst);
                } else {
                    const float arrowW = icon.w * 0.42f;
                    const float arrowH = icon.w * 0.28f;
                    const float thick = std::max(2.0f, icon.w * 0.055f);
                    const SDL_Color shadow = rgba(0, 0, 0, dark ? 150 : 90);
                    const SDL_Color ink = primaryText(dark);
                    for (int pass = 0; pass < 2; ++pass) {
                        const SDL_Color color = pass == 0 ? shadow : ink;
                        const float offset = pass == 0 ? 1.0f : 0.0f;
                        stack_.draw->thickLine(cx - arrowW * 0.5f + offset, cy + offset,
                                               cx + arrowW * 0.5f + offset, cy + offset,
                                               thick, color);
                        stack_.draw->thickLine(
                            cx + arrowW * 0.5f + offset, cy + offset,
                            cx + arrowW * 0.10f + offset, cy - arrowH * 0.5f + offset,
                            thick, color);
                        stack_.draw->thickLine(
                            cx + arrowW * 0.5f + offset, cy + offset,
                            cx + arrowW * 0.10f + offset, cy + arrowH * 0.5f + offset,
                            thick, color);
                    }
                }
            }

            // File names sit to the left of the icons *along the fan tangent*,
            // so their rotated right edge meets the icon centre instead of
            // drifting below it.
            const char *label = header ? "Open in Finder" : stack_.entries[i].label.c_str();
            const SDL_FPoint textSize = stack_.text->measure(label, 12.0f);
            // Generous padding keeps the 8 px corners from crowding the text
            // at narrow widths (a fixed-width stretched texture used to make
            // short labels look like capsules).
            float pillWidth = std::min(stack_.fanLabelWidth, textSize.x + 24.0f);
            if (pillWidth < 12.0f) {
                continue;
            }
            const float pillHeight = 24.0f;
            const float angleRad = angle * kPi / 180.0f;
            const float cosA = std::cos(angleRad);
            const float sinA = std::sin(angleRad);
            const float gap = 10.0f;
            float along = drawIcon * 0.5f + gap + pillWidth * 0.5f;
            float centerX = cx - along * cosA;
            float pillX = centerX - pillWidth * 0.5f;
            if (pillX < 6.0f) {
                // Keep the right edge attached and shorten from the left.
                const float attachX = cx - (drawIcon * 0.5f + gap) * cosA;
                pillX = 6.0f;
                pillWidth = std::max(12.0f, attachX - pillX);
                centerX = pillX + pillWidth * 0.5f;
                along = (cx - centerX) / std::max(0.2f, cosA);
            }
            const float centerY = cy - along * sinA;
            SDL_FRect pill{pillX, centerY - pillHeight * 0.5f, pillWidth, pillHeight};
            const int pillKey = std::max(1, static_cast<int>(std::lround(pillWidth)));
            SDL_Texture *pillTexture = nullptr;
            auto pillFound = stackPills_.find(pillKey);
            if (pillFound != stackPills_.end()) {
                pillTexture = pillFound->second;
            } else if (SDL_Surface *surface = makePillSurface(pillKey, 24, 8.0f)) {
                pillTexture = SDL_CreateTextureFromSurface(stack_.renderer, surface);
                SDL_DestroySurface(surface);
                if (pillTexture) {
                    SDL_SetTextureBlendMode(pillTexture, SDL_BLENDMODE_BLEND);
                }
                stackPills_[pillKey] = pillTexture;
            }
            if (pillTexture) {
                stack_.draw->texture(pillTexture, pill, pillBorder, angle,
                                     SDL_FPoint{pill.w * 0.5f, pill.h * 0.5f});
                const SDL_FRect inner = insetRect(pill, 1.0f, 1.0f);
                stack_.draw->texture(pillTexture, inner, pillFill, angle,
                                     SDL_FPoint{inner.w * 0.5f, inner.h * 0.5f});
            } else {
                stack_.draw->roundedRect(pill, 8.0f, pillFill);
            }
            // Centre the glyph inside the pill (the font's ink sits slightly
            // below the texture's geometric centre, hence the tiny nudge).
            const SDL_FRect textBox{pill.x + 12.0f, pill.y - 1.5f, pill.w - 24.0f,
                                    pill.h};
            // No drop shadow: an offset copy made the name look a pixel low
            // compared with the icon.
            stack_.text->drawTextEllipsizedRotated(label, textBox, 12.0f,
                                                  primaryText(dark),
                                                  angle);
        }
        if (files == 0) {
            stack_.text->drawText("Empty folder", stack_.width * 0.5f - 40.0f,
                                  stack_.height * 0.5f - 8.0f, 13.0f, secondaryText(dark));
        }
        stack_.draw->setOpacity(1.0f);
        stack_.text->setOpacity(1.0f);
        stack_.draw->endFrame();
        return;
    }

    stack_.draw->shadow(SDL_FRect{4, 4, stack_.width - 8, stack_.height - 8}, 9.0f,
                        rgba(0, 0, 0, 150), 10.0f);
    // Keep an opaque panel under the capture, exactly like the context menus.
    stack_.draw->roundedRect(SDL_FRect{0, 0, stack_.width, stack_.height}, 9.0f,
                             dark ? rgba(30, 30, 36, 246)
                                  : rgba(248, 248, 251, 248));
    const std::string gridKey = std::to_string(static_cast<int>(stack_.width)) + "x" +
                                std::to_string(static_cast<int>(stack_.height)) +
                                (dark ? "d" : "l");
    if (!stackGridMaterial_ || stackGridMaterialKey_ != gridKey) {
        if (stackGridMaterial_) {
            SDL_DestroyTexture(stackGridMaterial_);
            stackGridMaterial_ = nullptr;
        }
        // Same frosted material as the right-click menus (8 px blur, radius 9).
        stackGridMaterial_ = buildPopoverTexture(
            stack_.window, stack_.renderer, 0.0f, 0.0f, stack_.width, stack_.height, 9.0f,
            4.0f, 0.0f, 0.0f, dark, true, false, 8.0f);
        stackGridMaterialKey_ = gridKey;
    }
    if (stackGridMaterial_) {
        stack_.draw->texture(stackGridMaterial_,
                             SDL_FRect{0.0f, 0.0f, stack_.width, stack_.height});
    }
    stack_.text->drawTextEllipsized(stack_.title, SDL_FRect{16, 10, stack_.width - 32, 24},
                                    15.0f, primaryText(dark));

    const int perPage = std::max(1, stack_.columns * stack_.rows);
    const int pageCount = std::max(1, (static_cast<int>(stack_.entries.size()) + perPage - 1) / perPage);
    stack_.page = std::clamp(stack_.page, 0, pageCount - 1);
    const int begin = stack_.page * perPage;
    const int total = static_cast<int>(stack_.entries.size());
    const float iconSize = stack_.iconSize;
    for (int i = 0; i < perPage; ++i) {
        const int index = begin + i;
        if (index >= total) {
            break;
        }
        const int col = i % stack_.columns;
        const int row = i / stack_.columns;
        const float cx = stack_.gridRect.x + (static_cast<float>(col) + 0.5f) * stack_.cellWidth;
        const float cy = stack_.gridRect.y + static_cast<float>(row) * stack_.cellHeight +
                         iconSize * 0.5f;
        SDL_FRect icon{cx - iconSize * 0.5f, cy - iconSize * 0.5f, iconSize, iconSize};
        const DockItem &entry = stack_.entries[index];
        SDL_Texture *texture = entry.type == ItemType::Folder
                                   ? stack_.folderNonempty
                                   : stack_.icons->textureFor(
                                         entry, static_cast<int>(iconSize));
        if (texture) {
            // Preserve the shell artwork's aspect ratio, exactly like the fan.
            float tw = 0.0f;
            float th = 0.0f;
            SDL_GetTextureSize(texture, &tw, &th);
            if (tw > 0.0f && th > 0.0f) {
                const float fit = std::min(icon.w / tw, icon.h / th);
                const SDL_FRect dst{icon.x + (icon.w - tw * fit) * 0.5f,
                                    icon.y + (icon.h - th * fit) * 0.5f,
                                    tw * fit, th * fit};
                stack_.draw->texture(texture, dst);
            } else {
                stack_.draw->texture(texture, icon);
            }
        } else if (entry.type == ItemType::Folder) {
            stack_.draw->folderIcon(icon, rgba(80, 160, 235, 245), rgba(110, 185, 250, 245));
        } else {
            stack_.draw->placeholderIcon(icon, rgba(70, 72, 84, 245), rgba(110, 114, 132, 245));
        }
        stack_.text->drawTextEllipsized(entry.label,
                                       SDL_FRect{cx - stack_.cellWidth * 0.5f,
                                                 cy + iconSize * 0.5f + 5.0f,
                                                 stack_.cellWidth, 18.0f},
                                       11.0f, secondaryText(dark));
    }

    if (total == 0) {
        stack_.text->drawText("Empty folder", stack_.width * 0.5f - 40.0f,
                              stack_.height * 0.5f - 8.0f, 13.0f, secondaryText(dark));
    }
    if (pageCount > 1) {
        const float dotsWidth = static_cast<float>(pageCount) * 14.0f;
        for (int i = 0; i < pageCount; ++i) {
            SDL_FRect dot{stack_.width * 0.5f - dotsWidth * 0.5f + static_cast<float>(i) * 14.0f + 3.0f,
                          stack_.height - 16.0f, 6.0f, 6.0f};
            stack_.draw->circle(dot, i == stack_.page ? primaryText(dark)
                                                      : secondaryText(dark));
        }
    }
    stack_.draw->setOpacity(1.0f);
    stack_.text->setOpacity(1.0f);
    stack_.draw->endFrame();
}

void Application::ensureMinimizeWindow() {
    if (minimize_.window) {
        return;
    }
    minimize_.window = SDL_CreateWindow(
        "RevDock Minimize", 8, 8,
        static_cast<SDL_WindowFlags>(SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP |
                                     SDL_WINDOW_TRANSPARENT | SDL_WINDOW_UTILITY |
                                     SDL_WINDOW_HIGH_PIXEL_DENSITY));
    if (!minimize_.window) {
        RD_LOG_WARN("Minimize window creation failed: %s", SDL_GetError());
        return;
    }
    minimize_.renderer = SDL_CreateRenderer(minimize_.window, nullptr);
    if (!minimize_.renderer) {
        SDL_DestroyWindow(minimize_.window);
        minimize_.window = nullptr;
        return;
    }
    SDL_SetRenderVSync(minimize_.renderer, 1);
    SDL_SetRenderDrawBlendMode(minimize_.renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderScale(minimize_.renderer, contentScale_, contentScale_);
    minimize_.draw = std::make_unique<Renderer>(minimize_.renderer, contentScale_);
    SDL_SetWindowFocusable(minimize_.window, false);
    platform_->setClickThrough(minimize_.window, true, true);
    SDL_HideWindow(minimize_.window);
}

void Application::startMinimizeAnimation(const std::string &execPath, const std::string &aumid,
                                         std::uintptr_t handle, const std::string &title,
                                         const SDL_Rect &rect, const std::vector<uint8_t> &rgba,
                                         int width, int height) {
    if (rgba.empty() || width <= 0 || height <= 0 || rect.w <= 0 || rect.h <= 0) {
        return;
    }
    if (!apps_ || !dockWindow_ || settings_.reduceMotion()) {
        return;
    }
    if (autohide_.value() > 0.01f) {
        // A minimized window must still animate when the Dock is auto-hidden:
        // reveal it quickly and aim the Genie at the final on-screen tile.
        autohide_.setTarget(clock_.now(), 0.0f, 0.12);
        autohideHidden_ = false;
        autohideDeadline_ = clock_.now() + 1.0;
        dirty_ = true;
    }
    const AppInfo *app = apps_->findByPath(execPath);
    if (!app && !aumid.empty()) {
        app = apps_->findByAumid(aumid);
    }

    // Insert the minimized-window tile immediately so the Dock makes room for
    // it; the Genie animation then targets that new tile (between its two
    // neighbours) instead of the application icon.
    if (handle != 0) {
        if (windowThumbnails_.count(handle) == 0 && !rgba.empty() && width > 0 &&
            height > 0) {
            SDL_Texture *texture =
                SDL_CreateTexture(dockRenderer_, SDL_PIXELFORMAT_RGBA32,
                                  SDL_TEXTUREACCESS_STATIC, width, height);
            if (texture) {
                SDL_UpdateTexture(texture, nullptr, rgba.data(), width * 4);
                SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
                windowThumbnails_[handle] = texture;
            }
        }
        pendingMinimized_ = DockItem{};
        pendingMinimized_.type = ItemType::MinimizedWindow;
        pendingMinimized_.id = "window:" + std::to_string(handle);
        pendingMinimized_.path = execPath;
        pendingMinimized_.label = title.empty() ? (app ? app->name : std::string{}) : title;
        pendingMinimized_.windowHandle = handle;
        pendingMinimized_.pinned = false;
        if (width > 0 && height > 0) {
            pendingMinimized_.previewAspect =
                static_cast<float>(width) / static_cast<float>(height);
        }
        hasPendingMinimized_ = true;
        pendingMinimizedAt_ = clock_.now();
    }

    rebuildVisibleItems();
    relayoutWindow();
    layoutResult_ = layout_.compute(layoutItems(), settings_, windowWidth_, windowHeight_,
                                    mouseX_, mouseY_, mouseInside_, magEnvelope_.value());

    int index = -1;
    const std::string tileId = "window:" + std::to_string(handle);
    for (int i = 0; i < static_cast<int>(visibleItems_.size()); ++i) {
        if ((handle != 0 && visibleItems_[i].id == tileId) ||
            (handle == 0 && app && visibleItems_[i].id == app->id)) {
            index = i;
            break;
        }
    }
    if (index < 0 || index >= static_cast<int>(layoutResult_.tiles.size())) {
        RD_LOG_DEBUG("minimize anim: no target tile for %s", execPath.c_str());
        return;
    }
    RD_LOG_DEBUG("minimize anim start t=%llu %s from %d,%d %dx%d",
                 static_cast<unsigned long long>(GetTickCount64()), execPath.c_str(), rect.x,
                 rect.y, rect.w, rect.h);
    ensureMinimizeWindow();
    if (!minimize_.window || !minimize_.renderer || !minimize_.draw) {
        return;
    }
    int wx = 0;
    int wy = 0;
    SDL_GetWindowPosition(dockWindow_, &wx, &wy);
    const TileGeometry &tile = layoutResult_.tiles[index];
    const float previewAspect = visibleItems_[index].previewAspect;
    const SDL_FRect preview = (handle != 0 || windowThumbnails_.count(handle) > 0)
                                  ? minimizedPreviewRect(tile.icon, previewAspect)
                                  : tile.icon;
    // The Dock is revealed for this animation, so aim at its final on-screen
    // position rather than the current (possibly still hidden) offset.
    const SDL_FRect target{static_cast<float>(wx) + preview.x,
                           static_cast<float>(wy) + preview.y, preview.w, preview.h};
    RD_LOG_DEBUG("minimize anim target %s at %.0f,%.0f %.0fx%.0f",
                 visibleItems_[index].id.c_str(), target.x, target.y, target.w, target.h);
    // A full-screen transparent overlay keeps the coordinate mapping simple
    // and stays above the Dock/menus regardless of where the window was.
    const float originX = static_cast<float>(displayBounds_.x);
    const float originY = static_cast<float>(displayBounds_.y);
    const int windowW = std::max(1, displayBounds_.w);
    const int windowH = std::max(1, displayBounds_.h);
    SDL_SetWindowPosition(minimize_.window, displayBounds_.x, displayBounds_.y);
    SDL_SetWindowSize(minimize_.window, windowW, windowH);
    if (minimize_.texture) {
        SDL_DestroyTexture(minimize_.texture);
        minimize_.texture = nullptr;
    }
    minimize_.texture = SDL_CreateTexture(minimize_.renderer, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STATIC, width, height);
    if (!minimize_.texture) {
        return;
    }
    SDL_UpdateTexture(minimize_.texture, nullptr, rgba.data(), width * 4);
    SDL_SetTextureBlendMode(minimize_.texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(minimize_.texture, SDL_SCALEMODE_LINEAR);
    minimize_.duration = settings_.mineffect() == "genie" ? 0.62 : 0.32;
    minimize_.from = SDL_FRect{static_cast<float>(rect.x) - originX,
                              static_cast<float>(rect.y) - originY,
                              static_cast<float>(rect.w), static_cast<float>(rect.h)};
    minimize_.to = SDL_FRect{target.x - originX, target.y - originY, target.w, target.h};
    minimize_.start = clock_.now();
    minimize_.active = true;
    // Show first, then present the first frame synchronously: SDL_SyncWindow
    // only waits for a present while the window is visible, and that present
    // is what closes the flash gap after the real window disappears.
    SDL_ShowWindow(minimize_.window);
    SDL_SetWindowAlwaysOnTop(minimize_.window, true);
    SDL_RaiseWindow(minimize_.window);
    platform_->raiseAlwaysOnTop(minimize_.window);
    renderMinimizeAnimation();
    SDL_SyncWindow(minimize_.window);
    forceRender_ = true;
}

void Application::restoreMinimizedItem(const DockItem &item) {
    if (item.type != ItemType::MinimizedWindow || item.windowHandle == 0) {
        return;
    }
    const std::uintptr_t handle = item.windowHandle;
    SDL_Rect normal{};
    if (!platform_ || settings_.reduceMotion() ||
        !platform_->windowRestoreRect(handle, normal)) {
        platform_->restoreWindow(handle);
        rebuildVisibleItems();
        relayoutWindow();
        return;
    }
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
    for (const auto &window : platform_->minimizedWindows()) {
        if (window.handle == handle) {
            rgba = window.rgba;
            width = window.width;
            height = window.height;
            break;
        }
    }
    if (rgba.empty() || width <= 0 || height <= 0) {
        platform_->restoreWindow(handle);
        rebuildVisibleItems();
        relayoutWindow();
        return;
    }
    startRestoreAnimation(item, handle, normal, rgba, width, height);
}

void Application::startRestoreAnimation(const DockItem &item, std::uintptr_t handle,
                                        const SDL_Rect &normalRect,
                                        const std::vector<uint8_t> &rgba, int width,
                                        int height) {
    if (!dockWindow_ || rgba.empty() || width <= 0 || height <= 0 || normalRect.w <= 0 ||
        normalRect.h <= 0) {
        platform_->restoreWindow(handle);
        rebuildVisibleItems();
        relayoutWindow();
        return;
    }
    int index = -1;
    for (int i = 0; i < static_cast<int>(visibleItems_.size()); ++i) {
        if (visibleItems_[i].windowHandle == handle) {
            index = i;
            break;
        }
    }
    if (index < 0 || index >= static_cast<int>(layoutResult_.tiles.size())) {
        platform_->restoreWindow(handle);
        rebuildVisibleItems();
        relayoutWindow();
        return;
    }
    ensureMinimizeWindow();
    if (!minimize_.window || !minimize_.renderer || !minimize_.draw) {
        platform_->restoreWindow(handle);
        return;
    }
    int wx = 0;
    int wy = 0;
    SDL_GetWindowPosition(dockWindow_, &wx, &wy);
    const TileGeometry &tile = layoutResult_.tiles[index];
    const SDL_FRect preview = minimizedPreviewRect(tile.icon, item.previewAspect);
    const float originX = static_cast<float>(displayBounds_.x);
    const float originY = static_cast<float>(displayBounds_.y);
    SDL_SetWindowPosition(minimize_.window, displayBounds_.x, displayBounds_.y);
    SDL_SetWindowSize(minimize_.window, std::max(1, displayBounds_.w),
                      std::max(1, displayBounds_.h));
    if (minimize_.texture) {
        SDL_DestroyTexture(minimize_.texture);
        minimize_.texture = nullptr;
    }
    minimize_.texture = SDL_CreateTexture(minimize_.renderer, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STATIC, width, height);
    if (!minimize_.texture) {
        platform_->restoreWindow(handle);
        return;
    }
    SDL_UpdateTexture(minimize_.texture, nullptr, rgba.data(), width * 4);
    SDL_SetTextureBlendMode(minimize_.texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(minimize_.texture, SDL_SCALEMODE_LINEAR);
    minimize_.from = SDL_FRect{
        static_cast<float>(wx) + preview.x - originX,
        static_cast<float>(wy) + preview.y + dockHideOffset() - originY, preview.w, preview.h};
    minimize_.to = SDL_FRect{static_cast<float>(normalRect.x) - originX,
                             static_cast<float>(normalRect.y) - originY,
                             static_cast<float>(normalRect.w), static_cast<float>(normalRect.h)};
    minimize_.duration = settings_.mineffect() == "genie" ? 0.50 : 0.28;
    minimize_.start = clock_.now();
    minimize_.restoring = true;
    minimize_.restoreHandle = handle;
    minimize_.active = true;
    // Pop the tile out of the bar right away; the Genie now owns the visual
    // and neighbouring icons close the gap like on macOS.
    restoringHandle_ = handle;
    rebuildVisibleItems();
    relayoutWindow();
    SDL_ShowWindow(minimize_.window);
    SDL_SetWindowAlwaysOnTop(minimize_.window, true);
    SDL_RaiseWindow(minimize_.window);
    platform_->raiseAlwaysOnTop(minimize_.window);
    renderMinimizeAnimation();
    SDL_SyncWindow(minimize_.window);
    forceRender_ = true;
}

void Application::renderMinimizeAnimation() {
    if (!minimize_.active || !minimize_.draw || !minimize_.texture) {
        return;
    }
    const double t = clampd((now_ - minimize_.start) / minimize_.duration, 0.0, 1.0);
    const float e = easeInOut(static_cast<float>(t));
    float alpha = 1.0f;
    if (!minimize_.restoring && t > 0.85) {
        alpha = 1.0f - static_cast<float>((t - 0.85) / 0.15);
    }
    minimize_.draw->beginFrame();
    if (settings_.mineffect() == "genie") {
        // Genie: a grid of vertices is pulled into the Dock tile. Higher rows
        // lag behind, so the window stretches into a funnel before it is
        // swallowed by the icon; a mid-animation pinch narrows the neck.
        const SDL_FRect &from = minimize_.from;
        const SDL_FRect &to = minimize_.to;
        const int cols = std::clamp(static_cast<int>(std::lround(from.w / 70.0f)), 1, 19);
        const int rows = std::clamp(static_cast<int>(std::lround(from.h / 70.0f)), 1, 19);
        const float targetCenterX = to.x + to.w * 0.5f;
        constexpr float kGeniePi = 3.14159265358979323846f;
        const float pinchPhase = std::sin(kGeniePi * e);
        std::vector<SDL_Vertex> vertices;
        vertices.reserve(static_cast<size_t>(cols + 1) * (rows + 1));
        std::vector<int> indices;
        indices.reserve(static_cast<size_t>(cols) * rows * 6);
        for (int row = 0; row <= rows; ++row) {
            const float v = static_cast<float>(row) / static_cast<float>(rows);
            // Top rows start collapsing later, producing the long genie tail.
            const float lag = 0.26f * (1.0f - v);
            const float rowProgress =
                std::clamp((e - lag) / std::max(0.001f, 1.0f - lag), 0.0f, 1.0f);
            const float rowEase = easeInOut(rowProgress);
            const float pinch = 0.16f * pinchPhase * (0.30f + 0.70f * v);
            for (int col = 0; col <= cols; ++col) {
                const float u = static_cast<float>(col) / static_cast<float>(cols);
                const float sourceX = from.x + u * from.w;
                const float sourceY = from.y + v * from.h;
                float px = lerpf(sourceX, to.x + u * to.w, rowEase);
                const float py = lerpf(sourceY, to.y + v * to.h, rowEase);
                px += (targetCenterX - px) * pinch;
                SDL_Vertex vertex;
                vertex.position = SDL_FPoint{px, py};
                vertex.color = SDL_FColor{1.0f, 1.0f, 1.0f, alpha};
                vertex.tex_coord = SDL_FPoint{u, v};
                vertices.push_back(vertex);
            }
        }
        for (int row = 0; row < rows; ++row) {
            for (int col = 0; col < cols; ++col) {
                const int topLeft = row * (cols + 1) + col;
                const int topRight = topLeft + 1;
                const int bottomLeft = topLeft + (cols + 1);
                const int bottomRight = bottomLeft + 1;
                indices.push_back(topLeft);
                indices.push_back(topRight);
                indices.push_back(bottomLeft);
                indices.push_back(topRight);
                indices.push_back(bottomRight);
                indices.push_back(bottomLeft);
            }
        }
        SDL_SetTextureBlendMode(minimize_.texture, SDL_BLENDMODE_BLEND);
        SDL_RenderGeometry(minimize_.renderer, minimize_.texture, vertices.data(),
                           static_cast<int>(vertices.size()), indices.data(),
                           static_cast<int>(indices.size()));
    } else {
        SDL_FRect rect{lerpf(minimize_.from.x, minimize_.to.x, e),
                       lerpf(minimize_.from.y, minimize_.to.y, e),
                       lerpf(minimize_.from.w, minimize_.to.w, e),
                       lerpf(minimize_.from.h, minimize_.to.h, e)};
        minimize_.draw->texture(minimize_.texture, rect,
                                rgba(255, 255, 255, static_cast<uint8_t>(alpha * 255.0f)));
    }
    minimize_.draw->endFrame();
    if (t >= 1.0) {
        finishMinimizeAnimation();
    }
}

void Application::finishMinimizeAnimation() {
    const bool restoring = minimize_.restoring;
    const std::uintptr_t handle = minimize_.restoreHandle;
    minimize_.active = false;
    minimize_.restoring = false;
    minimize_.restoreHandle = 0;
    if (minimize_.texture) {
        SDL_DestroyTexture(minimize_.texture);
        minimize_.texture = nullptr;
    }
    if (restoring && handle != 0) {
        if (hasPendingMinimized_ && pendingMinimized_.windowHandle == handle) {
            hasPendingMinimized_ = false;
        }
        // Bring the real window back while the Genie still covers its final
        // rectangle; hiding the sheet first would expose the desktop for a
        // frame. Keep skipping the tile until the platform stops reporting the
        // window as minimized, so adjacent icons do not pop back.
        platform_->restoreWindow(handle);
        rebuildVisibleItems();
        relayoutWindow();
        restoringClearAt_ = now_ + 3.0;
    }
    if (minimize_.window && minimize_.draw) {
        // Clear the sheet while it is still visible, then hide it so an idle
        // full-screen overlay can never intercept input.
        minimize_.draw->beginFrame();
        minimize_.draw->endFrame();
        SDL_SyncWindow(minimize_.window);
        SDL_HideWindow(minimize_.window);
    }
    dirty_ = true;
}

void Application::createSettingsWindow() {
    if (settingsUi_.window) {
        return;
    }
    settingsUi_.window = SDL_CreateWindow("RevDock Settings", 820, 860,
        static_cast<SDL_WindowFlags>(SDL_WINDOW_RESIZABLE | SDL_WINDOW_UTILITY |
                                     SDL_WINDOW_HIGH_PIXEL_DENSITY));
    if (!settingsUi_.window) {
        RD_LOG_WARN("Settings window creation failed: %s", SDL_GetError());
        return;
    }
    settingsUi_.renderer = SDL_CreateRenderer(settingsUi_.window, nullptr);
    if (!settingsUi_.renderer) {
        SDL_DestroyWindow(settingsUi_.window);
        settingsUi_.window = nullptr;
        return;
    }
    SDL_SetRenderVSync(settingsUi_.renderer, 1);
    SDL_SetRenderDrawBlendMode(settingsUi_.renderer, SDL_BLENDMODE_BLEND);
    const float scale = SDL_GetWindowDisplayScale(settingsUi_.window);
    SDL_SetRenderScale(settingsUi_.renderer, scale > 0.0f ? scale : 1.0f,
                       scale > 0.0f ? scale : 1.0f);
    settingsUi_.draw = std::make_unique<Renderer>(settingsUi_.renderer,
                                                   scale > 0.0f ? scale : 1.0f);
    const std::filesystem::path regular = paths_.assetDir / "fonts" / "Inter" / "Inter-Regular.ttf";
    settingsUi_.text = std::make_unique<TextRenderer>(settingsUi_.renderer, regular.string(),
                                                      fallbackFontPath());
    SDL_SetWindowPosition(settingsUi_.window,
                          displayBounds_.x + (displayBounds_.w - 820) / 2,
                          displayBounds_.y + (displayBounds_.h - 860) / 2);
    SDL_SetWindowMinimumSize(settingsUi_.window, 760, 830);
    SDL_HideWindow(settingsUi_.window);
}

void Application::openSettings() {
    createSettingsWindow();
    if (!settingsUi_.window) {
        return;
    }
    closeMenu();
    closeGrid();
    closeStack(true);
    settingsUi_.open = true;
    SDL_SetWindowFocusable(settingsUi_.window, true);
    SDL_ShowWindow(settingsUi_.window);
    SDL_RaiseWindow(settingsUi_.window);
    platform_->focusWindow(settingsUi_.window);
    forceRender_ = true;
}

void Application::closeSettings() {
    if (!settingsUi_.open) {
        return;
    }
    settingsUi_.open = false;
    if (settingsUi_.window) {
        SDL_HideWindow(settingsUi_.window);
    }
    dirty_ = true;
}

void Application::handleSettingsEvent(const SDL_Event &event) {
    if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        closeSettings();
        return;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
        closeSettings();
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        settingsUi_.hover = -1;
        for (size_t i = 0; i < settingsUi_.hits.size(); ++i) {
            if (contains(settingsUi_.hits[i].rect, event.motion.x, event.motion.y)) {
                settingsUi_.hover = static_cast<int>(i);
                break;
            }
        }
        if (settingsUi_.activeSlider >= 0 &&
            settingsUi_.activeSlider < static_cast<int>(settingsUi_.hits.size())) {
            if (settingsUi_.hits[settingsUi_.activeSlider].id.find("slider") == 0 ||
                settingsUi_.hits[settingsUi_.activeSlider].id == "tile-size" ||
                settingsUi_.hits[settingsUi_.activeSlider].id == "large-size" ||
                settingsUi_.hits[settingsUi_.activeSlider].id == "autohide-delay" ||
                settingsUi_.hits[settingsUi_.activeSlider].id == "autohide-speed") {
                applySettingsControl(settingsUi_.hits[settingsUi_.activeSlider].id,
                                     event.motion.x, true);
            }
        }
        dirty_ = true;
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
        settingsUi_.activeSlider = -1;
        for (size_t i = 0; i < settingsUi_.hits.size(); ++i) {
            const auto &hit = settingsUi_.hits[i];
            if (!contains(hit.rect, event.button.x, event.button.y)) {
                continue;
            }
            if (hit.id == "tile-size" || hit.id == "large-size" ||
                hit.id == "autohide-delay" || hit.id == "autohide-speed") {
                settingsUi_.activeSlider = static_cast<int>(i);
            }
            applySettingsControl(hit.id, event.button.x, true);
            dirty_ = true;
            break;
        }
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        settingsUi_.activeSlider = -1;
        dirty_ = true;
    }
}

void Application::applySettingsControl(const std::string &id, float x, bool pressed) {
    (void)pressed;
    if (id == "magnification") {
        settings_.setMagnification(!settings_.magnification());
        markConfigDirty(now_);
        relayoutWindow();
        return;
    }
    if (id == "autohide") {
        settings_.setAutohide(!settings_.autohide());
        autohideDeadline_ = now_ + settings_.autohideDelay();
        markConfigDirty(now_);
        return;
    }
    if (id == "labels") {
        settings_.setShowLabels(!settings_.showLabels());
        markConfigDirty(now_);
        return;
    }
    if (id == "indicators") {
        settings_.setShowIndicators(!settings_.showIndicators());
        markConfigDirty(now_);
        return;
    }
    if (id == "theme-dark") {
        settings_.setTheme("dark");
        markConfigDirty(now_);
        return;
    }
    if (id == "theme-light") {
        settings_.setTheme("light");
        markConfigDirty(now_);
        return;
    }
    if (id == "theme-system") {
        settings_.setTheme("system");
        markConfigDirty(now_);
        return;
    }
    if (id == "pos-bottom" || id == "pos-left" || id == "pos-right") {
        settings_.setOrientation(id.substr(4));
        markConfigDirty(now_);
        relayoutWindow();
        return;
    }
    if (id == "reset") {
        settings_.resetDefaults();
        markConfigDirty(now_);
        relayoutWindow();
        return;
    }
    if (id == "open-config") {
        platform_->openPath(pathToUtf8(paths_.configDir));
        return;
    }
    if (id == "close") {
        closeSettings();
        return;
    }

    auto sliderRect = [&](const std::string &wanted) -> SDL_FRect {
        for (const auto &hit : settingsUi_.hits) {
            if (hit.id == wanted) {
                return hit.rect;
            }
        }
        return SDL_FRect{};
    };
    auto t = [&](const std::string &wanted) {
        const SDL_FRect rect = sliderRect(wanted);
        return rect.w > 0.0f ? clampf((x - rect.x) / rect.w, 0.0f, 1.0f) : 0.0f;
    };
    if (id == "tile-size") {
        settings_.setTileSize(16.0f + t(id) * 112.0f);
        markConfigDirty(now_);
        relayoutWindow();
    } else if (id == "large-size") {
        settings_.setLargeSize(settings_.tileSize() + t(id) * (192.0f - settings_.tileSize()));
        markConfigDirty(now_);
        relayoutWindow();
    } else if (id == "autohide-delay") {
        settings_.setAutohideDelay(t(id) * 2.0f);
        markConfigDirty(now_);
    } else if (id == "autohide-speed") {
        settings_.setAutohideTimeModifier(0.2f + t(id) * 2.8f);
        markConfigDirty(now_);
    }
}

void Application::renderSettings() {
    if (!settingsUi_.draw || !settingsUi_.text) {
        return;
    }
    int width = 820;
    int height = 860;
    SDL_GetWindowSize(settingsUi_.window, &width, &height);
    const bool dark = isDarkTheme(settings_);
    settingsUi_.hits.clear();
    settingsUi_.draw->beginFrame();

    const SDL_Color accent = rgba(64, 132, 255, 255);
    const SDL_Color accentHover = rgba(88, 150, 255, 255);
    const SDL_Color background =
        dark ? rgba(22, 22, 28, 255) : rgba(240, 241, 246, 255);
    const SDL_Color headerBackground =
        dark ? rgba(29, 29, 36, 255) : rgba(250, 250, 253, 255);
    const SDL_Color cardBackground =
        dark ? rgba(36, 36, 45, 255) : rgba(255, 255, 255, 255);
    const SDL_Color cardBorder =
        dark ? rgba(255, 255, 255, 18) : rgba(16, 20, 30, 14);
    const SDL_Color trackColor =
        dark ? rgba(255, 255, 255, 30) : rgba(16, 20, 30, 24);
    const SDL_Color hairline =
        dark ? rgba(255, 255, 255, 18) : rgba(16, 20, 30, 16);

    const float headerHeight = 88.0f;
    const float footerHeight = 72.0f;
    const float footerY = static_cast<float>(height) - footerHeight;
    settingsUi_.draw->fillRect(
        SDL_FRect{0.0f, 0.0f, static_cast<float>(width),
                  static_cast<float>(height)},
        background);
    settingsUi_.draw->fillRect(
        SDL_FRect{0.0f, 0.0f, static_cast<float>(width), headerHeight},
        headerBackground);
    // A restrained accent wash fades out of the header instead of a flat band.
    for (int i = 0; i < 44; ++i) {
        const float t = static_cast<float>(i) / 44.0f;
        const uint8_t alpha = static_cast<uint8_t>(
            (1.0f - t) * (dark ? 26.0f : 14.0f));
        if (alpha == 0) {
            continue;
        }
        settingsUi_.draw->fillRect(
            SDL_FRect{0.0f, static_cast<float>(i) * 2.0f,
                      static_cast<float>(width), 2.0f},
            rgba(64, 132, 255, alpha));
    }
    settingsUi_.draw->fillRect(
        SDL_FRect{0.0f, headerHeight - 1.0f, static_cast<float>(width), 1.0f},
        hairline);

    // App mark: an accent squircle with a miniature Dock glyph.
    const SDL_FRect logo{28.0f, 24.0f, 40.0f, 40.0f};
    settingsUi_.draw->roundedRect(
        SDL_FRect{logo.x - 1.0f, logo.y - 1.0f, logo.w + 2.0f, logo.h + 2.0f},
        12.0f, rgba(64, 132, 255, dark ? 55 : 32));
    settingsUi_.draw->roundedRect(logo, 11.0f, accent);
    for (int i = 0; i < 3; ++i) {
        settingsUi_.draw->circle(
            SDL_FRect{logo.x + 9.0f + static_cast<float>(i) * 8.0f,
                      logo.y + 12.0f, 6.0f, 6.0f},
            rgba(255, 255, 255, 238));
    }
    settingsUi_.draw->roundedRect(
        SDL_FRect{logo.x + 8.0f, logo.y + 25.0f, 24.0f, 5.0f}, 2.5f,
        rgba(255, 255, 255, 225));

    settingsUi_.text->drawText("RevDock Settings", 82.0f, 21.0f, 21.0f,
                               primaryText(dark));
    settingsUi_.text->drawText("Dock appearance & behavior", 83.0f, 50.0f,
                               11.5f, secondaryText(dark));

    const std::string version = std::string("v") + REVDOCK_VERSION;
    const SDL_FRect versionChip{static_cast<float>(width) - 104.0f, 31.0f,
                                76.0f, 24.0f};
    settingsUi_.draw->roundedRect(versionChip, 12.0f,
                                  dark ? rgba(255, 255, 255, 14)
                                       : rgba(16, 20, 30, 8));
    settingsUi_.text->drawTextCentered(version, versionChip, 11.0f,
                                       secondaryText(dark));

    settingsUi_.draw->fillRect(
        SDL_FRect{0.0f, footerY, static_cast<float>(width), footerHeight},
        headerBackground);
    settingsUi_.draw->fillRect(
        SDL_FRect{0.0f, footerY, static_cast<float>(width), 1.0f}, hairline);

    const float cardX = 24.0f;
    const float cardW = static_cast<float>(width) - cardX * 2.0f;
    const float paddingX = 28.0f;
    const float labelX = cardX + paddingX;
    const float controlW = std::clamp(cardW * 0.46f, 250.0f, 380.0f);
    const float controlX = cardX + cardW - paddingX - controlW;
    const float cardHeader = 42.0f;
    const float rowSlider = 46.0f;
    const float rowToggle = 44.0f;
    const float rowSegment = 50.0f;

    auto hoveredId = [&](const std::string &id) {
        for (size_t i = 0; i < settingsUi_.hits.size(); ++i) {
            if (static_cast<int>(i) == settingsUi_.hover &&
                settingsUi_.hits[i].id == id) {
                return true;
            }
        }
        return false;
    };
    auto addHit = [&](const std::string &id, const SDL_FRect &rect) {
        settingsUi_.hits.push_back(SettingsHit{rect, id});
    };
    auto sectionCard = [&](const char *title, float top, float cardHeight) {
        settingsUi_.draw->roundedRect(
            SDL_FRect{cardX, top, cardW, cardHeight}, 16.0f, cardBorder);
        settingsUi_.draw->roundedRect(
            SDL_FRect{cardX + 1.0f, top + 1.0f, cardW - 2.0f,
                      cardHeight - 2.0f},
            15.0f, cardBackground);
        settingsUi_.draw->circle(
            SDL_FRect{labelX, top + 17.0f, 6.0f, 6.0f}, accent);
        settingsUi_.text->drawText(title, labelX + 14.0f, top + 13.0f, 11.5f,
                                   secondaryText(dark));
        settingsUi_.draw->fillRect(
            SDL_FRect{labelX, top + 36.0f, cardW - paddingX * 2.0f, 1.0f},
            hairline);
    };
    float y = headerHeight + 20.0f;
    auto rowLabel = [&](const char *text, float rowHeight) {
        settingsUi_.text->drawText(
            text, labelX, y + (rowHeight - 18.0f) * 0.5f + 1.0f, 14.5f,
            primaryText(dark));
    };
    auto slider = [&](const std::string &id, const char *text, float value,
                      float minValue, float maxValue, const char *valueText) {
        const float rowHeight = rowSlider;
        rowLabel(text, rowHeight);
        addHit(id, SDL_FRect{controlX, y + 4.0f, controlW, rowHeight - 8.0f});
        const float valueW = 66.0f;
        const float trackW = controlW - valueW - 16.0f;
        const float cy = y + rowHeight * 0.5f;
        const float t = std::clamp((value - minValue) /
                                       std::max(0.001f, maxValue - minValue),
                                   0.0f, 1.0f);
        settingsUi_.draw->roundedRect(
            SDL_FRect{controlX, cy - 3.0f, trackW, 6.0f}, 3.0f,
            hoveredId(id) ? (dark ? rgba(255, 255, 255, 44)
                                  : rgba(16, 20, 30, 36))
                          : trackColor);
        if (t > 0.0f) {
            settingsUi_.draw->roundedRect(
                SDL_FRect{controlX, cy - 3.0f, trackW * t, 6.0f}, 3.0f,
                accent);
        }
        const SDL_FRect knob{controlX + trackW * t - 9.0f, cy - 9.0f, 18.0f,
                             18.0f};
        settingsUi_.draw->circle(
            SDL_FRect{knob.x - 1.0f, knob.y + 1.0f, 20.0f, 20.0f},
            rgba(0, 0, 0, dark ? 80 : 28));
        settingsUi_.draw->circle(knob, rgba(250, 250, 253, 255));
        const SDL_FRect chip{controlX + controlW - valueW,
                             cy - 12.0f, valueW, 24.0f};
        settingsUi_.draw->roundedRect(
            chip, 7.0f,
            hoveredId(id) ? (dark ? rgba(255, 255, 255, 26)
                                  : rgba(16, 20, 30, 14))
                          : (dark ? rgba(255, 255, 255, 14)
                                  : rgba(16, 20, 30, 8)));
        settingsUi_.text->drawTextCentered(valueText, chip, 11.5f,
                                           secondaryText(dark));
        y += rowHeight;
    };
    auto toggle = [&](const std::string &id, const char *text, bool on) {
        const float rowHeight = rowToggle;
        rowLabel(text, rowHeight);
        addHit(id, SDL_FRect{controlX, y + 4.0f, controlW, rowHeight - 8.0f});
        const SDL_FRect rect{controlX + controlW - 50.0f,
                             y + (rowHeight - 28.0f) * 0.5f, 50.0f, 28.0f};
        const bool hovered = hoveredId(id);
        SDL_Color fill = on ? (hovered ? accentHover : accent)
                            : (dark ? rgba(255, 255, 255, hovered ? 44 : 30)
                                    : rgba(16, 20, 30, hovered ? 34 : 24));
        settingsUi_.draw->roundedRect(rect, 14.0f, fill);
        const float knobX = rect.x + (on ? 24.0f : 4.0f);
        settingsUi_.draw->circle(
            SDL_FRect{knobX - 1.0f, rect.y + 4.0f, 24.0f, 24.0f},
            rgba(0, 0, 0, dark ? 60 : 22));
        settingsUi_.draw->circle(SDL_FRect{knobX, rect.y + 3.0f, 22.0f, 22.0f},
                                 rgba(252, 252, 255, 255));
        y += rowHeight;
    };
    auto segmented = [&](const std::string &prefix, const char *text,
                         const std::vector<std::pair<std::string, std::string>> &options,
                         const std::string &selected) {
        const float rowHeight = rowSegment;
        rowLabel(text, rowHeight);
        const SDL_FRect container{controlX, y + (rowHeight - 34.0f) * 0.5f,
                                  controlW, 34.0f};
        settingsUi_.draw->roundedRect(
            container, 9.0f,
            dark ? rgba(255, 255, 255, 14) : rgba(16, 20, 30, 10));
        const float inset = 3.0f;
        const float gap = 3.0f;
        const float itemW =
            (container.w - inset * 2.0f -
             gap * static_cast<float>(options.size() - 1)) /
            static_cast<float>(options.size());
        for (size_t i = 0; i < options.size(); ++i) {
            SDL_FRect rect{container.x + inset +
                               static_cast<float>(i) * (itemW + gap),
                           container.y + inset, itemW,
                           container.h - inset * 2.0f};
            const std::string id = prefix + "-" + options[i].first;
            addHit(id, rect);
            const bool on = options[i].first == selected;
            if (on) {
                settingsUi_.draw->roundedRect(rect, 7.0f, accent);
            } else if (hoveredId(id)) {
                settingsUi_.draw->roundedRect(
                    rect, 7.0f,
                    dark ? rgba(255, 255, 255, 18)
                         : rgba(255, 255, 255, 210));
            }
            settingsUi_.text->drawTextCentered(
                options[i].second, rect, 12.5f,
                on ? rgba(255, 255, 255, 255) : primaryText(dark));
        }
        y += rowHeight;
    };
    auto button = [&](const std::string &id, const char *text, float bx,
                      float by, float bw, bool accented) {
        const SDL_FRect rect{bx, by, bw, 34.0f};
        addHit(id, rect);
        const bool hovered = hoveredId(id);
        if (accented) {
            settingsUi_.draw->roundedRect(rect, 9.0f,
                                          hovered ? accentHover : accent);
        } else {
            settingsUi_.draw->roundedRect(rect, 9.0f, cardBorder);
            settingsUi_.draw->roundedRect(
                SDL_FRect{rect.x + 1.0f, rect.y + 1.0f, rect.w - 2.0f,
                          rect.h - 2.0f},
                8.0f,
                hovered ? (dark ? rgba(255, 255, 255, 30)
                                : rgba(16, 20, 30, 14))
                        : cardBackground);
        }
        settingsUi_.text->drawTextCentered(
            text, rect, 12.5f,
            accented ? rgba(255, 255, 255, 255) : primaryText(dark));
    };

    const float appearanceHeight =
        cardHeader + 3.0f * rowSlider + rowToggle + 2.0f * rowSegment +
        2.0f * rowToggle + 12.0f;
    const float behaviorHeight =
        cardHeader + rowToggle + 2.0f * rowSlider + 12.0f;

    sectionCard("Appearance", y, appearanceHeight);
    y += cardHeader;
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%d px",
                  static_cast<int>(settings_.tileSize()));
    slider("tile-size", "Icon size", settings_.tileSize(), 16.0f, 128.0f,
           buffer);
    toggle("magnification", "Magnification", settings_.magnification());
    std::snprintf(buffer, sizeof(buffer), "%d px",
                  static_cast<int>(settings_.largeSize()));
    slider("large-size", "Magnification size", settings_.largeSize(),
           settings_.tileSize(), 192.0f, buffer);
    segmented("pos", "Position",
              {{"bottom", "Bottom"}, {"left", "Left"}, {"right", "Right"}},
              settings_.orientation());
    segmented("theme", "Theme",
              {{"dark", "Dark"}, {"light", "Light"}, {"system", "System"}},
              settings_.theme());
    toggle("labels", "Show labels", settings_.showLabels());
    toggle("indicators", "Show running indicators",
           settings_.showIndicators());
    y += 12.0f;

    y += 14.0f;
    sectionCard("Behavior", y, behaviorHeight);
    y += cardHeader;
    toggle("autohide", "Automatically hide the Dock", settings_.autohide());
    std::snprintf(buffer, sizeof(buffer), "%.1f s", settings_.autohideDelay());
    slider("autohide-delay", "Auto-hide delay", settings_.autohideDelay(),
           0.0f, 2.0f, buffer);
    std::snprintf(buffer, sizeof(buffer), "%.1fx",
                  settings_.autohideTimeModifier());
    slider("autohide-speed", "Auto-hide speed",
           settings_.autohideTimeModifier(), 0.2f, 3.0f, buffer);

    button("reset", "Reset", 28.0f, footerY + 19.0f, 96.0f, false);
    settingsUi_.text->drawText("RevDock " + version, 138.0f,
                               footerY + 29.0f, 11.0f, secondaryText(dark));
    const float closeW = 100.0f;
    const float closeX = static_cast<float>(width) - 28.0f - closeW;
    button("close", "Close", closeX, footerY + 19.0f, closeW, true);
    button("open-config", "Open Config Folder", closeX - 12.0f - 176.0f,
           footerY + 19.0f, 176.0f, false);

    settingsUi_.draw->endFrame();
}

bool Application::stackAnimating() const {
    if (!stack_.open) {
        return false;
    }
    if (stack_.closing) {
        return true;
    }
    if (now_ - stack_.openedAt < 0.20) {
        return true;
    }
    return stack_.hover >= 0 && now_ - stack_.hoverAt < 0.16;
}

bool Application::shouldKeepAnimating() const {
    bool gridAnimating = grid_.folderAnim.active() || grid_.folderClosing ||
                         !grid_.returnFrom.empty() || grid_.pendingTextAt > 0.0 ||
                         grid_.folderDrop.active ||
                         (grid_.pressed && grid_.dragMoved);
    if (!gridAnimating) {
        for (const auto &[key, anim] : grid_.cellAnims) {
            (void)key;
            if (anim.init && now_ < anim.start + anim.duration) {
                gridAnimating = true;
                break;
            }
        }
    }
    return autohide_.active() || dragging_ || gridOpacity_.active() || !launching_.empty() ||
           reorderAnimating_ || grid_.swipeAnimating || grid_.blankPressed || grid_.wheelSwiping ||
           stackAnimating() || gridAnimating ||
           minimize_.active || magEnvelope_.active() || !flashId_.empty();
}

} // namespace revdock
