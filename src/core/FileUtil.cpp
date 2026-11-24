#include "core/FileUtil.h"

#include <fstream>
#include <system_error>

namespace revdock {

std::string readTextFile(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool writeBinaryFileAtomic(const std::filesystem::path &path, const void *data, size_t size) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto temp = path;
    temp += ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            return false;
        }
        if (size > 0) {
            file.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
        }
        file.flush();
    }
    std::filesystem::remove(path, ec);
    std::filesystem::rename(temp, path, ec);
    return !ec;
}

bool writeTextFileAtomic(const std::filesystem::path &path, const std::string &contents) {
    return writeBinaryFileAtomic(path, contents.data(), contents.size());
}

} // namespace revdock
