#pragma once

#include <filesystem>
#include <string>

namespace edgeai {

std::string readTextFile(const std::filesystem::path& path);
void writeTextFile(const std::filesystem::path& path, const std::string& content);
void ensureDirectory(const std::filesystem::path& path);
void removePath(const std::filesystem::path& path);
std::string computeMd5(const std::filesystem::path& path);
bool extractTarGz(const std::filesystem::path& archive, const std::filesystem::path& destination);
std::filesystem::path makeTempJson(const std::filesystem::path& dir, const std::string& prefix, const std::string& payload);

}  // namespace edgeai
