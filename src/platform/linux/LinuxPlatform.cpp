#include "platform/Platform.h"

#include "apps/AppMenu.h"
#include "core/Log.h"
#include "core/Types.h"
#include "core/Utf.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#ifdef REVDOCK_HAVE_X11
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif
#ifdef REVDOCK_HAVE_XSHAPE
#include <X11/extensions/shape.h>
#endif

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace revdock {
namespace {

std::string trim(const std::string &value) {
    size_t begin = value.find_first_not_of(" \t\r\n");
    size_t end = value.find_last_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    return value.substr(begin, end - begin + 1);
}

std::vector<std::string> splitDesktopList(const std::string &value) {
    std::vector<std::string> result;
    std::stringstream stream(value);
    std::string part;
    while (std::getline(stream, part, ';')) {
        if (!part.empty()) {
            result.push_back(part);
        }
    }
    return result;
}

std::string unquoteExecToken(const std::string &token) {
    if (token.size() >= 2 && token.front() == '"' && token.back() == '"') {
        return token.substr(1, token.size() - 2);
    }
    return token;
}

std::vector<std::string> parseExec(const std::string &command) {
    std::vector<std::string> args;
    std::string current;
    bool quoted = false;
    bool escaped = false;
    for (char ch : command) {
        if (escaped) {
            current.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            quoted = !quoted;
            continue;
        }
        if (!quoted && (ch == ' ' || ch == '\t')) {
            if (!current.empty()) {
                args.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        args.push_back(current);
    }
    for (auto &arg : args) {
        arg = unquoteExecToken(arg);
    }
    if (!args.empty()) {
        args.erase(std::remove_if(args.begin(), args.end(), [](const std::string &arg) {
            return !arg.empty() && arg[0] == '%' && arg.size() == 2 &&
                   std::string("fFuUdDnNickvm").find(arg[1]) != std::string::npos;
        }), args.end());
    }
    return args;
}

std::string baseName(const std::string &path) {
    return std::filesystem::path(path).filename().string();
}

std::string findExecutableInPath(const std::string &name) {
    if (name.empty()) {
        return {};
    }
    if (name.find('/') != std::string::npos) {
        return access(name.c_str(), X_OK) == 0 ? name : std::string{};
    }
    const char *pathEnv = std::getenv("PATH");
    if (!pathEnv) {
        return {};
    }
    std::stringstream stream(pathEnv);
    std::string dir;
    while (std::getline(stream, dir, ':')) {
        std::filesystem::path candidate = std::filesystem::path(dir) / name;
        if (access(candidate.c_str(), X_OK) == 0) {
            return candidate.string();
        }
    }
    return {};
}

bool spawnDetached(const std::vector<std::string> &args) {
    if (args.empty()) {
        return false;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        setsid();
        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (const auto &arg : args) {
            argv.push_back(const_cast<char *>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    return true;
}

bool spawnShellOpen(const std::string &target) {
    const std::string opener = findExecutableInPath("xdg-open");
    if (opener.empty()) {
        return false;
    }
    return spawnDetached({opener, target});
}

struct DesktopEntry {
    std::map<std::string, std::string> values;
    bool valid = false;
};

DesktopEntry parseDesktopFile(const std::filesystem::path &path) {
    DesktopEntry entry;
    std::ifstream file(path);
    if (!file) {
        return entry;
    }
    std::string line;
    bool inEntry = false;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            inEntry = line == "[Desktop Entry]";
            continue;
        }
        if (!inEntry) {
            continue;
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        entry.values[trim(line.substr(0, equals))] = trim(line.substr(equals + 1));
    }
    entry.valid = entry.values.count("Name") > 0;
    return entry;
}

std::string localizedValue(const std::map<std::string, std::string> &values, const std::string &key) {
    const char *langEnv = std::getenv("LANG");
    if (langEnv) {
        std::string lang = langEnv;
        const size_t dot = lang.find('.');
        if (dot != std::string::npos) {
            lang = lang.substr(0, dot);
        }
        const size_t underscore = lang.find('_');
        if (underscore != std::string::npos) {
            const std::string language = lang.substr(0, underscore);
            const std::string region = lang.substr(underscore + 1);
            auto regional = values.find(key + "[" + language + "_" + region + "]");
            if (regional != values.end()) {
                return regional->second;
            }
            auto shortLang = values.find(key + "[" + language + "]");
            if (shortLang != values.end()) {
                return shortLang->second;
            }
        }
    }
    auto it = values.find(key);
    return it == values.end() ? std::string{} : it->second;
}

std::string userDataHome() {
    const char *xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg) {
        return xdg;
    }
    const char *home = std::getenv("HOME");
    return home ? std::string(home) + "/.local/share" : std::string{};
}

std::string userConfigHome() {
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        return xdg;
    }
    const char *home = std::getenv("HOME");
    return home ? std::string(home) + "/.config" : std::string{};
}

std::vector<std::string> dataDirs() {
    std::vector<std::string> result;
    result.push_back(userDataHome());
    const char *dirs = std::getenv("XDG_DATA_DIRS");
    if (dirs && *dirs) {
        std::stringstream stream(dirs);
        std::string dir;
        while (std::getline(stream, dir, ':')) {
            if (!dir.empty()) {
                result.push_back(dir);
            }
        }
    } else {
        result.push_back("/usr/local/share");
        result.push_back("/usr/share");
    }
    return result;
}

class IconThemeIndex {
public:
    std::string lookup(const std::string &name) {
        if (name.empty()) {
            return {};
        }
        if (name.front() == '/') {
            return std::filesystem::exists(name) ? name : std::string{};
        }
        if (!built_) {
            build();
        }
        auto it = icons_.find(name);
        if (it != icons_.end()) {
            return it->second;
        }
        return {};
    }

private:
    bool built_ = false;
    std::map<std::string, std::string> icons_;

    void build() {
        built_ = true;
        std::vector<std::filesystem::path> roots;
        for (const auto &dir : dataDirs()) {
            roots.emplace_back(std::filesystem::path(dir) / "icons");
        }
        const char *home = std::getenv("HOME");
        if (home) {
            roots.emplace_back(std::filesystem::path(home) / ".icons");
        }
        roots.emplace_back("/usr/share/pixmaps");

        for (const auto &root : roots) {
            std::error_code ec;
            if (!std::filesystem::exists(root, ec)) {
                continue;
            }
            for (auto it = std::filesystem::recursive_directory_iterator(
                     root, std::filesystem::directory_options::skip_permission_denied, ec);
                 it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) {
                    ec.clear();
                    continue;
                }
                if (!it->is_regular_file(ec)) {
                    continue;
                }
                const auto ext = lowerAscii(it->path().extension().string());
                if (ext != ".png" && ext != ".svg") {
                    continue;
                }
                const std::string stem = it->path().stem().string();
                auto found = icons_.find(stem);
                if (found == icons_.end()) {
                    icons_[stem] = it->path().string();
                } else if (ext == ".svg" && lowerAscii(found->second).find(".svg") == std::string::npos) {
                    found->second = it->path().string();
                }
            }
        }
    }
};

std::string iconSearchName(const std::string &icon) {
    if (icon.empty()) {
        return {};
    }
    if (icon.front() == '/') {
        return icon;
    }
    static IconThemeIndex index;
    return index.lookup(icon);
}

SDL_Surface *loadImageFile(const std::string &path, int pixelSize) {
    if (path.empty()) {
        return nullptr;
    }
    if (lowerAscii(std::filesystem::path(path).extension().string()) == ".svg") {
        SDL_IOStream *io = SDL_IOFromFile(path.c_str(), "rb");
        if (!io) {
            return nullptr;
        }
        SDL_Surface *surface = IMG_LoadSizedSVG_IO(io, pixelSize, pixelSize);
        SDL_CloseIO(io);
        return surface;
    }
    return IMG_Load(path.c_str());
}

std::string readDownloadDir() {
    const std::filesystem::path file = std::filesystem::path(userConfigHome()) / "user-dirs.dirs";
    std::ifstream stream(file);
    std::string line;
    const char *home = std::getenv("HOME");
    while (std::getline(stream, line)) {
        if (line.rfind("XDG_DOWNLOAD_DIR=", 0) == 0) {
            std::string value = line.substr(17);
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.size() - 2);
            }
            const std::string homeMarker = "$HOME";
            if (value.rfind(homeMarker, 0) == 0 && home) {
                value = std::string(home) + value.substr(homeMarker.size());
            }
            return value;
        }
    }
    return home ? std::string(home) + "/Downloads" : std::string{};
}

} // namespace

class LinuxPlatform final : public Platform {
public:
    std::vector<AppInfo> scanApplications() override {
        std::vector<AppInfo> apps;
        std::set<std::string> seen;
        for (const auto &dir : dataDirs()) {
            const auto root = std::filesystem::path(dir) / "applications";
            std::error_code ec;
            if (!std::filesystem::exists(root, ec)) {
                continue;
            }
            for (auto it = std::filesystem::recursive_directory_iterator(
                     root, std::filesystem::directory_options::skip_permission_denied, ec);
                 it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) {
                    ec.clear();
                    continue;
                }
                if (!it->is_regular_file(ec) ||
                    lowerAscii(it->path().extension().string()) != ".desktop") {
                    continue;
                }
                DesktopEntry entry = parseDesktopFile(it->path());
                if (!entry.valid) {
                    continue;
                }
                if (entry.values["NoDisplay"] == "true" || entry.values["Hidden"] == "true") {
                    continue;
                }
                const std::string onlyShow = entry.values["OnlyShowIn"];
                if (!onlyShow.empty()) {
                    const std::string current = currentDesktop();
                    auto entries = splitDesktopList(onlyShow);
                    if (std::find(entries.begin(), entries.end(), current) == entries.end()) {
                        continue;
                    }
                }
                const std::string notShow = entry.values["NotShowIn"];
                if (!notShow.empty()) {
                    const std::string current = currentDesktop();
                    auto entries = splitDesktopList(notShow);
                    if (std::find(entries.begin(), entries.end(), current) != entries.end()) {
                        continue;
                    }
                }
                const std::string tryExec = entry.values["TryExec"];
                if (!tryExec.empty() && findExecutableInPath(tryExec).empty()) {
                    continue;
                }
                // Only real applications: links/directories and shell wrappers are
                // not launchable programs in the macOS sense.
                const auto typeIt = entry.values.find("Type");
                if (typeIt != entry.values.end() && typeIt->second != "Application") {
                    continue;
                }
                const std::vector<std::string> execArgs = parseExec(entry.values["Exec"]);
                if (execArgs.empty()) {
                    continue;
                }
                const std::string program = lowerAscii(baseName(execArgs.front()));
                static const std::set<std::string> kShells = {"sh", "bash", "dash", "zsh"};
                if (kShells.count(program) > 0 || program.rfind("uninst", 0) == 0 ||
                    program == "msiexec") {
                    continue;
                }
                AppInfo app;
                app.id = it->path().string();
                app.sourcePath = app.id;
                app.name = localizedValue(entry.values, "Name");
                app.execPath = execArgs.front();
                app.iconPath = entry.values["Icon"];
                app.wmClass = entry.values["StartupWMClass"];
                if (app.wmClass.empty() && !app.execPath.empty()) {
                    app.wmClass = baseName(app.execPath);
                }
                app.terminal = entry.values["Terminal"] == "true";
                const std::string key = lowerAscii(app.execPath.empty() ? app.id : app.execPath);
                if (!seen.insert(key).second) {
                    continue;
                }
                apps.push_back(std::move(app));
            }
        }
        std::sort(apps.begin(), apps.end(), [](const AppInfo &a, const AppInfo &b) {
            return lowerAscii(a.name) < lowerAscii(b.name);
        });
        RD_LOG_INFO("Scanned %zu desktop applications", apps.size());
        return apps;
    }

    SDL_Surface *loadIcon(const AppInfo &app, int pixelSize) override {
        std::string path;
        if (!app.iconPath.empty()) {
            path = iconSearchName(app.iconPath);
        }
        if (path.empty() && !app.execPath.empty()) {
            path = iconSearchName(baseName(app.execPath));
        }
        return loadImageFile(path, pixelSize);
    }

    SDL_Surface *loadIconPath(const std::string &path, int pixelSize) override {
        return loadImageFile(iconSearchName(path), pixelSize);
    }

    bool launchApplication(const AppInfo &app) override {
        if (app.sourcePath.empty()) {
            return false;
        }
        if (!findExecutableInPath("gio").empty()) {
            return spawnDetached({"gio", "launch", app.sourcePath});
        }
        DesktopEntry entry = parseDesktopFile(app.sourcePath);
        std::vector<std::string> args = parseExec(entry.values["Exec"]);
        if (args.empty()) {
            return false;
        }
        return spawnDetached(args);
    }

    bool launchItem(const DockItem &item) override {
        switch (item.type) {
        case ItemType::Application:
        case ItemType::Folder:
        case ItemType::File:
        case ItemType::Url:
            return spawnShellOpen(item.path);
        case ItemType::Trash:
            return openTrash();
        case ItemType::MinimizedWindow:
        case ItemType::Separator:
        case ItemType::AppGrid:
            return false;
        case ItemType::Finder:
            return openFileManager();
        }
        return false;
    }

    bool activateApplication(const AppInfo &app) override {
#ifdef REVDOCK_HAVE_X11
        if (!window_) {
            return false;
        }
        SDL_PropertiesID props = SDL_GetWindowProperties(window_);
        Display *display = static_cast<Display *>(
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
        if (!display) {
            return false;
        }
        const std::string wanted = lowerAscii(app.wmClass.empty()
                                                  ? std::filesystem::path(app.execPath).stem().string()
                                                  : app.wmClass);
        if (wanted.empty()) {
            return false;
        }
        const Window root = DefaultRootWindow(display);
        const Atom clientList = XInternAtom(display, "_NET_CLIENT_LIST", False);
        Atom actualType = None;
        int actualFormat = 0;
        unsigned long count = 0;
        unsigned long remaining = 0;
        unsigned char *data = nullptr;
        if (XGetWindowProperty(display, root, clientList, 0, 4096, False, XA_WINDOW,
                               &actualType, &actualFormat, &count, &remaining, &data) != Success ||
            !data) {
            return false;
        }
        Window found = 0;
        auto windows = reinterpret_cast<Window *>(data);
        for (unsigned long i = 0; i < count; ++i) {
            XClassHint hint{};
            if (XGetClassHint(display, windows[i], &hint)) {
                const std::string resClass = hint.res_class ? hint.res_class : "";
                const std::string resName = hint.res_name ? hint.res_name : "";
                if (lowerAscii(resClass) == wanted || lowerAscii(resName) == wanted) {
                    found = windows[i];
                }
                if (hint.res_class) XFree(hint.res_class);
                if (hint.res_name) XFree(hint.res_name);
                if (found) {
                    break;
                }
            }
        }
        XFree(data);
        if (!found) {
            return false;
        }
        XEvent event{};
        event.xclient.type = ClientMessage;
        event.xclient.window = found;
        event.xclient.message_type = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
        event.xclient.format = 32;
        event.xclient.data.l[0] = 2;
        event.xclient.data.l[1] = CurrentTime;
        XSendEvent(display, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &event);
        XRaiseWindow(display, found);
        XFlush(display);
        return true;
#else
        (void)app;
        return false;
#endif
    }

    bool quitApplication(const AppInfo &app) override {
        const std::string wanted = lowerAscii(app.execPath);
        const std::string stem = lowerAscii(
            std::filesystem::path(app.execPath.empty() ? app.sourcePath : app.execPath)
                .stem()
                .string());
        if (wanted.empty() && stem.empty()) {
            return false;
        }
        bool quit = false;
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator("/proc", ec);
             it != std::filesystem::directory_iterator(); it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            const std::string name = it->path().filename().string();
            if (name.empty() || !std::all_of(name.begin(), name.end(), [](unsigned char c) {
                    return std::isdigit(c) != 0;
                })) {
                continue;
            }
            std::error_code linkError;
            const std::filesystem::path exe =
                std::filesystem::read_symlink(it->path() / "exe", linkError);
            const std::string exeLower = linkError ? std::string{} : lowerAscii(exe.string());
            bool matches = !wanted.empty() && exeLower == wanted;
            if (!matches && !stem.empty()) {
                std::ifstream comm(it->path() / "comm");
                std::string processName;
                std::getline(comm, processName);
                matches = lowerAscii(processName) == stem;
            }
            if (matches) {
                const pid_t pid = static_cast<pid_t>(std::strtol(name.c_str(), nullptr, 10));
                if (pid > 0 && kill(pid, SIGTERM) == 0) {
                    quit = true;
                }
            }
        }
        return quit;
    }

    bool launchApplicationWithArgs(const AppInfo &app,
                                   const std::vector<std::string> &args) override {
        if (args.empty()) {
            return launchApplication(app);
        }
        std::vector<std::string> command;
        if (!app.sourcePath.empty() &&
            lowerAscii(std::filesystem::path(app.sourcePath).extension().string()) ==
                ".desktop") {
            DesktopEntry entry = parseDesktopFile(app.sourcePath);
            command = parseExec(entry.values["Exec"]);
        }
        if (command.empty() && !app.execPath.empty()) {
            command.push_back(app.execPath);
        }
        if (command.empty()) {
            return false;
        }
        command.insert(command.end(), args.begin(), args.end());
        return spawnDetached(command);
    }

    std::vector<AppMenuItem> appMenuItems(const AppInfo &app) override {
        return appProfileMenuItems(app, 8);
    }

    bool runAppMenuItem(const AppInfo &app, const AppMenuItem &item) override {
        const auto args = appMenuArguments(app, item.id, item.target);
        if (!args.empty()) {
            return launchApplicationWithArgs(app, args);
        }
        if (item.target.empty()) {
            return false;
        }
        if (isWebAddress(item.target)) {
            return openPath(item.target);
        }
        return openPathWithApp(app, item.target);
    }

    bool isOpenAtLogin(const AppInfo &app) override {
        const auto path = autostartPath(app);
        std::error_code ec;
        return !path.empty() && std::filesystem::exists(path, ec);
    }

    bool setOpenAtLogin(const AppInfo &app, bool enabled) override {
        const auto path = autostartPath(app);
        if (path.empty()) {
            return false;
        }
        std::error_code ec;
        if (!enabled) {
            return std::filesystem::remove(path, ec);
        }
        if (std::filesystem::exists(path, ec)) {
            return true;
        }
        if (app.sourcePath.empty() || !std::filesystem::exists(app.sourcePath, ec)) {
            return false;
        }
        std::filesystem::create_directories(path.parent_path(), ec);
        std::filesystem::copy_file(utf8ToPath(app.sourcePath), path,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        return !ec;
    }

    std::string revealLabel() const override { return "Show in Files"; }

    bool openPath(const std::string &path) override { return spawnShellOpen(path); }

    bool openPathWithApp(const AppInfo &app, const std::string &path) override {
        if (path.empty()) {
            return false;
        }
        return launchApplicationWithArgs(app, {path});
    }

    bool revealPath(const std::string &path) override {
        const auto parent = std::filesystem::path(path).parent_path();
        return spawnShellOpen(parent.empty() ? path : parent.string());
    }

    bool openTrash() override {
        const std::filesystem::path trash = std::filesystem::path(userDataHome()) / "Trash" / "files";
        if (std::filesystem::exists(trash)) {
            return spawnShellOpen(trash.string());
        }
        return false;
    }

    bool emptyTrash() override {
        if (!findExecutableInPath("gio").empty()) {
            return spawnDetached({"gio", "trash", "--empty"});
        }
        return false;
    }

    bool openFileManager() override { return spawnShellOpen(std::filesystem::path(userDataHome()).string()); }

    void setSystemMinimizeAnimationsEnabled(bool) override {
        // On X11/Wayland the minimize effect belongs to the compositor/WM
        // (Mutter, KWin, ...); there is no portable per-application switch.
        // The Dock's own overlay still draws the Genie animation on top.
    }

    std::set<std::string> runningExecutables() override {
        std::set<std::string> result;
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator("/proc", ec);
             it != std::filesystem::directory_iterator(); it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            const auto name = it->path().filename().string();
            if (name.empty() || !std::all_of(name.begin(), name.end(), [](unsigned char c) {
                    return std::isdigit(c) != 0;
                })) {
                continue;
            }
            std::ifstream comm(it->path() / "comm");
            std::string processName;
            std::getline(comm, processName);
            if (!processName.empty()) {
                result.insert(lowerAscii(processName));
            }
        }
        return result;
    }

    bool fileManagerRunning() override {
        static const std::set<std::string> kFileManagers = {
            "nautilus", "dolphin",  "thunar", "pcmanfm", "nemo",
            "caja",     "krusader", "files",  "org.gnome.nautilus"};
        const std::set<std::string> running = runningExecutables();
        for (const auto &name : kFileManagers) {
            if (running.count(name) > 0) {
                return true;
            }
        }
        return false;
    }

    bool isFileManagerApp(const AppInfo &app) override {
        static const std::set<std::string> kFileManagers = {
            "nautilus", "dolphin",  "thunar", "pcmanfm", "nemo",
            "caja",     "krusader", "files"};
        const std::string exec =
            lowerAscii(std::filesystem::path(app.execPath).stem().string());
        return kFileManagers.count(exec) > 0 ||
               kFileManagers.count(lowerAscii(app.wmClass)) > 0;
    }

    std::string downloadsPath() override { return readDownloadDir(); }
    std::string trashLabel() override { return "Trash"; }

    bool globalCursor(float &x, float &y) override {
        float mx = 0, my = 0;
        SDL_MouseButtonFlags flags = SDL_GetGlobalMouseState(&mx, &my);
        (void)flags;
        x = mx;
        y = my;
        return true;
    }

    void prepareDockWindow(SDL_Window *window) override {
        window_ = window;
#ifdef REVDOCK_HAVE_X11
        SDL_PropertiesID props = SDL_GetWindowProperties(window);
        Display *display = static_cast<Display *>(
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
        const Window xwindow = static_cast<Window>(
            SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
        if (!display || !xwindow) {
            return;
        }
        Atom type = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
        Atom dock = XInternAtom(display, "_NET_WM_WINDOW_TYPE_DOCK", False);
        XChangeProperty(display, xwindow, type, XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<unsigned char *>(&dock), 1);

        Atom state = XInternAtom(display, "_NET_WM_STATE", False);
        Atom above = XInternAtom(display, "_NET_WM_STATE_ABOVE", False);
        Atom skipTaskbar = XInternAtom(display, "_NET_WM_STATE_SKIP_TASKBAR", False);
        Atom skipPager = XInternAtom(display, "_NET_WM_STATE_SKIP_PAGER", False);
        Atom states[] = {above, skipTaskbar, skipPager};
        XChangeProperty(display, xwindow, state, XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<unsigned char *>(states), 3);
        XFlush(display);
#endif
    }

    void setHitTestRects(SDL_Window *window, const std::vector<SDL_Rect> &rects) override {
#ifdef REVDOCK_HAVE_XSHAPE
        SDL_PropertiesID props = SDL_GetWindowProperties(window);
        Display *display = static_cast<Display *>(
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
        const Window xwindow = static_cast<Window>(
            SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
        if (!display || !xwindow) {
            return;
        }
        std::vector<XRectangle> xrects;
        xrects.reserve(rects.size());
        for (const auto &rect : rects) {
            xrects.push_back(XRectangle{static_cast<short>(rect.x), static_cast<short>(rect.y),
                                        static_cast<unsigned short>(std::max(1, rect.w)),
                                        static_cast<unsigned short>(std::max(1, rect.h))});
        }
        XShapeCombineRectangles(display, xwindow, ShapeInput, 0, 0, xrects.data(),
                                static_cast<int>(xrects.size()), ShapeSet, Unsorted);
        XFlush(display);
#else
        (void)window;
        (void)rects;
#endif
    }

    void reserveWorkArea(int displayIndex, const std::string &edge, int thickness, bool enable) override {
#ifdef REVDOCK_HAVE_X11
        if (!window_) {
            return;
        }
        SDL_PropertiesID props = SDL_GetWindowProperties(window_);
        Display *display = static_cast<Display *>(
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
        const Window xwindow = static_cast<Window>(
            SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
        if (!display || !xwindow) {
            return;
        }
        SDL_Rect bounds{};
        SDL_DisplayID displayId = SDL_GetPrimaryDisplay();
        SDL_GetDisplayBounds(displayId, &bounds);
        const unsigned long values[12] = {
            enable && edge == "left" ? static_cast<unsigned long>(thickness) : 0UL,
            enable && edge == "right" ? static_cast<unsigned long>(thickness) : 0UL,
            enable && edge == "top" ? static_cast<unsigned long>(thickness) : 0UL,
            enable && edge == "bottom" ? static_cast<unsigned long>(thickness) : 0UL,
            static_cast<unsigned long>(bounds.y),
            static_cast<unsigned long>(bounds.y + bounds.h - 1),
            static_cast<unsigned long>(bounds.y),
            static_cast<unsigned long>(bounds.y + bounds.h - 1),
            static_cast<unsigned long>(bounds.x),
            static_cast<unsigned long>(bounds.x + bounds.w - 1),
            static_cast<unsigned long>(bounds.x),
            static_cast<unsigned long>(bounds.x + bounds.w - 1),
        };
        Atom strut = XInternAtom(display, "_NET_WM_STRUT_PARTIAL", False);
        XChangeProperty(display, xwindow, strut, XA_CARDINAL, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(values), 12);
        XFlush(display);
#else
        (void)displayIndex;
        (void)edge;
        (void)thickness;
        (void)enable;
#endif
    }

private:
    SDL_Window *window_ = nullptr;

    static std::filesystem::path autostartPath(const AppInfo &app) {
        std::string name;
        if (!app.sourcePath.empty()) {
            const auto source = utf8ToPath(app.sourcePath);
            if (lowerAscii(source.extension().string()) == ".desktop") {
                name = source.filename().string();
            }
        }
        if (name.empty()) {
            name = app.name.empty() ? "application" : app.name;
            for (char &ch : name) {
                if (ch == '/' || ch == '\\') {
                    ch = '_';
                }
            }
            name += ".desktop";
        }
        const std::string config = userConfigHome();
        if (config.empty()) {
            return {};
        }
        return std::filesystem::path(config) / "autostart" / name;
    }

    static std::string currentDesktop() {
        const char *desktop = std::getenv("XDG_CURRENT_DESKTOP");
        if (!desktop || !*desktop) {
            return {};
        }
        std::string value = desktop;
        const size_t colon = value.find(':');
        if (colon != std::string::npos) {
            value = value.substr(0, colon);
        }
        return value;
    }
};

std::unique_ptr<Platform> createPlatform() {
    return std::make_unique<LinuxPlatform>();
}

} // namespace revdock
