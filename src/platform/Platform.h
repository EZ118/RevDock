#pragma once

#include "model/DockModel.h"
#include "platform/AppInfo.h"

#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_video.h>

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <cstdint>

namespace revdock {

class Platform {
public:
    virtual ~Platform() = default;

    virtual std::vector<AppInfo> scanApplications() = 0;
    virtual SDL_Surface *loadIcon(const AppInfo &app, int pixelSize) = 0;
    virtual SDL_Surface *loadIconPath(const std::string &path, int pixelSize) = 0;

    virtual bool launchApplication(const AppInfo &app) = 0;
    virtual bool launchItem(const DockItem &item) = 0;
    virtual bool activateApplication(const AppInfo &app) = 0;
    virtual bool showAllWindows(const AppInfo &app) { return activateApplication(app); }
    virtual bool minimizeApplication(const AppInfo &app) {
        (void)app;
        return false;
    }
    virtual bool quitApplication(const AppInfo &app) = 0;
    virtual bool forceQuitApplication(const AppInfo &app) { return quitApplication(app); }
    // Launches an application with command-line arguments (used by the
    // app-provided Dock menu: new window, new private window, recent items).
    virtual bool launchApplicationWithArgs(const AppInfo &app,
                                           const std::vector<std::string> &args) {
        (void)app;
        (void)args;
        return false;
    }
    // Application-provided Dock menu entries: jump-list tasks and recent
    // history/documents. Returned entries are inserted above the standard
    // macOS-style Dock menu items.
    virtual std::vector<AppMenuItem> appMenuItems(const AppInfo &app) {
        (void)app;
        return {};
    }
    virtual bool runAppMenuItem(const AppInfo &app, const AppMenuItem &item) {
        (void)app;
        (void)item;
        return false;
    }
    // Application state used to build the macOS process menu.
    virtual bool isApplicationResponding(const AppInfo &app) {
        (void)app;
        return true;
    }
    virtual bool canHideApplications() const { return false; }
    virtual bool hideApplication(const AppInfo &app) {
        (void)app;
        return false;
    }
    virtual bool hideOtherApplications(const AppInfo &app) {
        (void)app;
        return false;
    }
    virtual bool isApplicationHidden(const AppInfo &app) {
        (void)app;
        return false;
    }
    // macOS "Open at Login" equivalent (Windows Startup folder / XDG autostart).
    virtual bool isOpenAtLogin(const AppInfo &app) {
        (void)app;
        return false;
    }
    virtual bool setOpenAtLogin(const AppInfo &app, bool enabled) {
        (void)app;
        (void)enabled;
        return false;
    }
    virtual std::string revealLabel() const { return "Show in Finder"; }
    virtual bool openPath(const std::string &path) = 0;
    virtual bool openPathWithApp(const AppInfo &app, const std::string &path) {
        (void)app;
        (void)path;
        return false;
    }
    virtual bool revealPath(const std::string &path) = 0;
    virtual bool openTrash() = 0;
    virtual bool emptyTrash() = 0;
    virtual bool isTrashEmpty() { return true; }
    virtual bool openFileManager() = 0;
    // Starts a native file drag (Windows OLE / desktop drag-and-drop).
    virtual bool startFileDrag(const std::vector<std::string> &paths) {
        (void)paths;
        return false;
    }
    virtual bool recyclePath(const std::string &path) {
        (void)path;
        return false;
    }
    // Moves a file/folder into `directory` (used when dropping a Dock item or
    // an external file onto a folder tile). Returns false if it was not moved.
    virtual bool movePathInto(const std::string &path, const std::string &directory) {
        (void)path;
        (void)directory;
        return false;
    }
    // Resolves a shortcut to its executable target and (for packaged/UWP
    // shortcuts) its Application User Model ID.
    virtual std::string shortcutTarget(const std::string &path) {
        (void)path;
        return {};
    }
    virtual std::string shortcutAumid(const std::string &path) {
        (void)path;
        return {};
    }
    // Suppresses the platform/compositor's own minimize animation while the
    // Dock is running; implementations must restore the previous state.
    virtual void setSystemMinimizeAnimationsEnabled(bool) {}
    // Finder stands in for the platform's file manager everywhere.
    virtual bool fileManagerRunning() { return false; }
    virtual bool isFileManagerApp(const AppInfo &app) {
        (void)app;
        return false;
    }
    virtual bool isFileManagerShortcut(const std::string &path) {
        (void)path;
        return false;
    }

    virtual std::set<std::string> runningExecutables() = 0;
    // Every running process's executable path (lower-case absolute path).
    // Window-based detection misses console programs hosted by another
    // terminal and apps whose shortcut points at a launcher stub.
    virtual std::set<std::string> runningProcessPaths() { return {}; }
    // Executable path of the current foreground window's process, or empty.
    virtual std::string foregroundProcessPath() { return {}; }

    virtual std::string downloadsPath() = 0;
    virtual std::string wallpaperPath() { return {}; }
    virtual std::string trashLabel() = 0;

    virtual bool globalCursor(float &x, float &y) = 0;
    virtual void prepareDockWindow(SDL_Window *window) = 0;
    virtual void setClickThrough(SDL_Window *, bool, bool = false) {}
    virtual void raiseAlwaysOnTop(SDL_Window *) {}
    // Brings an overlay such as Launchpad to the foreground so keyboard input
    // (search, folder renaming) reaches it immediately.
    virtual void focusWindow(SDL_Window *window) {
        if (window) {
            SDL_RaiseWindow(window);
        }
    }
    // Captures committed IME text natively (Windows).  SDL's text input can be
    // silently dropped when the Dock/no-activate activation path leaves SDL's
    // keyboard focus behind; this keeps CJK input working regardless.
    virtual void setTextCommitHandler(SDL_Window *,
                                      std::function<void(const std::string &)>) {}
    // Makes `owned` an owned window of `owner` (pass nullptr to clear), which
    // keeps it above the owner independently of activation (used to keep the
    // Dock over Launchpad while the grid is visible).
    virtual void setWindowOwner(SDL_Window *, SDL_Window *) {}
    virtual void setTaskbarVisible(bool) {}
    virtual void setHitTestRects(SDL_Window *, const std::vector<SDL_Rect> &) {}
    // Makes the given window let clicks through in these client-area rects
    // (used so Launchpad never swallows clicks aimed at the Dock above it).
    virtual void setPassThroughRects(SDL_Window *, const std::vector<SDL_Rect> &) {}
    // Captures the screen region behind the window. `downscale` lets the
    // platform capture (and therefore blur) at a lower resolution; the result is
    // upscaled with hardware filtering when drawn.
    virtual bool captureWindowBackdrop(SDL_Window *, int, int, int, int,
                                       std::vector<uint8_t> &, int &, int &,
                                       int downscale = 1) {
        (void)downscale;
        return false;
    }
    virtual void reserveWorkArea(int, const std::string &, int, bool) {}

    struct MinimizeEvent {
        std::uintptr_t handle = 0;
        std::string execPath;
        std::string aumid;
        std::string title;
        SDL_Rect rect{};
        std::vector<uint8_t> rgba;
        int width = 0;
        int height = 0;
    };

    struct MinimizedWindow {
        std::uintptr_t handle = 0;
        std::string execPath;
        std::string title;
        std::vector<uint8_t> rgba;
        int width = 0;
        int height = 0;
        std::uint64_t minimizedAt = 0;
    };
    virtual std::vector<MinimizedWindow> minimizedWindows() { return {}; }
    virtual bool restoreWindow(std::uintptr_t handle) {
        (void)handle;
        return false;
    }
    // Normal (restored) rectangle of a minimized window, used to animate the
    // window back out of its Dock tile.
    virtual bool windowRestoreRect(std::uintptr_t handle, SDL_Rect &rect) {
        (void)handle;
        (void)rect;
        return false;
    }
    virtual bool closeWindow(std::uintptr_t handle) {
        (void)handle;
        return false;
    }
    virtual bool pollMinimizeEvent(MinimizeEvent &event) {
        (void)event;
        return false;
    }
};

std::unique_ptr<Platform> createPlatform();

} // namespace revdock
