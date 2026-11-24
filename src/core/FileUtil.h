#pragma once

#include <filesystem>
#include <string>

namespace revdock {

std::string readTextFile(const std::filesystem::path &path);
bool writeTextFileAtomic(const std::filesystem::path &path, const std::string &contents);
bool writeBinaryFileAtomic(const std::filesystem::path &path, const void *data, size_t size);

} // namespace revdock
