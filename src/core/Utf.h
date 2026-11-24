#pragma once

#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace revdock {

#ifdef _WIN32
inline std::wstring utf8ToWide(const std::string &text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

inline std::string wideToUtf8(const std::wstring &text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}
#else
inline std::string utf8ToWide(const std::string &text) { return text; }
inline std::string wideToUtf8(const std::string &text) { return text; }
#endif

inline std::filesystem::path utf8ToPath(const std::string &text) {
#ifdef _WIN32
    return std::filesystem::path(utf8ToWide(text));
#else
    return std::filesystem::path(text);
#endif
}

inline std::string pathToUtf8(const std::filesystem::path &path) {
#ifdef _WIN32
    return wideToUtf8(path.wstring());
#else
    return path.string();
#endif
}

} // namespace revdock
