#include "platform/Platform.h"

#include "apps/AppMenu.h"
#include "core/Log.h"
#include "core/Types.h"
#include "core/Utf.h"

#include <SDL3/SDL.h>

#define WIN32_LEAN_AND_MEAN
#include <appmodel.h>
#include <windows.h>
#include <windowsx.h>
#include <tlhelp32.h>
#include <imm.h>
#include <dwmapi.h>
#include <propkey.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_set>
#include <vector>

#ifndef WDA_NONE
#define WDA_NONE 0x00000000
#endif

namespace revdock {
namespace {

struct ComInit {
    HRESULT result = E_FAIL;
    ComInit() { result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); }
    ~ComInit() {
        if (SUCCEEDED(result)) {
            CoUninitialize();
        }
    }
};

std::wstring utf8ToPathWide(const std::string &path) {
    return utf8ToWide(path);
}

std::wstring quoteCommandLineArgument(const std::string &argument) {
    const std::wstring wide = utf8ToPathWide(argument);
    std::wstring quoted = L"\"";
    for (wchar_t ch : wide) {
        if (ch == L'"') {
            quoted.push_back(L'\\');
        }
        quoted.push_back(ch);
    }
    quoted.push_back(L'"');
    return quoted;
}

std::wstring joinCommandLineArguments(const std::vector<std::string> &arguments) {
    std::wstring result;
    for (const auto &argument : arguments) {
        if (!result.empty()) {
            result.push_back(L' ');
        }
        result += quoteCommandLineArgument(argument);
    }
    return result;
}

struct ShortcutInfo {
    std::string target;
    std::string arguments;
    std::string aumid;
};

std::string startMenuGroupFor(const std::filesystem::path &file,
                              const std::filesystem::path &root) {
    // The first folder below Programs is the group shown in the initial
    // Launchpad; deeper nesting collapses into that outermost name.
    const std::filesystem::path relative = file.lexically_relative(root);
    auto it = relative.begin();
    if (it == relative.end()) {
        return {};
    }
    const std::string first = wideToUtf8(it->wstring());
    if (first.empty() || first == "." || first == "..") {
        return {};
    }
    ++it;
    if (it == relative.end()) {
        return {};
    }
    return first;
}

std::string appPathForExecutable(const std::string &exeName) {
    if (exeName.empty()) {
        return {};
    }
    const std::wstring name = utf8ToPathWide(exeName);
    struct Root {
        HKEY key;
        const wchar_t *prefix;
    };
    const Root roots[] = {
        {HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\"},
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\"},
        {HKEY_LOCAL_MACHINE,
         L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\App Paths\\"},
    };
    for (const Root &root : roots) {
        const std::wstring subkey = std::wstring(root.prefix) + name;
        wchar_t buffer[32768];
        DWORD size = sizeof(buffer);
        DWORD type = 0;
        const LSTATUS status = RegGetValueW(
            root.key, subkey.c_str(), nullptr,
            RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &type, buffer,
            &size);
        if (status != ERROR_SUCCESS || size <= sizeof(wchar_t)) {
            continue;
        }
        std::wstring value = buffer;
        if (type == REG_EXPAND_SZ) {
            wchar_t expanded[32768];
            const DWORD needed = ExpandEnvironmentStringsW(
                value.c_str(), expanded, static_cast<DWORD>(std::size(expanded)));
            if (needed > 0 && needed <= std::size(expanded)) {
                value = expanded;
            }
        }
        std::error_code ec;
        if (std::filesystem::is_regular_file(value, ec)) {
            return wideToUtf8(value);
        }
    }
    return {};
}

// Office 2013/2016 Click-to-Run creates Start Menu shortcuts that point at
// Windows Installer "icon" stubs (wordicon.exe, xlicons.exe, ...).  Those
// stubs are not the program: launching them does not create a matching
// process/window, so the Dock bounced forever and never showed the running
// dot.  Resolve the stub to the real executable through the App Paths
// registry, like the Start menu does internally.
std::string resolveInstallerLauncher(const std::string &target) {
    if (target.empty()) {
        return {};
    }
    const std::filesystem::path path(utf8ToPath(target));
    const std::string parent = lowerAscii(wideToUtf8(path.parent_path().wstring()));
    if (parent.find("\\windows\\installer\\") == std::string::npos) {
        return {};
    }
    const std::string base = lowerAscii(wideToUtf8(path.filename().wstring()));
    static const std::map<std::string, std::string> kLauncherStubs = {
        {"accicons.exe", "MSACCESS.EXE"}, {"wordicon.exe", "WINWORD.EXE"},
        {"xlicons.exe", "EXCEL.EXE"},     {"pptico.exe", "POWERPNT.EXE"},
        {"outicon.exe", "OUTLOOK.EXE"},   {"pubs.exe", "MSPUB.EXE"},
        {"grv_icons.exe", "VISIO.EXE"},   {"joticon.exe", "ONENOTE.EXE"},
        {"lyncicon.exe", "LYNC.EXE"},     {"inficon.exe", "INFOPATH.EXE"},
        {"msouc.exe", "MSOUC.EXE"},
    };
    const auto found = kLauncherStubs.find(base);
    if (found == kLauncherStubs.end()) {
        return {};
    }
    return appPathForExecutable(found->second);
}

ShortcutInfo resolveShortcut(const std::filesystem::path &lnk) {
    ShortcutInfo info;
    IShellLinkW *link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, reinterpret_cast<void **>(&link))) ||
        !link) {
        return info;
    }
    IPersistFile *persist = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&persist))) && persist) {
        if (SUCCEEDED(persist->Load(lnk.wstring().c_str(), STGM_READ))) {
            wchar_t target[MAX_PATH * 2] = {};
            WIN32_FIND_DATAW data{};
            if (SUCCEEDED(link->GetPath(target, static_cast<int>(std::size(target)), &data, 0))) {
                info.target = wideToUtf8(target);
            }
            wchar_t arguments[4096] = {};
            if (SUCCEEDED(link->GetArguments(arguments, static_cast<int>(std::size(arguments))))) {
                info.arguments = wideToUtf8(arguments);
            }
        }
        persist->Release();
    }
    link->Release();

    // Packaged/UWP shortcuts are IDList-only: GetPath/GetArguments return
    // nothing, but the shell exposes the app's AUMID as TargetParsingPath.
    IPropertyStore *store = nullptr;
    if (SUCCEEDED(SHGetPropertyStoreFromParsingName(lnk.c_str(), nullptr, GPS_DEFAULT,
                                                    IID_PPV_ARGS(&store))) &&
        store) {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(store->GetValue(PKEY_Link_TargetParsingPath, &value)) &&
            value.vt == VT_LPWSTR && value.pwszVal) {
            const std::string parsed = wideToUtf8(value.pwszVal);
            if (parsed.find('!') != std::string::npos &&
                parsed.find('\\') == std::string::npos &&
                parsed.find('/') == std::string::npos) {
                info.aumid = parsed;
            }
        }
        PropVariantClear(&value);
        store->Release();
    }
    return info;
}

bool isCustomProtocol(const std::string &value) {
    const size_t separator = value.find("://");
    if (separator == std::string::npos || separator == 0) {
        return false;
    }
    const std::string scheme = lowerAscii(value.substr(0, separator));
    for (char c : scheme) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '-' && c != '.') {
            return false;
        }
    }
    // Web/file URLs are not applications; app-registered schemes (steam://,
    // bilibili://, ...) are.
    static const std::set<std::string> kWebSchemes = {"http", "https", "ftp", "ftps", "ws",
                                                      "wss",  "file",  "mailto"};
    return kWebSchemes.count(scheme) == 0;
}

bool isProgramTarget(const std::string &target) {
    static const std::set<std::string> kProgramExtensions = {
        ".exe", ".com", ".bat", ".cmd", ".ps1", ".vbs", ".jar", ".msi", ".appref-ms",
    };
    if (target.empty()) {
        return false;
    }
    const std::filesystem::path path(utf8ToWide(target));
    const std::string fileName = lowerAscii(wideToUtf8(path.filename().wstring()));
    // Installers / uninstallers are not applications the user wants to launch
    // from Launchpad.
    if (fileName == "msiexec.exe" || fileName == "msiexec" ||
        fileName.rfind("uninst", 0) == 0) {
        return false;
    }
    const std::string extension = lowerAscii(path.extension().string());
    return kProgramExtensions.count(extension) > 0;
}

std::string windowAumid(HWND hwnd) {
    // ApplicationFrameHost hosts many UWP windows, so the process AUMID is not
    // always the app's. The window property store carries the hosted app's
    // AppUserModelID reliably.
    IPropertyStore *store = nullptr;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store))) && store) {
        std::string fromWindow;
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &value)) &&
            value.vt == VT_LPWSTR && value.pwszVal) {
            fromWindow = wideToUtf8(value.pwszVal);
        }
        PropVariantClear(&value);
        store->Release();
        if (!fromWindow.empty()) {
            return fromWindow;
        }
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || pid == GetCurrentProcessId()) {
        return {};
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return {};
    }
    UINT32 length = 0;
    std::string result;
    if (GetApplicationUserModelId(process, &length, nullptr) == ERROR_INSUFFICIENT_BUFFER &&
        length > 0) {
        std::wstring buffer(length, L'\0');
        if (GetApplicationUserModelId(process, &length, buffer.data()) == ERROR_SUCCESS) {
            result = wideToUtf8(buffer.c_str());
        }
    }
    CloseHandle(process);
    return result;
}

std::string windowProcessPath(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || pid == GetCurrentProcessId()) {
        return {};
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return {};
    }
    wchar_t buffer[32768];
    DWORD size = static_cast<DWORD>(std::size(buffer));
    std::string result;
    if (QueryFullProcessImageNameW(process, 0, buffer, &size)) {
        result = wideToUtf8(buffer);
    }
    CloseHandle(process);
    return result;
}

bool isCloaked(HWND hwnd) {
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        return cloaked != 0;
    }
    return false;
}

void downscaleRGBA(std::vector<uint8_t> &pixels, int &width, int &height, int maxSide) {
    if (width <= 0 || height <= 0 || maxSide <= 0) {
        return;
    }
    const int longest = std::max(width, height);
    if (longest <= maxSide) {
        return;
    }
    const float factor = static_cast<float>(maxSide) / static_cast<float>(longest);
    const int dstW = std::max(1, static_cast<int>(std::lround(width * factor)));
    const int dstH = std::max(1, static_cast<int>(std::lround(height * factor)));
    std::vector<uint8_t> scaled(static_cast<size_t>(dstW) * dstH * 4, 0);
    for (int y = 0; y < dstH; ++y) {
        const int sy0 = y * height / dstH;
        const int sy1 = std::max(sy0 + 1, (y + 1) * height / dstH);
        for (int x = 0; x < dstW; ++x) {
            const int sx0 = x * width / dstW;
            const int sx1 = std::max(sx0 + 1, (x + 1) * width / dstW);
            uint32_t r = 0, g = 0, b = 0, a = 0;
            int count = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                for (int sx = sx0; sx < sx1; ++sx) {
                    const size_t index = (static_cast<size_t>(sy) * width + sx) * 4;
                    r += pixels[index + 0];
                    g += pixels[index + 1];
                    b += pixels[index + 2];
                    a += pixels[index + 3];
                    ++count;
                }
            }
            if (count > 0) {
                const size_t index = (static_cast<size_t>(y) * dstW + x) * 4;
                scaled[index + 0] = static_cast<uint8_t>(r / count);
                scaled[index + 1] = static_cast<uint8_t>(g / count);
                scaled[index + 2] = static_cast<uint8_t>(b / count);
                scaled[index + 3] = static_cast<uint8_t>(a / count);
            }
        }
    }
    pixels = std::move(scaled);
    width = dstW;
    height = dstH;
}

bool imageHasDetail(const std::vector<uint8_t> &rgba, int width, int height) {
    int minValue = 255;
    int maxValue = 0;
    const int stepX = std::max(1, width / 24);
    const int stepY = std::max(1, height / 24);
    for (int y = 0; y < height; y += stepY) {
        for (int x = 0; x < width; x += stepX) {
            const size_t index = (static_cast<size_t>(y) * width + x) * 4;
            const int value = (rgba[index] + rgba[index + 1] + rgba[index + 2]) / 3;
            minValue = std::min(minValue, value);
            maxValue = std::max(maxValue, value);
        }
    }
    return maxValue - minValue > 6;
}

bool blitScreenRect(const RECT &rect, std::vector<uint8_t> &rgba, int width, int height) {
    if (width < 24 || height < 24) {
        return false;
    }
    HDC screen = GetDC(nullptr);
    if (!screen) {
        return false;
    }
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (memory && bitmap && bits) {
        HGDIOBJ old = SelectObject(memory, bitmap);
        if (BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top,
                   SRCCOPY | CAPTUREBLT)) {
            const auto *bgra = static_cast<const uint8_t *>(bits);
            rgba.resize(static_cast<size_t>(width) * height * 4);
            for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
                rgba[i * 4 + 0] = bgra[i * 4 + 2];
                rgba[i * 4 + 1] = bgra[i * 4 + 1];
                rgba[i * 4 + 2] = bgra[i * 4 + 0];
                rgba[i * 4 + 3] = 255;
            }
            ok = true;
        }
        SelectObject(memory, old);
    }
    if (bitmap) {
        DeleteObject(bitmap);
    }
    if (memory) {
        DeleteDC(memory);
    }
    ReleaseDC(nullptr, screen);
    return ok;
}

bool printWindowRect(HWND hwnd, std::vector<uint8_t> &rgba, int width, int height) {
    if (width < 24 || height < 24) {
        return false;
    }
    HDC screen = GetDC(nullptr);
    if (!screen) {
        return false;
    }
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (memory && bitmap && bits) {
        HGDIOBJ old = SelectObject(memory, bitmap);
        if (PrintWindow(hwnd, memory, PW_RENDERFULLCONTENT) ||
            PrintWindow(hwnd, memory, 0)) {
            const auto *bgra = static_cast<const uint8_t *>(bits);
            rgba.resize(static_cast<size_t>(width) * height * 4);
            for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
                rgba[i * 4 + 0] = bgra[i * 4 + 2];
                rgba[i * 4 + 1] = bgra[i * 4 + 1];
                rgba[i * 4 + 2] = bgra[i * 4 + 0];
                rgba[i * 4 + 3] = 255;
            }
            ok = true;
        }
        SelectObject(memory, old);
    }
    if (bitmap) {
        DeleteObject(bitmap);
    }
    if (memory) {
        DeleteDC(memory);
    }
    ReleaseDC(nullptr, screen);
    return ok;
}

bool captureWindowPixels(HWND hwnd, std::vector<uint8_t> &rgba, int &width, int &height) {
    // A minimized window reports (-32000,-32000), so use the restored rect.
    RECT rect{};
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (GetWindowPlacement(hwnd, &placement)) {
        rect = placement.rcNormalPosition;
    }
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;
    if (width < 24 || height < 24) {
        if (!GetWindowRect(hwnd, &rect)) {
            return false;
        }
        width = rect.right - rect.left;
        height = rect.bottom - rect.top;
    }
    if (width < 24 || height < 24) {
        return false;
    }
    // The MINIMIZESTART callback fires after Windows hides the window, so the
    // screen no longer contains its pixels. Prefer WM_PRINT; a screenshot cache
    // of the foreground window (refreshThumbnailCache) is used when available.
    std::vector<uint8_t> printed;
    if (!printWindowRect(hwnd, printed, width, height) ||
        !imageHasDetail(printed, width, height)) {
        return false;
    }
    rgba = std::move(printed);
    downscaleRGBA(rgba, width, height, 640);
    return true;
}

bool isHelperWindowClass(const std::string &cls) {
    static const std::set<std::string> kHelperClasses = {
        "IME", "MSCTFIME UI", "Base_PowerMessageWindow",
        "Electron_NotifyIconHostWindow", "tooltips_class32",
        "TaskListThumbnailWnd", "ForegroundStaging", "MultitaskingViewFrame",
        "Shell_TrayWnd", "Shell_SecondaryTrayWnd", "Progman", "WorkerW",
        "tray_icon_app"};
    if (kHelperClasses.count(cls) > 0) {
        return true;
    }
    return cls.rfind("IME", 0) == 0 || cls.rfind("MSCTF", 0) == 0;
}

std::string windowClassOf(HWND hwnd) {
    wchar_t name[256] = {};
    GetClassNameW(hwnd, name, static_cast<int>(std::size(name)));
    return wideToUtf8(name);
}

// Smallest window we are willing to bring to the front.  Tray/notification
// helper windows are 16x16 or 48x39; real app windows are much larger.
bool isUsableAppWindow(HWND hwnd) {
    if (!IsWindow(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr || isCloaked(hwnd)) {
        return false;
    }
    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) {
        return false;
    }
    const long long width = rect.right - rect.left;
    const long long height = rect.bottom - rect.top;
    if (width < 80 || height < 60 || width * height < 12000) {
        return false;
    }
    wchar_t className[256] = {};
    GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    return !isHelperWindowClass(wideToUtf8(className));
}

struct AppWindowSearch {
    std::string wantedLower;
    HWND visible = nullptr;
    HWND hidden = nullptr;
    long long hiddenScore = -1;
};

BOOL CALLBACK enumAppWindows(HWND hwnd, LPARAM param) {
    auto *search = reinterpret_cast<AppWindowSearch *>(param);
    if (!isUsableAppWindow(hwnd)) {
        return TRUE;
    }
    if (lowerAscii(windowProcessPath(hwnd)) != search->wantedLower) {
        return TRUE;
    }
    const int titleLength = GetWindowTextLengthW(hwnd);
    if (IsWindowVisible(hwnd) && titleLength > 0) {
        if (!search->visible) {
            search->visible = hwnd;
        }
        return TRUE;
    }
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    const long long area = static_cast<long long>(rect.right - rect.left) *
                           static_cast<long long>(rect.bottom - rect.top);
    const long long score = area + (titleLength > 0 ? (1LL << 40) : 0);
    if (score > search->hiddenScore) {
        search->hiddenScore = score;
        search->hidden = hwnd;
    }
    return TRUE;
}

struct AppWindowCollect {
    std::string wantedLower;
    std::string aumidLower;
    std::vector<HWND> windows;
};

BOOL CALLBACK collectAppWindows(HWND hwnd, LPARAM param) {
    auto *state = reinterpret_cast<AppWindowCollect *>(param);
    if (GetWindow(hwnd, GW_OWNER) != nullptr) {
        return TRUE;
    }
    bool matches = false;
    if (!state->wantedLower.empty() &&
        lowerAscii(windowProcessPath(hwnd)) == state->wantedLower) {
        matches = true;
    }
    if (!matches && !state->aumidLower.empty() &&
        lowerAscii(windowAumid(hwnd)) == state->aumidLower) {
        matches = true;
    }
    if (matches) {
        state->windows.push_back(hwnd);
    }
    return TRUE;
}

std::vector<HWND> appTopLevelWindows(const std::string &wantedLower,
                                     const std::string &aumidLower) {
    AppWindowCollect state{wantedLower, aumidLower, {}};
    EnumWindows(collectAppWindows, reinterpret_cast<LPARAM>(&state));
    return std::move(state.windows);
}

void forceWindowToFront(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return;
    }
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    } else {
        ShowWindow(hwnd, SW_SHOW);
    }
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    const DWORD myThread = GetCurrentThreadId();
    HWND foreground = GetForegroundWindow();
    const DWORD foregroundThread =
        foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD appThread = GetWindowThreadProcessId(hwnd, nullptr);
    const bool attachForeground =
        foregroundThread != 0 && foregroundThread != myThread;
    const bool attachApp = appThread != 0 && appThread != myThread &&
                           appThread != foregroundThread;
    if (attachForeground) {
        AttachThreadInput(myThread, foregroundThread, TRUE);
    }
    if (attachApp) {
        AttachThreadInput(myThread, appThread, TRUE);
    }
    // Lift the foreground lock left behind by the no-activate Dock click.
    keybd_event(VK_MENU, 0, 0, 0);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    if (attachApp) {
        AttachThreadInput(myThread, appThread, FALSE);
    }
    if (attachForeground) {
        AttachThreadInput(myThread, foregroundThread, FALSE);
    }
}

void removeEdgeWhiteMatte(SDL_Surface *surface) {
    if (!surface || surface->w <= 0 || surface->h <= 0) {
        return;
    }
    SDL_LockSurface(surface);
    const int width = surface->w;
    const int height = surface->h;
    const int pitch = surface->pitch / 4;
    auto *pixels = static_cast<uint32_t *>(surface->pixels);
    std::vector<uint8_t> visited(static_cast<size_t>(width) * height, 0);
    std::vector<std::pair<int, int>> stack;
    auto isWhite = [&](int x, int y) {
        const uint32_t value = pixels[static_cast<size_t>(y) * pitch + x];
        const int b = static_cast<int>(value & 0xff);
        const int g = static_cast<int>((value >> 8) & 0xff);
        const int r = static_cast<int>((value >> 16) & 0xff);
        return b >= 235 && g >= 235 && r >= 235;
    };
    auto push = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= width || y >= height) {
            return;
        }
        const size_t index = static_cast<size_t>(y) * width + x;
        if (!visited[index] && isWhite(x, y)) {
            visited[index] = 1;
            stack.emplace_back(x, y);
        }
    };
    for (int x = 0; x < width; ++x) {
        push(x, 0);
        push(x, height - 1);
    }
    for (int y = 0; y < height; ++y) {
        push(0, y);
        push(width - 1, y);
    }
    while (!stack.empty()) {
        const auto [x, y] = stack.back();
        stack.pop_back();
        uint32_t &value = pixels[static_cast<size_t>(y) * pitch + x];
        value &= 0x00ffffffu;
        push(x - 1, y);
        push(x + 1, y);
        push(x, y - 1);
        push(x, y + 1);
    }
    SDL_UnlockSurface(surface);
}

SDL_Surface *surfaceFromHBitmap(HBITMAP bitmap) {
    BITMAP bm{};
    if (!GetObject(bitmap, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) {
        return nullptr;
    }
    const int w = bm.bmWidth;
    const int h = bm.bmHeight;
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4, 0);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(nullptr);
    const int lines = GetDIBits(dc, bitmap, 0, h, pixels.data(), &info, DIB_RGB_COLORS);
    DeleteDC(dc);
    if (lines == 0) {
        return nullptr;
    }

    bool anyAlpha = false;
    for (size_t i = 3; i < pixels.size(); i += 4) {
        if (pixels[i] != 0) {
            anyAlpha = true;
            break;
        }
    }
    if (!anyAlpha) {
        for (size_t i = 3; i < pixels.size(); i += 4) {
            pixels[i] = 255;
        }
    }

    SDL_Surface *surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) {
        return nullptr;
    }
    if (!SDL_ConvertPixels(w, h, SDL_PIXELFORMAT_BGRA32, pixels.data(), w * 4,
                           SDL_PIXELFORMAT_ARGB8888, surface->pixels, surface->pitch)) {
        SDL_DestroySurface(surface);
        return nullptr;
    }
    if (!anyAlpha) {
        removeEdgeWhiteMatte(surface);
    }
    return surface;
}

SDL_Surface *shellIcon(const std::string &path, int pixelSize) {
    if (path.empty()) {
        return nullptr;
    }
    const std::wstring wide = utf8ToPathWide(path);
    IShellItem *item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(wide.c_str(), nullptr, IID_IShellItem,
                                           reinterpret_cast<void **>(&item))) ||
        !item) {
        return nullptr;
    }
    IShellItemImageFactory *factory = nullptr;
    SDL_Surface *result = nullptr;
    if (SUCCEEDED(item->QueryInterface(IID_IShellItemImageFactory,
                                       reinterpret_cast<void **>(&factory))) &&
        factory) {
        HBITMAP bitmap = nullptr;
        SIZE size{std::max(16, pixelSize), std::max(16, pixelSize)};
        if (SUCCEEDED(factory->GetImage(size, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bitmap)) &&
            bitmap) {
            result = surfaceFromHBitmap(bitmap);
            DeleteObject(bitmap);
        }
        factory->Release();
    }
    item->Release();
    return result;
}

struct UrlShortcutInfo {
    std::string url;
    std::string iconFile;
};

UrlShortcutInfo parseUrlShortcut(const std::filesystem::path &path) {
    UrlShortcutInfo info;
    std::ifstream file(path);
    if (!file) {
        return info;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("URL=", 0) == 0) {
            info.url = line.substr(4);
        } else if (line.rfind("IconFile=", 0) == 0) {
            info.iconFile = line.substr(9);
        }
    }
    return info;
}

std::string pickDesktopName(const std::string &stem) {
    if (stem.empty()) {
        return "Application";
    }
    return stem;
}

} // namespace

class WindowsPlatform final : public Platform {
public:
    // The overlay/thumbnail pipeline is kept for the upcoming Genie/Scale
    // minimize animation, but is disabled until it can capture GPU windows
    // reliably on every desktop composition mode. Minimized-window thumbnails
    // in the Dock use the same capture path but do not need the overlay.
    static constexpr bool kMinimizeAnimationEnabled = true;
    static constexpr bool kMinimizedWindowTilesEnabled = true;

    WindowsPlatform() : com_() {}
    ~WindowsPlatform() override {
        setSystemMinimizeAnimationsEnabled(true);
        releaseCaptureSurface();
        if (winEventHook_) {
            UnhookWinEvent(winEventHook_);
            winEventHook_ = nullptr;
        }
        if (appBarRegistered_) {
            APPBARDATA data{};
            data.cbSize = sizeof(data);
            data.hWnd = appBarHwnd_;
            SHAppBarMessage(ABM_REMOVE, &data);
        }
        restoreTaskbarState();
    }

    std::vector<AppInfo> scanApplications() override {
        std::vector<AppInfo> apps;
        std::unordered_set<std::string> seen;

        const std::vector<std::filesystem::path> roots = startMenuRoots();
        for (const auto &root : roots) {
            scanRoot(root, apps, seen);
        }
        // Start Menu folders do not always contain files for packaged (UWP)
        // apps. Enumerate the virtual AppsFolder so Launchpad can list them.
        scanPackagedApps(apps, seen);

        std::sort(apps.begin(), apps.end(), [](const AppInfo &a, const AppInfo &b) {
            return lowerAscii(a.name) < lowerAscii(b.name);
        });
        RD_LOG_INFO("Scanned %zu applications", apps.size());
        return apps;
    }

    SDL_Surface *loadIcon(const AppInfo &app, int pixelSize) override {
        // The shortcut carries the icon the Start Menu intends to show. Use the
        // resolved executable only as a fallback; resolving some documentation
        // shortcuts would otherwise show the browser's icon.
        if (!app.sourcePath.empty()) {
            if (SDL_Surface *surface = shellIcon(app.sourcePath, pixelSize)) {
                return surface;
            }
        }
        if (!app.execPath.empty()) {
            if (SDL_Surface *surface = shellIcon(app.execPath, pixelSize)) {
                return surface;
            }
        }
        return nullptr;
    }

    SDL_Surface *loadIconPath(const std::string &path, int pixelSize) override {
        return shellIcon(path, pixelSize);
    }

    // Launches/activates a packaged (UWP/MSIX) app by AUMID. With a file path
    // it asks the app to open that file through its registered file verb.
    bool activateUwp(const std::string &aumid, const std::string &filePath = {}) {
        if (aumid.empty()) {
            return false;
        }
        IApplicationActivationManager *manager = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr,
                                      CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager));
        if (FAILED(hr) || !manager) {
            return false;
        }
        const std::wstring wide = utf8ToPathWide(aumid);
        DWORD processId = 0;
        if (filePath.empty()) {
            hr = manager->ActivateApplication(wide.c_str(), nullptr, AO_NONE, &processId);
        } else {
            IShellItem *item = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(utf8ToPathWide(filePath).c_str(), nullptr,
                                                      IID_PPV_ARGS(&item))) &&
                item) {
                IShellItemArray *items = nullptr;
                if (SUCCEEDED(SHCreateShellItemArrayFromShellItem(item, IID_PPV_ARGS(&items))) &&
                    items) {
                    hr = manager->ActivateForFile(wide.c_str(), items, L"open", &processId);
                    items->Release();
                }
                item->Release();
            } else {
                hr = E_FAIL;
            }
        }
        manager->Release();
        if (FAILED(hr)) {
            RD_LOG_DEBUG("UWP activation failed for %s (0x%08lx)", aumid.c_str(),
                         static_cast<unsigned long>(hr));
            return false;
        }
        return true;
    }

    bool launchApplication(const AppInfo &app) override {
        if (!app.aumid.empty()) {
            return activateUwp(app.aumid);
        }
        const std::string target =
            (app.launchExec && !app.execPath.empty())
                ? app.execPath
                : (!app.sourcePath.empty() ? app.sourcePath : app.execPath);
        return shellOpen(target, {});
    }

    bool launchItem(const DockItem &item) override {
        switch (item.type) {
        case ItemType::Application:
        case ItemType::Folder:
        case ItemType::File:
        case ItemType::Url:
            return shellOpen(item.path, {});
        case ItemType::Trash:
            return openTrash();
        case ItemType::MinimizedWindow:
            return restoreWindow(item.windowHandle);
        case ItemType::Finder:
            return openFileManager();
        case ItemType::Separator:
        case ItemType::AppGrid:
            return false;
        }
        return false;
    }

    bool activateApplication(const AppInfo &app) override {
        if (!app.aumid.empty()) {
            // ActivateApplication focuses an existing instance or launches one.
            return activateUwp(app.aumid);
        }
        if (app.execPath.empty()) {
            return false;
        }
        const std::string wanted = lowerAscii(app.execPath);
        HWND firstHidden = nullptr;
        for (auto it = hiddenAppWindows_.begin(); it != hiddenAppWindows_.end();) {
            HWND hwnd = *it;
            if (!IsWindow(hwnd)) {
                it = hiddenAppWindows_.erase(it);
                continue;
            }
            if (lowerAscii(windowProcessPath(hwnd)) == wanted) {
                ShowWindow(hwnd, SW_SHOW);
                if (!firstHidden) {
                    firstHidden = hwnd;
                }
            }
            ++it;
        }
        if (firstHidden) {
            forceWindowToFront(firstHidden);
            return true;
        }
        AppWindowSearch search{wanted, nullptr, nullptr, -1};
        EnumWindows(enumAppWindows, reinterpret_cast<LPARAM>(&search));
        HWND target = search.visible ? search.visible : search.hidden;
        if (!target) {
            return false;
        }
        forceWindowToFront(target);
        return true;
    }

    bool showAllWindows(const AppInfo &app) override {
        if (app.execPath.empty()) {
            return false;
        }
        const std::string wanted = lowerAscii(app.execPath);
        for (auto it = hiddenAppWindows_.begin(); it != hiddenAppWindows_.end();) {
            HWND hwnd = *it;
            if (!IsWindow(hwnd)) {
                it = hiddenAppWindows_.erase(it);
                continue;
            }
            if (lowerAscii(windowProcessPath(hwnd)) == wanted) {
                ShowWindow(hwnd, SW_SHOW);
            }
            ++it;
        }
        struct ShowSearch {
            std::string wanted;
            HWND first = nullptr;
            int count = 0;
        } search{wanted, nullptr, 0};
        EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
            auto *state = reinterpret_cast<ShowSearch *>(param);
            if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr ||
                isCloaked(hwnd)) {
                return TRUE;
            }
            if (lowerAscii(windowProcessPath(hwnd)) != state->wanted) {
                return TRUE;
            }
            if (IsIconic(hwnd)) {
                ShowWindow(hwnd, SW_RESTORE);
            }
            if (!state->first) {
                state->first = hwnd;
            }
            ++state->count;
            return TRUE;
        }, reinterpret_cast<LPARAM>(&search));
        if (search.first) {
            SetForegroundWindow(search.first);
            return true;
        }
        AppWindowSearch hidden{lowerAscii(app.execPath), nullptr, nullptr, -1};
        EnumWindows(enumAppWindows, reinterpret_cast<LPARAM>(&hidden));
        HWND target = hidden.visible ? hidden.visible : hidden.hidden;
        if (!target) {
            return false;
        }
        forceWindowToFront(target);
        return true;
    }

    bool minimizeApplication(const AppInfo &app) override {
        if (app.execPath.empty()) {
            return false;
        }
        struct MinimizeSearch {
            std::string wanted;
            int count = 0;
        } search{lowerAscii(app.execPath), 0};
        EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
            auto *state = reinterpret_cast<MinimizeSearch *>(param);
            if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) {
                return TRUE;
            }
            if (lowerAscii(windowProcessPath(hwnd)) != state->wanted) {
                return TRUE;
            }
            ShowWindow(hwnd, SW_MINIMIZE);
            ++state->count;
            return TRUE;
        }, reinterpret_cast<LPARAM>(&search));
        return search.count > 0;
    }

    bool quitApplication(const AppInfo &app) override {
        // Graceful "quit application", not "close this window".  We ask every
        // top-level window to end the session (the same path Windows uses for
        // a real shutdown/logoff), which lets the app save/prompt and then
        // terminate itself; tray-only apps exit as well.  Nothing is ever
        // terminated from here - Force Quit is the separate, explicit action.
        const std::string wanted = app.execPath.empty()
                                       ? std::string()
                                       : lowerAscii(app.execPath);
        const std::string aumid = app.aumid.empty() ? std::string()
                                                    : lowerAscii(app.aumid);
        if (wanted.empty() && aumid.empty()) {
            return false;
        }
        const std::vector<HWND> windows = appTopLevelWindows(wanted, aumid);
        if (windows.empty()) {
            return false;
        }
        std::thread([windows]() {
            if (windows.empty()) {
                return;
            }
            bool vetoed = false;
            bool queried = false;
            for (HWND hwnd : windows) {
                if (!IsWindow(hwnd)) {
                    continue;
                }
                DWORD_PTR result = 0;
                const LRESULT sent = SendMessageTimeoutW(
                    hwnd, WM_QUERYENDSESSION, ENDSESSION_CLOSEAPP, 0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 12000, &result);
                if (sent == 0) {
                    // Unresponsive window: leave it alone (Force Quit is the
                    // explicit escape hatch), unless it is a hidden helper.
                    if (IsWindowVisible(hwnd) || GetWindowTextLengthW(hwnd) > 0) {
                        vetoed = true;
                    }
                    continue;
                }
                queried = true;
                if (result == 0 &&
                    !isHelperWindowClass(windowClassOf(hwnd))) {
                    vetoed = true;
                }
            }
            if (!queried || vetoed) {
                // The app (or an unsaved-changes prompt) refused to quit.
                return;
            }
            for (HWND hwnd : windows) {
                if (IsWindow(hwnd)) {
                    SendMessageTimeoutW(hwnd, WM_ENDSESSION, TRUE, 0,
                                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 6000,
                                        nullptr);
                }
            }
            // Apps that ignore WM_ENDSESSION still get the normal window-close
            // path after a moment.  Still no forced termination.
            Sleep(1200);
            for (HWND hwnd : windows) {
                if (IsWindow(hwnd)) {
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                }
            }
        }).detach();
        return true;
    }

    bool forceQuitApplication(const AppInfo &app) override {
        if (app.execPath.empty()) {
            return false;
        }
        struct ForceSearch {
            std::string wanted;
            bool killed = false;
        } search{lowerAscii(app.execPath), false};
        EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
            auto *state = reinterpret_cast<ForceSearch *>(param);
            if (lowerAscii(windowProcessPath(hwnd)) != state->wanted) {
                return TRUE;
            }
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == 0) {
                return TRUE;
            }
            HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
            if (process) {
                if (TerminateProcess(process, 1)) {
                    state->killed = true;
                }
                CloseHandle(process);
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&search));
        return search.killed;
    }

    bool launchApplicationWithArgs(const AppInfo &app,
                                  const std::vector<std::string> &args) override {
        if (args.empty()) {
            return launchApplication(app);
        }
        if (!app.aumid.empty()) {
            // Packaged apps do not expose arbitrary command-line arguments;
            // their jump-list tasks are handled by the shell.
            return false;
        }
        const std::string target =
            !app.execPath.empty() ? app.execPath : app.sourcePath;
        if (target.empty()) {
            return false;
        }
        return shellOpen(target, joinCommandLineArguments(args));
    }

    std::vector<AppMenuItem> appMenuItems(const AppInfo &app) override {
        std::vector<AppMenuItem> items = appProfileMenuItems(app, 8);
        const bool hasRecent =
            std::any_of(items.begin(), items.end(), [](const AppMenuItem &item) {
                return item.recent;
            });
        if (!hasRecent) {
            // Other applications expose their recent/frequent destinations
            // through the Windows jump list.
            IApplicationDocumentLists *lists = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_ApplicationDocumentLists, nullptr,
                                           CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&lists))) &&
                lists) {
                std::string id = app.aumid;
                if (id.empty()) {
                    id = !app.execPath.empty() ? app.execPath : app.sourcePath;
                }
                if (!id.empty()) {
                    const std::wstring wideId = utf8ToPathWide(id);
                    if (SUCCEEDED(lists->SetAppID(wideId.c_str()))) {
                        std::set<std::string> seen;
                        for (int type = 0; type < 2 &&
                                           static_cast<int>(items.size()) < 16; ++type) {
                            IObjectArray *array = nullptr;
                            if (FAILED(lists->GetList(
                                    static_cast<APPDOCLISTTYPE>(type), 12,
                                    IID_IObjectArray,
                                    reinterpret_cast<void **>(&array))) ||
                                !array) {
                                continue;
                            }
                            UINT count = 0;
                            array->GetCount(&count);
                            for (UINT i = 0; i < count; ++i) {
                                IShellItem *shellItem = nullptr;
                                if (FAILED(array->GetAt(i, IID_PPV_ARGS(&shellItem))) ||
                                    !shellItem) {
                                    continue;
                                }
                                std::string label;
                                std::string path;
                                PWSTR display = nullptr;
                                if (SUCCEEDED(shellItem->GetDisplayName(
                                        SIGDN_NORMALDISPLAY, &display)) &&
                                    display) {
                                    label = wideToUtf8(display);
                                    CoTaskMemFree(display);
                                }
                                PWSTR parsing = nullptr;
                                if (SUCCEEDED(shellItem->GetDisplayName(
                                        SIGDN_DESKTOPABSOLUTEPARSING, &parsing)) &&
                                    parsing) {
                                    path = wideToUtf8(parsing);
                                    CoTaskMemFree(parsing);
                                }
                                shellItem->Release();
                                if (path.empty() || !seen.insert(lowerAscii(path)).second) {
                                    continue;
                                }
                                AppMenuItem entry;
                                entry.id = "recent";
                                entry.label = label.empty() ? path : label;
                                entry.target = path;
                                const std::filesystem::path fsPath = utf8ToPath(path);
                                entry.detail =
                                    pathToUtf8(fsPath.parent_path().filename());
                                entry.recent = true;
                                items.push_back(std::move(entry));
                                if (items.size() >= 16) {
                                    break;
                                }
                            }
                            array->Release();
                        }
                    }
                }
                lists->Release();
            }
        }
        return items;
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

    bool isApplicationResponding(const AppInfo &app) override {
        if (app.execPath.empty()) {
            return true;
        }
        struct RespondSearch {
            std::string wanted;
            HWND found = nullptr;
        } search{lowerAscii(app.execPath), nullptr};
        EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
            auto *state = reinterpret_cast<RespondSearch *>(param);
            if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr ||
                lowerAscii(windowProcessPath(hwnd)) != state->wanted) {
                return TRUE;
            }
            state->found = hwnd;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&search));
        if (!search.found) {
            return true;
        }
        DWORD_PTR result = 0;
        const LRESULT sent = SendMessageTimeoutW(search.found, WM_NULL, 0, 0,
                                                 SMTO_ABORTIFHUNG | SMTO_BLOCK, 120,
                                                 &result);
        return sent != 0 && !IsHungAppWindow(search.found);
    }

    bool canHideApplications() const override { return true; }

    bool hideApplication(const AppInfo &app) override {
        if (app.execPath.empty()) {
            return false;
        }
        struct HideSearch {
            std::string wanted;
            std::set<HWND> *hidden = nullptr;
            int count = 0;
        } search{lowerAscii(app.execPath), &hiddenAppWindows_, 0};
        EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
            auto *state = reinterpret_cast<HideSearch *>(param);
            if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr ||
                lowerAscii(windowProcessPath(hwnd)) != state->wanted) {
                return TRUE;
            }
            ShowWindow(hwnd, SW_HIDE);
            state->hidden->insert(hwnd);
            ++state->count;
            return TRUE;
        }, reinterpret_cast<LPARAM>(&search));
        return search.count > 0;
    }

    bool hideOtherApplications(const AppInfo &app) override {
        struct HideOthers {
            std::string except;
            HWND dock = nullptr;
            std::set<HWND> *hidden = nullptr;
            int count = 0;
        } search{lowerAscii(app.execPath), dockHwnd_, &hiddenAppWindows_, 0};
        EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
            auto *state = reinterpret_cast<HideOthers *>(param);
            if (!IsWindowVisible(hwnd) || IsIconic(hwnd) ||
                GetWindow(hwnd, GW_OWNER) != nullptr || hwnd == state->dock ||
                GetWindowTextLengthW(hwnd) <= 0) {
                return TRUE;
            }
            const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            if ((exStyle & WS_EX_TOOLWINDOW) != 0) {
                return TRUE;
            }
            const std::string process = lowerAscii(windowProcessPath(hwnd));
            if (process.empty() || process == state->except ||
                process.find("explorer.exe") != std::string::npos ||
                process.find("revdock.exe") != std::string::npos) {
                return TRUE;
            }
            wchar_t className[128] = {};
            GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
            const std::string cls = wideToUtf8(className);
            if (cls == "Progman" || cls == "WorkerW" || cls == "Shell_TrayWnd" ||
                cls == "Shell_SecondaryTrayWnd") {
                return TRUE;
            }
            ShowWindow(hwnd, SW_HIDE);
            state->hidden->insert(hwnd);
            ++state->count;
            return TRUE;
        }, reinterpret_cast<LPARAM>(&search));
        return search.count > 0;
    }

    bool isApplicationHidden(const AppInfo &app) override {
        if (app.execPath.empty()) {
            return false;
        }
        const std::string wanted = lowerAscii(app.execPath);
        for (auto it = hiddenAppWindows_.begin(); it != hiddenAppWindows_.end();) {
            HWND hwnd = *it;
            if (!IsWindow(hwnd)) {
                it = hiddenAppWindows_.erase(it);
                continue;
            }
            if (lowerAscii(windowProcessPath(hwnd)) == wanted) {
                return true;
            }
            ++it;
        }
        return false;
    }

    bool isOpenAtLogin(const AppInfo &app) override {
        const auto path = startupShortcutPath(app);
        std::error_code ec;
        return !path.empty() && std::filesystem::exists(path, ec);
    }

    bool setOpenAtLogin(const AppInfo &app, bool enabled) override {
        const auto path = startupShortcutPath(app);
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
        if (!app.sourcePath.empty() && std::filesystem::exists(app.sourcePath, ec)) {
            std::filesystem::copy_file(utf8ToPath(app.sourcePath), path,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            return !ec;
        }
        if (app.execPath.empty()) {
            return false;
        }
        IShellLinkW *link = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IShellLinkW, reinterpret_cast<void **>(&link))) ||
            !link) {
            return false;
        }
        link->SetPath(utf8ToPathWide(app.execPath).c_str());
        link->SetDescription(utf8ToPathWide(app.name).c_str());
        link->SetIconLocation(utf8ToPathWide(app.execPath).c_str(), 0);
        IPersistFile *persist = nullptr;
        bool ok = false;
        if (SUCCEEDED(link->QueryInterface(IID_IPersistFile,
                                           reinterpret_cast<void **>(&persist))) &&
            persist) {
            ok = SUCCEEDED(persist->Save(path.wstring().c_str(), TRUE));
            persist->Release();
        }
        link->Release();
        return ok;
    }

    std::string revealLabel() const override { return "Show in Explorer"; }

    bool openPath(const std::string &path) override { return shellOpen(path, {}); }

    bool openPathWithApp(const AppInfo &app, const std::string &path) override {
        if (path.empty()) {
            return false;
        }
        if (!app.aumid.empty()) {
            return activateUwp(app.aumid, path);
        }
        if (app.execPath.empty()) {
            return false;
        }
        const std::wstring target = utf8ToPathWide(app.execPath);
        const std::wstring args = L"\"" + utf8ToPathWide(path) + L"\"";
        const HINSTANCE result = ShellExecuteW(nullptr, L"open", target.c_str(), args.c_str(),
                                               nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(result) > 32;
    }

    bool revealPath(const std::string &path) override {
        if (path.empty()) {
            return false;
        }
        std::wstring args = L"/select,\"" + utf8ToPathWide(path) + L"\"";
        return shellExecute(L"explorer.exe", args);
    }

    bool openTrash() override {
        PWSTR raw = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RecycleBinFolder, 0, nullptr, &raw)) && raw) {
            std::wstring path(raw);
            CoTaskMemFree(raw);
            return shellExecute(L"explorer.exe", L"\"" + path + L"\"");
        }
        return shellExecute(L"explorer.exe", L"shell:RecycleBinFolder");
    }

    bool emptyTrash() override {
        const HRESULT hr = SHEmptyRecycleBinW(nullptr, nullptr,
            SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
        return SUCCEEDED(hr);
    }

    bool isTrashEmpty() override {
        SHQUERYRBINFO info{};
        info.cbSize = sizeof(info);
        if (SUCCEEDED(SHQueryRecycleBinW(nullptr, &info))) {
            return info.i64NumItems == 0;
        }
        return true;
    }

    bool openFileManager() override { return shellExecute(L"explorer.exe", L""); }

    bool startFileDrag(const std::vector<std::string> &paths) override {
        if (paths.empty()) {
            return false;
        }
        std::vector<std::wstring> wide;
        wide.reserve(paths.size());
        for (const auto &path : paths) {
            wide.push_back(utf8ToPathWide(path));
        }
        HWND owner = dockHwnd_ ? dockHwnd_ : GetForegroundWindow();
        if (FAILED(OleInitialize(nullptr))) {
            return false;
        }
        PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(wide.front().c_str());
        if (pidl) {
            PIDLIST_ABSOLUTE parent = ILClone(pidl);
            if (parent) {
                ILRemoveLastID(parent);
                PCUITEMID_CHILD child = ILFindLastID(pidl);
                IDataObject *data = nullptr;
                const HRESULT createResult = SHCreateDataObject(
                    parent, 1, &child, nullptr, IID_IDataObject,
                    reinterpret_cast<void **>(&data));
                if (SUCCEEDED(createResult) && data) {
                    DWORD effect = 0;
                    SHDoDragDrop(owner, data, nullptr,
                                 DROPEFFECT_COPY | DROPEFFECT_MOVE, &effect);
                    data->Release();
                }
                ILFree(parent);
            }
            ILFree(pidl);
        }
        OleUninitialize();
        return true;
    }

    bool recyclePath(const std::string &path) override {
        if (path.empty()) {
            return false;
        }
        std::wstring from = utf8ToPathWide(path);
        from.push_back(L'\0');
        SHFILEOPSTRUCTW operation{};
        operation.wFunc = FO_DELETE;
        operation.pFrom = from.c_str();
        operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
        return SHFileOperationW(&operation) == 0 && !operation.fAnyOperationsAborted;
    }

    void setSystemMinimizeAnimationsEnabled(bool enabled) override {
        RD_LOG_DEBUG("system minimize animations -> %d", enabled ? 1 : 0);
        if (!enabled) {
            if (systemMinimizeAnimationsDisabled_) {
                return;
            }
            systemMinimizeAnimationsDisabled_ = true;
            refreshTransitionWindows(true);
            return;
        }
        if (!systemMinimizeAnimationsDisabled_) {
            return;
        }
        systemMinimizeAnimationsDisabled_ = false;
        for (HWND hwnd : transitionDisabledWindows_) {
            if (IsWindow(hwnd)) {
                BOOL allow = FALSE;
                DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &allow,
                                      sizeof(allow));
            }
        }
        transitionDisabledWindows_.clear();
    }

    void refreshTransitionWindows(bool force = false) {
        if (!systemMinimizeAnimationsDisabled_) {
            return;
        }
        const ULONGLONG now = GetTickCount64();
        if (!force && now - lastTransitionRefresh_ < 500) {
            return;
        }
        lastTransitionRefresh_ = now;
        struct RefreshState {
            WindowsPlatform *self = nullptr;
            int applied = 0;
        } state{this, 0};
        EnumWindows(
            [](HWND hwnd, LPARAM param) -> BOOL {
                auto *state = reinterpret_cast<RefreshState *>(param);
                auto *self = state->self;
                if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr ||
                    GetWindowTextLengthW(hwnd) <= 0) {
                    return TRUE;
                }
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                if (pid == GetCurrentProcessId() ||
                    self->transitionDisabledWindows_.count(hwnd) > 0) {
                    return TRUE;
                }
                BOOL current = FALSE;
                // Some Windows versions return E_INVALIDARG when querying this
                // attribute even though setting it works; assume it is off.
                if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED,
                                                 &current, sizeof(current)))) {
                    current = FALSE;
                }
                if (!current) {
                    BOOL disable = TRUE;
                    const HRESULT hr = DwmSetWindowAttribute(
                        hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disable, sizeof(disable));
                    if (SUCCEEDED(hr)) {
                        self->transitionDisabledWindows_.insert(hwnd);
                        ++state->applied;
                    } else {
                        RD_LOG_DEBUG("DwmSetWindowAttribute failed 0x%08lx",
                                     static_cast<unsigned long>(hr));
                    }
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&state));
        if (state.applied > 0) {
            RD_LOG_DEBUG("forced DWM transitions off for %d windows", state.applied);
        }
    }

    bool fileManagerRunning() override { return true; }

    bool isFileManagerApp(const AppInfo &app) override {
        const std::string file =
            lowerAscii(std::filesystem::path(app.execPath).filename().string());
        if (file == "explorer.exe") {
            return true;
        }
        const std::string name = lowerAscii(app.name);
        return name == "file explorer" || name == "windows explorer" ||
               name == "文件资源管理器";
    }

    bool isFileManagerShortcut(const std::string &path) override {
        if (lowerAscii(std::filesystem::path(path).filename().string()) == "explorer.exe") {
            return true;
        }
        if (lowerAscii(std::filesystem::path(path).extension().string()) != ".lnk") {
            return false;
        }
        const ShortcutInfo info = resolveShortcut(path);
        return lowerAscii(std::filesystem::path(info.target).filename().string()) ==
               "explorer.exe";
    }

    std::string shortcutTarget(const std::string &path) override {
        if (lowerAscii(std::filesystem::path(path).extension().string()) != ".lnk") {
            return {};
        }
        return resolveShortcut(path).target;
    }

    std::string shortcutAumid(const std::string &path) override {
        if (lowerAscii(std::filesystem::path(path).extension().string()) != ".lnk") {
            return {};
        }
        IPropertyStore *store = nullptr;
        if (FAILED(SHGetPropertyStoreFromParsingName(utf8ToPathWide(path).c_str(), nullptr,
                                                     GPS_DEFAULT, IID_PPV_ARGS(&store))) ||
            !store) {
            return {};
        }
        std::string aumid;
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(store->GetValue(PKEY_Link_TargetParsingPath, &value)) &&
            value.vt == VT_LPWSTR && value.pwszVal) {
            const std::string parsed = wideToUtf8(value.pwszVal);
            if (parsed.find('!') != std::string::npos &&
                parsed.find('\\') == std::string::npos &&
                parsed.find('/') == std::string::npos &&
                parsed.find(':') == std::string::npos) {
                aumid = parsed;
            }
        }
        PropVariantClear(&value);
        store->Release();
        return aumid;
    }

    bool movePathInto(const std::string &path, const std::string &directory) override {
        if (path.empty() || directory.empty()) {
            return false;
        }
        std::error_code ec;
        const std::filesystem::path source = utf8ToPath(path);
        const std::filesystem::path targetDir = utf8ToPath(directory);
        if (!std::filesystem::exists(source, ec) ||
            !std::filesystem::is_directory(targetDir, ec)) {
            return false;
        }
        // Dropping a folder onto itself (or its own descendant) is a no-op.
        const std::filesystem::path canonicalSource =
            std::filesystem::weakly_canonical(source, ec);
        const std::filesystem::path canonicalTarget =
            std::filesystem::weakly_canonical(targetDir, ec);
        if (!ec && canonicalTarget.string().rfind(canonicalSource.string(), 0) == 0) {
            return false;
        }
        std::wstring from = utf8ToPathWide(path);
        from.push_back(L'\0');
        const std::wstring to = utf8ToPathWide(directory);
        SHFILEOPSTRUCTW operation{};
        operation.wFunc = FO_MOVE;
        operation.pFrom = from.c_str();
        operation.pTo = to.c_str();
        operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT |
                           FOF_RENAMEONCOLLISION;
        return SHFileOperationW(&operation) == 0 && !operation.fAnyOperationsAborted;
    }

    std::set<std::string> runningExecutables() override {
        std::set<std::string> result;
        EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
            auto *set = reinterpret_cast<std::set<std::string> *>(param);
            if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr || isCloaked(hwnd)) {
                return TRUE;
            }
            if (GetWindowTextLengthW(hwnd) <= 0) {
                return TRUE;
            }
            std::string path = windowProcessPath(hwnd);
            if (!path.empty()) {
                set->insert(lowerAscii(path));
            }
            const std::string aumid = windowAumid(hwnd);
            if (!aumid.empty()) {
                set->insert(lowerAscii(aumid));
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&result));
        return result;
    }

    std::set<std::string> runningProcessPaths() override {
        std::set<std::string> result;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return result;
        }
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                const DWORD pid = entry.th32ProcessID;
                if (pid == 0 || pid == GetCurrentProcessId()) {
                    continue;
                }
                HANDLE process =
                    OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (!process) {
                    continue;
                }
                wchar_t buffer[32768];
                DWORD size = static_cast<DWORD>(std::size(buffer));
                if (QueryFullProcessImageNameW(process, 0, buffer, &size)) {
                    result.insert(lowerAscii(wideToUtf8(buffer)));
                }
                CloseHandle(process);
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return result;
    }

    std::string foregroundProcessPath() override {
        HWND foreground = GetForegroundWindow();
        if (!foreground) {
            return {};
        }
        const std::string path = windowProcessPath(foreground);
        return path.empty() ? std::string() : lowerAscii(path);
    }

    std::string downloadsPath() override {
        PWSTR raw = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &raw)) && raw) {
            std::string result = wideToUtf8(raw);
            CoTaskMemFree(raw);
            return result;
        }
        return {};
    }

    std::string wallpaperPath() override {
        wchar_t path[32768] = {};
        if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, static_cast<UINT>(std::size(path)),
                                  path, 0) &&
            path[0]) {
            return wideToUtf8(path);
        }
        return {};
    }

    std::string trashLabel() override { return "Trash"; }

    bool globalCursor(float &x, float &y) override {
        POINT point{};
        if (!GetCursorPos(&point)) {
            return false;
        }
        x = static_cast<float>(point.x);
        y = static_cast<float>(point.y);
        return true;
    }

    void prepareDockWindow(SDL_Window *window) override {
        SDL_PropertiesID props = SDL_GetWindowProperties(window);
        HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        if (!hwnd) {
            return;
        }
        hwnd_ = hwnd;
        LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        exStyle |= WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
        exStyle &= ~WS_EX_APPWINDOW;
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);

        dockHwnd_ = hwnd;
        oldWndProc_ = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(windowProc)));
        instance_ = this;
        if ((kMinimizeAnimationEnabled || kMinimizedWindowTilesEnabled) && !winEventHook_) {
            // MINIMIZESTART fires before the window disappears, so the screen
            // still contains its pixels. OUTOFCONTEXT keeps the callback on
            // this thread; SKIPOWNPROCESS excludes RevDock's own windows.
            winEventHook_ = SetWinEventHook(
                EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, winEventProc,
                0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        }
    }

    std::vector<MinimizedWindow> minimizedWindows() override {
        struct Search {
            WindowsPlatform *self = nullptr;
            std::vector<MinimizedWindow> *out = nullptr;
        } search{this, nullptr};
        std::vector<MinimizedWindow> result;
        search.out = &result;
        EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
            auto *state = reinterpret_cast<Search *>(param);
            if (!IsWindowVisible(hwnd) || !IsIconic(hwnd) ||
                GetWindow(hwnd, GW_OWNER) != nullptr) {
                return TRUE;
            }
            const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            if ((exStyle & WS_EX_TOOLWINDOW) != 0 || GetWindowTextLengthW(hwnd) <= 0) {
                return TRUE;
            }
            const std::string execPath = windowProcessPath(hwnd);
            if (execPath.empty()) {
                return TRUE;
            }
            wchar_t titleBuffer[512] = {};
            GetWindowTextW(hwnd, titleBuffer, static_cast<int>(std::size(titleBuffer)));
            MinimizedWindow info;
            info.handle = reinterpret_cast<std::uintptr_t>(hwnd);
            info.execPath = execPath;
            info.title = titleBuffer[0] ? wideToUtf8(titleBuffer) : std::string{};
            auto cached = state->self->minimizedThumbs_.find(hwnd);
            if (cached != state->self->minimizedThumbs_.end()) {
                info.rgba = cached->second.rgba;
                info.width = cached->second.width;
                info.height = cached->second.height;
                info.minimizedAt = cached->second.minimizedAt;
            }
            state->out->push_back(std::move(info));
            return TRUE;
        }, reinterpret_cast<LPARAM>(&search));
        // Keep the tiles in minimization order so the placeholder inserted for
        // the Genie animation does not jump when the periodic poll sees it.
        std::sort(result.begin(), result.end(), [](const MinimizedWindow &a,
                                                   const MinimizedWindow &b) {
            if (a.minimizedAt != b.minimizedAt) {
                return a.minimizedAt < b.minimizedAt;
            }
            return a.handle < b.handle;
        });
        return result;
    }

    bool restoreWindow(std::uintptr_t handle) override {
        HWND hwnd = reinterpret_cast<HWND>(handle);
        if (!hwnd || !IsWindow(hwnd)) {
            return false;
        }
        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        return true;
    }

    bool windowRestoreRect(std::uintptr_t handle, SDL_Rect &rect) override {
        HWND hwnd = reinterpret_cast<HWND>(handle);
        if (!hwnd || !IsWindow(hwnd)) {
            return false;
        }
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        if (!GetWindowPlacement(hwnd, &placement)) {
            return false;
        }
        RECT normal = placement.rcNormalPosition;
        rect = SDL_Rect{normal.left, normal.top, normal.right - normal.left,
                        normal.bottom - normal.top};
        return rect.w > 0 && rect.h > 0;
    }

    bool closeWindow(std::uintptr_t handle) override {
        HWND hwnd = reinterpret_cast<HWND>(handle);
        if (!hwnd || !IsWindow(hwnd)) {
            return false;
        }
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return true;
    }

    bool pollMinimizeEvent(MinimizeEvent &event) override {
        refreshTransitionWindows();
        refreshThumbnailCache();
        if (!kMinimizeAnimationEnabled) {
            return false;
        }
        std::lock_guard<std::mutex> lock(minimizeMutex_);
        if (minimizeQueue_.empty()) {
            return false;
        }
        event = std::move(minimizeQueue_.front());
        minimizeQueue_.pop_front();
        return true;
    }

    bool captureWindowBackdrop(SDL_Window *window, int x, int y, int w, int h,
                               std::vector<uint8_t> &pixels, int &outW, int &outH,
                               int downscale) override {
        if (!window || w <= 0 || h <= 0) {
            return false;
        }
        downscale = std::clamp(downscale, 1, 8);
        SDL_PropertiesID props = SDL_GetWindowProperties(window);
        HWND hwnd = static_cast<HWND>(
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        if (!hwnd) {
            return false;
        }
        // Exclude the captured window from the screen capture, otherwise the
        // backdrop contains a ghost of the window itself. Only the Dock window
        // keeps the affinity permanently (it is captured continuously); menus
        // and popovers restore their capturability after the one-shot capture,
        // otherwise they would also vanish from screenshots and recordings.
        constexpr DWORD kWdaExcludeFromCapture = 0x00000011;
        const bool permanent = hwnd == dockHwnd_;
        bool excluded = false;
        if (permanent) {
            if (excludedFromCapture_ != hwnd) {
                excluded = SetWindowDisplayAffinity(hwnd, kWdaExcludeFromCapture) != FALSE;
                if (excluded) {
                    DwmFlush();
                    excludedFromCapture_ = hwnd;
                }
            }
        } else {
            excluded = SetWindowDisplayAffinity(hwnd, kWdaExcludeFromCapture) != FALSE;
            if (excluded) {
                DwmFlush();
            }
        }
        POINT origin{x, y};
        ClientToScreen(hwnd, &origin);

        const int targetW = std::max(1, w / downscale);
        const int targetH = std::max(1, h / downscale);

        // Reuse the GDI surface and screen DC across captures: allocating a
        // DIB section and DCs every frame was a measurable part of the cost.
        bool ok = false;
        if (ensureCaptureSurface(w, h) && captureBits_) {
            const uint8_t *bits = static_cast<const uint8_t *>(captureBits_);
            ok = BitBlt(captureMemDc_, 0, 0, w, h, captureScreenDc_, origin.x, origin.y,
                        SRCCOPY) != FALSE;
            if (ok) {
                // Downscale with a simple box filter while converting the DIB's
                // BGRA layout to RGBA. A plain BitBlt is much faster than a
                // stretched GDI copy and the result is blurred anyway.
                const auto *source = static_cast<const uint8_t *>(bits);
                pixels.resize(static_cast<size_t>(targetW) * targetH * 4);
                for (int ty = 0; ty < targetH; ++ty) {
                    for (int tx = 0; tx < targetW; ++tx) {
                        int red = 0;
                        int green = 0;
                        int blue = 0;
                        int count = 0;
                        const int maxSy = std::min(downscale, h - ty * downscale);
                        const int maxSx = std::min(downscale, w - tx * downscale);
                        for (int sy = 0; sy < maxSy; ++sy) {
                            const uint8_t *row =
                                source + (static_cast<size_t>(ty * downscale + sy) * w +
                                          tx * downscale) * 4;
                            for (int sx = 0; sx < maxSx; ++sx) {
                                blue += row[sx * 4 + 0];
                                green += row[sx * 4 + 1];
                                red += row[sx * 4 + 2];
                                ++count;
                            }
                        }
                        if (count <= 0) {
                            count = 1;
                        }
                        const size_t out =
                            (static_cast<size_t>(ty) * targetW + tx) * 4;
                        pixels[out + 0] = static_cast<uint8_t>(red / count);
                        pixels[out + 1] = static_cast<uint8_t>(green / count);
                        pixels[out + 2] = static_cast<uint8_t>(blue / count);
                        pixels[out + 3] = 255;
                    }
                }
                outW = targetW;
                outH = targetH;
            }
        }
        if (!permanent && excluded) {
            SetWindowDisplayAffinity(hwnd, WDA_NONE);
        }
        return ok;
    }

    bool ensureCaptureSurface(int w, int h) {
        if (!captureScreenDc_) {
            captureScreenDc_ = GetDC(nullptr);
            if (!captureScreenDc_) {
                return false;
            }
        }
        if (captureBitmap_ && captureW_ == w && captureH_ == h) {
            return true;
        }
        releaseCaptureSurface();
        captureScreenDc_ = captureScreenDc_ ? captureScreenDc_ : GetDC(nullptr);
        captureMemDc_ = CreateCompatibleDC(captureScreenDc_);
        if (!captureMemDc_) {
            return false;
        }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        captureBitmap_ =
            CreateDIBSection(captureScreenDc_, &info, DIB_RGB_COLORS, &captureBits_, nullptr, 0);
        if (!captureBitmap_ || !captureBits_) {
            DeleteDC(captureMemDc_);
            captureMemDc_ = nullptr;
            return false;
        }
        captureOriginalBitmap_ = SelectObject(captureMemDc_, captureBitmap_);
        captureW_ = w;
        captureH_ = h;
        return true;
    }

    void releaseCaptureSurface() {
        if (captureMemDc_) {
            if (captureOriginalBitmap_) {
                SelectObject(captureMemDc_, captureOriginalBitmap_);
                captureOriginalBitmap_ = nullptr;
            }
            DeleteDC(captureMemDc_);
            captureMemDc_ = nullptr;
        }
        if (captureBitmap_) {
            DeleteObject(captureBitmap_);
            captureBitmap_ = nullptr;
            captureBits_ = nullptr;
        }
        if (captureScreenDc_) {
            ReleaseDC(nullptr, captureScreenDc_);
            captureScreenDc_ = nullptr;
        }
        captureW_ = 0;
        captureH_ = 0;
    }

    void setTaskbarVisible(bool visible) override {
        HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (tray && (IsWindowVisible(tray) != visible)) {
            ShowWindow(tray, visible ? SW_SHOW : SW_HIDE);
            SetWindowPos(tray, nullptr, 0, 0, 0, 0,
                         (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW) |
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
            const BOOL want = param ? TRUE : FALSE;
            wchar_t className[64] = {};
            GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
            if (wcscmp(className, L"Shell_SecondaryTrayWnd") == 0 &&
                IsWindowVisible(hwnd) != want) {
                ShowWindow(hwnd, want ? SW_SHOW : SW_HIDE);
                SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                             (want ? SWP_SHOWWINDOW : SWP_HIDEWINDOW) |
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            return TRUE;
        }, visible ? 1 : 0);
        taskbarHidden_ = !visible;
        if (visible) {
            applyReserve();
            restoreTaskbarState();
        } else {
            saveAndAutohideTaskbar();
            applyReserve();
        }
    }

    void setWindowOwner(SDL_Window *owned, SDL_Window *owner) override {
        if (!owned) {
            return;
        }
        HWND ownedHwnd = static_cast<HWND>(SDL_GetPointerProperty(
            SDL_GetWindowProperties(owned), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        if (!ownedHwnd) {
            return;
        }
        HWND ownerHwnd = nullptr;
        if (owner) {
            ownerHwnd = static_cast<HWND>(SDL_GetPointerProperty(
                SDL_GetWindowProperties(owner), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        }
        SetWindowLongPtrW(ownedHwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(ownerHwnd));
        SetWindowPos(ownedHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void raiseAlwaysOnTop(SDL_Window *window) override {
        if (!window) {
            return;
        }
        SDL_PropertiesID props = SDL_GetWindowProperties(window);
        HWND hwnd = static_cast<HWND>(
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        if (!hwnd) {
            return;
        }
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void focusWindow(SDL_Window *window) override {
        if (!window) {
            return;
        }
        SDL_PropertiesID props = SDL_GetWindowProperties(window);
        HWND hwnd = static_cast<HWND>(
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        if (!hwnd) {
            return;
        }
        // The Dock is WS_EX_NOACTIVATE; Windows' foreground lock can otherwise
        // keep the previous application active even after we call
        // SetForegroundWindow, so the IME composes there and never reaches the
        // Launchpad window.  A synthetic Alt tap lifts the foreground lock.
        keybd_event(VK_MENU, 0, 0, 0);
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
        const DWORD targetThread = GetCurrentThreadId();
        HWND foreground = GetForegroundWindow();
        const DWORD foregroundThread =
            foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
        if (foregroundThread != 0 && foregroundThread != targetThread) {
            AttachThreadInput(targetThread, foregroundThread, TRUE);
        }
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
        if (foregroundThread != 0 && foregroundThread != targetThread) {
            AttachThreadInput(targetThread, foregroundThread, FALSE);
        }
    }

    void setTextCommitHandler(
        SDL_Window *window,
        std::function<void(const std::string &)> handler) override {
        if (!window) {
            return;
        }
        SDL_PropertiesID props = SDL_GetWindowProperties(window);
        HWND hwnd = static_cast<HWND>(
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        if (!hwnd) {
            return;
        }
        textCommitHandler_ = std::move(handler);
        textCommitInstance_ = this;
        if (textCommitOld_.find(hwnd) == textCommitOld_.end()) {
            WNDPROC old = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(textCommitProc)));
            textCommitOld_[hwnd] = old;
        }
    }

    static LRESULT CALLBACK textCommitProc(HWND hwnd, UINT msg, WPARAM wParam,
                                           LPARAM lParam) {
        WindowsPlatform *self = textCommitInstance_;
        if (self && msg == WM_IME_COMPOSITION && (lParam & GCS_RESULTSTR)) {
            std::string committed;
            HIMC himc = ImmGetContext(hwnd);
            if (himc) {
                LONG bytes = ImmGetCompositionStringW(
                    himc, GCS_RESULTSTR, nullptr, 0);
                if (bytes > 0) {
                    std::wstring wide(static_cast<size_t>(bytes / 2), L'\0');
                    ImmGetCompositionStringW(himc, GCS_RESULTSTR, wide.data(),
                                             bytes);
                    const int size = WideCharToMultiByte(
                        CP_UTF8, 0, wide.c_str(),
                        static_cast<int>(wide.size()), nullptr, 0, nullptr,
                        nullptr);
                    if (size > 0) {
                        committed.resize(static_cast<size_t>(size));
                        WideCharToMultiByte(
                            CP_UTF8, 0, wide.c_str(),
                            static_cast<int>(wide.size()), committed.data(),
                            size, nullptr, nullptr);
                    }
                }
                ImmReleaseContext(hwnd, himc);
            }
            if (!committed.empty() && self->textCommitHandler_) {
                self->textCommitHandler_(committed);
            }
        }
        if (self) {
            auto found = self->textCommitOld_.find(hwnd);
            if (found != self->textCommitOld_.end() && found->second) {
                return CallWindowProcW(found->second, hwnd, msg, wParam,
                                       lParam);
            }
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    void setClickThrough(SDL_Window *window, bool enabled, bool layered = false) override {
        if (!window) {
            return;
        }
        SDL_PropertiesID props = SDL_GetWindowProperties(window);
        HWND hwnd = static_cast<HWND>(
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        if (!hwnd) {
            return;
        }
        LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (enabled) {
            exStyle |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
            if (layered) {
                // A plain WS_EX_TRANSPARENT top-level window still blocks mouse
                // activation of other processes. Layering it makes the (fully
                // transparent) sheet pass input through, while the D3D
                // swapchain's per-pixel alpha keeps rendering transparent.
                exStyle |= WS_EX_LAYERED;
            }
        } else {
            exStyle &= ~WS_EX_TRANSPARENT;
        }
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    void setPassThroughRects(SDL_Window *window,
                             const std::vector<SDL_Rect> &rects) override {
        if (!window) {
            return;
        }
        SDL_PropertiesID props = SDL_GetWindowProperties(window);
        HWND hwnd = static_cast<HWND>(
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        if (!hwnd) {
            return;
        }
        auto oldIt = passThroughOld_.find(hwnd);
        if (oldIt == passThroughOld_.end()) {
            WNDPROC old = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(passThroughProc)));
            passThroughOld_[hwnd] = old;
        }
        auto &stored = passThroughRects_[hwnd];
        stored.clear();
        stored.reserve(rects.size());
        for (const auto &rect : rects) {
            stored.push_back(RECT{rect.x, rect.y, rect.x + rect.w, rect.y + rect.h});
        }
    }

    void setHitTestRects(SDL_Window *window, const std::vector<SDL_Rect> &rects) override {
        (void)window;
        hitRects_.clear();
        hitRects_.reserve(rects.size());
        for (const auto &rect : rects) {
            hitRects_.push_back(RECT{rect.x, rect.y, rect.x + rect.w, rect.y + rect.h});
        }
        hitTestEnabled_ = true;
    }

    void reserveWorkArea(int displayIndex, const std::string &edge, int thickness,
                         bool enable) override {
        (void)displayIndex;
        reserveEnabled_ = enable;
        reserveEdge_ = edge;
        reserveThickness_ = std::max(16, thickness);
        applyReserve();
    }

    void applyReserve() {
        if (!hwnd_) {
            return;
        }
        APPBARDATA data{};
        data.cbSize = sizeof(data);
        data.hWnd = hwnd_;
        if (!reserveEnabled_ || !taskbarHidden_) {
            if (appBarRegistered_) {
                SHAppBarMessage(ABM_REMOVE, &data);
                appBarRegistered_ = false;
                appBarHwnd_ = nullptr;
            }
            return;
        }
        if (!appBarRegistered_) {
            if (!SHAppBarMessage(ABM_NEW, &data)) {
                return;
            }
            appBarRegistered_ = true;
            appBarHwnd_ = hwnd_;
        }
        HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info)) {
            return;
        }
        // Always reserve relative to the full monitor. Using the current
        // (already reduced) work area would shrink it again on every call.
        RECT work = info.rcMonitor;
        const int thickness = std::max(16, reserveThickness_);
        if (reserveEdge_ == "left") {
            data.uEdge = ABE_LEFT;
            data.rc = RECT{work.left, work.top, work.left + thickness, work.bottom};
        } else if (reserveEdge_ == "right") {
            data.uEdge = ABE_RIGHT;
            data.rc = RECT{work.right - thickness, work.top, work.right, work.bottom};
        } else {
            data.uEdge = ABE_BOTTOM;
            data.rc = RECT{work.left, work.bottom - thickness, work.right, work.bottom};
        }
        SHAppBarMessage(ABM_SETPOS, &data);
    }

    void saveAndAutohideTaskbar() {
        APPBARDATA data{};
        data.cbSize = sizeof(data);
        data.hWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (!data.hWnd || taskbarStateSaved_) {
            return;
        }
        taskbarState_ = static_cast<uint32_t>(SHAppBarMessage(ABM_GETSTATE, &data));
        taskbarStateSaved_ = true;
        data.lParam = ABS_AUTOHIDE;
        SHAppBarMessage(ABM_SETSTATE, &data);
    }

    void restoreTaskbarState() {
        if (!taskbarStateSaved_) {
            return;
        }
        APPBARDATA data{};
        data.cbSize = sizeof(data);
        data.hWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (data.hWnd) {
            data.lParam = taskbarState_;
            SHAppBarMessage(ABM_SETSTATE, &data);
        }
        taskbarStateSaved_ = false;
    }

private:
    ComInit com_;
    HWND hwnd_ = nullptr;
    HWND dockHwnd_ = nullptr;
    bool reserveEnabled_ = false;
    std::string reserveEdge_ = "bottom";
    int reserveThickness_ = 74;
    HWND appBarHwnd_ = nullptr;
    bool appBarRegistered_ = false;
    uint32_t taskbarState_ = 0;
    bool taskbarStateSaved_ = false;
    WNDPROC oldWndProc_ = nullptr;
    std::vector<RECT> hitRects_;
    bool hitTestEnabled_ = false;
    bool taskbarHidden_ = false;
    HWINEVENTHOOK winEventHook_ = nullptr;
    std::mutex minimizeMutex_;
    std::deque<MinimizeEvent> minimizeQueue_;
    struct MinimizedEntry {
        std::string execPath;
        std::string title;
        std::vector<uint8_t> rgba;
        int width = 0;
        int height = 0;
        std::uint64_t minimizedAt = 0;
    };
    std::map<HWND, MinimizedEntry> minimizedThumbs_;
    HWND excludedFromCapture_ = nullptr;
    HDC captureScreenDc_ = nullptr;
    HDC captureMemDc_ = nullptr;
    HBITMAP captureBitmap_ = nullptr;
    void *captureBits_ = nullptr;
    HGDIOBJ captureOriginalBitmap_ = nullptr;
    int captureW_ = 0;
    int captureH_ = 0;
    bool systemMinimizeAnimationsDisabled_ = false;
    std::set<HWND> transitionDisabledWindows_;
    std::set<HWND> hiddenAppWindows_;
    ULONGLONG lastTransitionRefresh_ = 0;
    HWND cachedHwnd_ = nullptr;
    SDL_Rect cachedRect_{};
    std::vector<uint8_t> cachedPixels_;
    int cachedWidth_ = 0;
    int cachedHeight_ = 0;
    ULONGLONG cachedImageAt_ = 0;
    ULONGLONG lastCachePoll_ = 0;

    void refreshThumbnailCache() {
        const ULONGLONG now = GetTickCount64();
        // Keep a fresh foreground snapshot so MINIMIZESTART (which fires after
        // the window is already hidden) has pixels and the restored rectangle.
        if (now - lastCachePoll_ < 900) {
            return;
        }
        lastCachePoll_ = now;
        HWND foreground = GetForegroundWindow();
        if (!foreground || foreground == dockHwnd_ || !IsWindowVisible(foreground) ||
            IsIconic(foreground) || GetWindow(foreground, GW_OWNER) != nullptr ||
            GetWindowTextLengthW(foreground) <= 0) {
            return;
        }
        const LONG_PTR exStyle = GetWindowLongPtrW(foreground, GWL_EXSTYLE);
        if ((exStyle & WS_EX_TOOLWINDOW) != 0 || windowProcessPath(foreground).empty()) {
            return;
        }
        RECT rect{};
        if (FAILED(DwmGetWindowAttribute(foreground, DWMWA_EXTENDED_FRAME_BOUNDS, &rect,
                                         sizeof(rect)))) {
            if (!GetWindowRect(foreground, &rect)) {
                return;
            }
        }
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        if (width < 40 || height < 40) {
            return;
        }
        std::vector<uint8_t> pixels;
        if (!blitScreenRect(rect, pixels, width, height) ||
            !imageHasDetail(pixels, width, height)) {
            return;
        }
        int scaledWidth = width;
        int scaledHeight = height;
        downscaleRGBA(pixels, scaledWidth, scaledHeight, 640);
        cachedHwnd_ = foreground;
        cachedRect_ = SDL_Rect{rect.left, rect.top, width, height};
        cachedPixels_ = std::move(pixels);
        cachedWidth_ = scaledWidth;
        cachedHeight_ = scaledHeight;
        cachedImageAt_ = now;
    }

    static WindowsPlatform *instance_;
    static WindowsPlatform *textCommitInstance_;
    std::map<HWND, WNDPROC> textCommitOld_;
    std::function<void(const std::string &)> textCommitHandler_;

    void queueMinimize(HWND hwnd) {
        RD_LOG_DEBUG("minimize event t=%llu", static_cast<unsigned long long>(GetTickCount64()));
        if (!hwnd || GetWindow(hwnd, GW_OWNER) != nullptr) {
            return;
        }
        const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if ((exStyle & WS_EX_TOOLWINDOW) != 0 || GetWindowTextLengthW(hwnd) <= 0) {
            return;
        }
        const std::string execPath = windowProcessPath(hwnd);
        if (execPath.empty()) {
            return;
        }
        RECT rect{};
        if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect,
                                         sizeof(rect))) ||
            rect.left <= -30000 || rect.top <= -30000 ||
            rect.right - rect.left < 24 || rect.bottom - rect.top < 24) {
            // An already-iconic window reports the -32000 parking rectangle;
            // GetWindowPlacement still knows where it will restore.
            WINDOWPLACEMENT placement{};
            placement.length = sizeof(placement);
            if (GetWindowPlacement(hwnd, &placement)) {
                rect = placement.rcNormalPosition;
            } else if (!GetWindowRect(hwnd, &rect)) {
                return;
            }
        }
        MinimizeEvent event;
        event.handle = reinterpret_cast<std::uintptr_t>(hwnd);
        event.execPath = execPath;
        event.aumid = windowAumid(hwnd);
        event.rect = SDL_Rect{rect.left, rect.top, rect.right - rect.left,
                              rect.bottom - rect.top};
        if (hwnd == cachedHwnd_ && !cachedPixels_.empty() &&
            GetTickCount64() - cachedImageAt_ < 4000) {
            event.rect = cachedRect_;
            event.rgba = cachedPixels_;
            event.width = cachedWidth_;
            event.height = cachedHeight_;
        } else if (!captureWindowPixels(hwnd, event.rgba, event.width, event.height)) {
            event.rgba.clear();
            event.width = 0;
            event.height = 0;
        }
        wchar_t titleBuffer[512] = {};
        GetWindowTextW(hwnd, titleBuffer, static_cast<int>(std::size(titleBuffer)));
        event.title = titleBuffer[0] ? wideToUtf8(titleBuffer) : std::string{};
        MinimizedEntry entry;
        entry.execPath = execPath;
        entry.title = titleBuffer[0] ? wideToUtf8(titleBuffer) : std::string{};
        entry.rgba = event.rgba;
        entry.width = event.width;
        entry.height = event.height;
        entry.minimizedAt = GetTickCount64();
        minimizedThumbs_[hwnd] = std::move(entry);
        if (!kMinimizeAnimationEnabled) {
            return;
        }
        std::lock_guard<std::mutex> lock(minimizeMutex_);
        minimizeQueue_.push_back(std::move(event));
        while (minimizeQueue_.size() > 4) {
            minimizeQueue_.pop_front();
        }
    }

    void forgetMinimized(HWND hwnd) {
        minimizedThumbs_.erase(hwnd);
    }

    static void CALLBACK winEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject,
                                      LONG idChild, DWORD, DWORD) {
        if (!instance_ || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) {
            return;
        }
        if (event == EVENT_SYSTEM_MINIMIZEEND) {
            instance_->forgetMinimized(hwnd);
        } else if (event == EVENT_SYSTEM_MINIMIZESTART) {
            instance_->queueMinimize(hwnd);
            // Wake SDL's event loop immediately; otherwise it can sit in
            // WaitEventTimeout and the Genie overlay would appear a few frames
            // after the window has already vanished.
            SDL_Event wake;
            SDL_zero(wake);
            wake.type = SDL_EVENT_USER;
            SDL_PushEvent(&wake);
        }
    }

    static std::unordered_map<HWND, WNDPROC> passThroughOld_;
    static std::unordered_map<HWND, std::vector<RECT>> passThroughRects_;

    static LRESULT CALLBACK passThroughProc(HWND hwnd, UINT message, WPARAM wparam,
                                            LPARAM lparam) {
        auto oldIt = passThroughOld_.find(hwnd);
        if (oldIt == passThroughOld_.end() || !oldIt->second) {
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
        auto rectIt = passThroughRects_.find(hwnd);
        if (message == WM_NCHITTEST && rectIt != passThroughRects_.end() &&
            !rectIt->second.empty()) {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd, &point);
            for (const RECT &rect : rectIt->second) {
                if (point.x >= rect.left && point.x < rect.right && point.y >= rect.top &&
                    point.y < rect.bottom) {
                    return HTTRANSPARENT;
                }
            }
        }
        WNDPROC old = oldIt->second;
        if (message == WM_NCDESTROY) {
            passThroughOld_.erase(hwnd);
            passThroughRects_.erase(hwnd);
        }
        return CallWindowProcW(old, hwnd, message, wparam, lparam);
    }

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (instance_ && message == WM_NCHITTEST && instance_->hitTestEnabled_ &&
            hwnd == instance_->dockHwnd_) {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd, &point);
            bool inside = false;
            for (const RECT &rect : instance_->hitRects_) {
                if (point.x >= rect.left && point.x < rect.right &&
                    point.y >= rect.top && point.y < rect.bottom) {
                    inside = true;
                    break;
                }
            }
            if (!inside) {
                return HTTRANSPARENT;
            }
        }
        if (instance_ && instance_->oldWndProc_) {
            return CallWindowProcW(instance_->oldWndProc_, hwnd, message, wparam, lparam);
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    std::filesystem::path startupShortcutPath(const AppInfo &app) {
        PWSTR raw = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &raw)) || !raw) {
            return {};
        }
        const std::filesystem::path startup(raw);
        CoTaskMemFree(raw);
        std::string name;
        if (!app.sourcePath.empty()) {
            const std::filesystem::path source = utf8ToPath(app.sourcePath);
            const std::string extension = lowerAscii(source.extension().string());
            if (extension == ".lnk" || extension == ".url") {
                name = pathToUtf8(source.filename());
            }
        }
        if (name.empty()) {
            name = app.name.empty() ? "Application" : app.name;
            for (char &ch : name) {
                if (ch == '\\' || ch == '/' || ch == ':' || ch == '*' || ch == '?' ||
                    ch == '"' || ch == '<' || ch == '>' || ch == '|') {
                    ch = '_';
                }
            }
            if (name.size() < 4 || lowerAscii(name.substr(name.size() - 4)) != ".lnk") {
                name += ".lnk";
            }
        }
        return startup / utf8ToPath(name);
    }

    std::vector<std::filesystem::path> startMenuRoots() {
        std::vector<std::filesystem::path> roots;
        for (REFKNOWNFOLDERID id : {FOLDERID_Programs, FOLDERID_CommonPrograms}) {
            PWSTR raw = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &raw)) && raw) {
                roots.emplace_back(raw);
                CoTaskMemFree(raw);
            }
        }
        return roots;
    }

    void scanPackagedApps(std::vector<AppInfo> &apps,
                          std::unordered_set<std::string> &seen) {
        IShellItem *folder = nullptr;
        if (FAILED(SHCreateItemFromParsingName(L"shell:AppsFolder", nullptr,
                                               IID_PPV_ARGS(&folder))) ||
            !folder) {
            return;
        }
        IEnumShellItems *enumerator = nullptr;
        if (SUCCEEDED(folder->BindToHandler(nullptr, BHID_EnumItems,
                                            IID_PPV_ARGS(&enumerator))) &&
            enumerator) {
            IShellItem *item = nullptr;
            while (enumerator->Next(1, &item, nullptr) == S_OK && item) {
                PWSTR parsing = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &parsing)) &&
                    parsing) {
                    const std::string path = wideToUtf8(parsing);
                    CoTaskMemFree(parsing);
                    // PackageFamilyName!AppId; desktop apps and shell CLSIDs
                    // are already covered by the Start Menu scan.
                    if (path.find('!') != std::string::npos &&
                        path.find('_') != std::string::npos &&
                        path.find('\\') == std::string::npos &&
                        path.find('/') == std::string::npos &&
                        path.find(':') == std::string::npos &&
                        seen.insert(lowerAscii(path)).second) {
                        std::string name;
                        PWSTR display = nullptr;
                        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &display)) &&
                            display) {
                            name = wideToUtf8(display);
                            CoTaskMemFree(display);
                        }
                        AppInfo app;
                        app.aumid = path;
                        app.id = "uwp:" + path;
                        app.name = name.empty() ? path : name;
                        app.sourcePath = "shell:AppsFolder\\" + path;
                        app.iconPath = app.sourcePath;
                        apps.push_back(std::move(app));
                    }
                }
                item->Release();
            }
            enumerator->Release();
        }
        folder->Release();
    }

    void scanRoot(const std::filesystem::path &root, std::vector<AppInfo> &apps,
                  std::unordered_set<std::string> &seen) {
        std::error_code ec;
        if (!std::filesystem::exists(root, ec)) {
            return;
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
            const auto extension = lowerAscii(it->path().extension().string());
            if (extension == ".url") {
                // Internet shortcuts: keep only app-registered protocols such as
                // steam://rungameid/... (web URLs are not applications).
                const UrlShortcutInfo url = parseUrlShortcut(it->path());
                if (!isCustomProtocol(url.url)) {
                    continue;
                }
                AppInfo app;
                app.sourcePath = pathToUtf8(it->path());
                app.name = pickDesktopName(pathToUtf8(it->path().stem()));
                app.menuGroup = startMenuGroupFor(it->path(), root);
                app.id = app.sourcePath;
                app.iconPath = url.iconFile.empty() ? app.sourcePath : url.iconFile;
                if (!seen.insert(lowerAscii(app.id)).second) {
                    continue;
                }
                apps.push_back(std::move(app));
                continue;
            }
            if (extension != ".lnk") {
                continue;
            }
            const ShortcutInfo shortcut = resolveShortcut(it->path());
            if (!shortcut.aumid.empty()) {
                const std::string key = lowerAscii(shortcut.aumid);
                if (!seen.insert(key).second) {
                    continue;
                }
                AppInfo app;
                app.sourcePath = pathToUtf8(it->path());
                app.name = pickDesktopName(pathToUtf8(it->path().stem()));
                app.id = app.sourcePath;
                app.iconPath = app.sourcePath;
                app.aumid = shortcut.aumid;
                apps.push_back(std::move(app));
                continue;
            }
            const std::string lowerArgs = lowerAscii(shortcut.arguments);
            // Launchpad shows programs only. Shortcuts to documents, URLs or
            // folders are skipped; shell/AppsFolder shortcuts and shortcuts that
            // start a program with arguments stay.
            const bool shellTarget = lowerArgs.find("shell:") != std::string::npos;
            const bool customProtocol =
                isCustomProtocol(shortcut.target) || isCustomProtocol(shortcut.arguments);
            if (!isProgramTarget(shortcut.target) && !shellTarget && !customProtocol) {
                continue;
            }
            AppInfo app;
            app.sourcePath = pathToUtf8(it->path());
            app.name = pickDesktopName(pathToUtf8(it->path().stem()));
            app.menuGroup = startMenuGroupFor(it->path(), root);
            app.execPath = shortcut.target;
            if (const std::string real = resolveInstallerLauncher(shortcut.target);
                !real.empty()) {
                app.execPath = real;
                app.launchExec = true;
            }
            app.id = app.sourcePath;
            app.iconPath = app.sourcePath;
            const std::string key = lowerAscii(app.execPath.empty() ? app.sourcePath : app.execPath);
            if (!seen.insert(key).second) {
                continue;
            }
            apps.push_back(std::move(app));
        }
    }

    bool shellOpen(const std::string &target, const std::wstring &extraArgs) {
        if (target.empty()) {
            return false;
        }
        const std::wstring wide = utf8ToPathWide(target);
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
        info.lpVerb = L"open";
        info.lpFile = wide.c_str();
        info.lpParameters = extraArgs.empty() ? nullptr : extraArgs.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info)) {
            RD_LOG_WARN("ShellExecuteExW failed for %s", target.c_str());
            return false;
        }
        return true;
    }

    bool shellExecute(const wchar_t *file, const std::wstring &parameters) {
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
        info.lpVerb = L"open";
        info.lpFile = file;
        info.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
        info.nShow = SW_SHOWNORMAL;
        return ShellExecuteExW(&info) != FALSE;
    }
};

WindowsPlatform *WindowsPlatform::instance_ = nullptr;
WindowsPlatform *WindowsPlatform::textCommitInstance_ = nullptr;
std::unordered_map<HWND, WNDPROC> WindowsPlatform::passThroughOld_;
std::unordered_map<HWND, std::vector<RECT>> WindowsPlatform::passThroughRects_;

std::unique_ptr<Platform> createPlatform() {
    return std::make_unique<WindowsPlatform>();
}

} // namespace revdock
