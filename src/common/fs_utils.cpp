#include <edgeai/fs_utils.h>
#include <edgeai/process_utils.h>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace edgeai {

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Unable to open file for reading: " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void writeTextFile(const std::filesystem::path& path, const std::string& content) {
    if (path.has_parent_path()) {
        ensureDirectory(path.parent_path());
    }
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("Unable to open file for writing: " + path.string());
    }
    output << content;
}

void ensureDirectory(const std::filesystem::path& path) {
    std::filesystem::create_directories(path);
}

void removePath(const std::filesystem::path& path) {
    if (std::filesystem::exists(path)) {
        std::filesystem::remove_all(path);
    }
}

std::string computeMd5(const std::filesystem::path& path) {
#ifdef __APPLE__
    auto result = runCommandCapture({"md5", "-q", path.string()});
#else
    auto result = runCommandCapture({"md5sum", path.string()});
#endif
    if (result.exit_code != 0) {
        throw std::runtime_error("Failed to compute md5 for " + path.string());
    }
    std::string output = result.output;
    output.erase(output.find_last_not_of(" \r\n\t") + 1);
#ifndef __APPLE__
    auto pos = output.find(' ');
    if (pos != std::string::npos) {
        output = output.substr(0, pos);
    }
#endif
    return output;
}

bool extractTarGz(const std::filesystem::path& archive, const std::filesystem::path& destination) {
    ensureDirectory(destination);
    auto result = runCommandCapture({
        "tar", "-xzf", archive.string(), "-C", destination.string()
    });
    return result.exit_code == 0;
}

std::filesystem::path makeTempJson(const std::filesystem::path& dir, const std::string& prefix, const std::string& payload) {
    ensureDirectory(dir);
    auto path = dir / (prefix + ".json");
    int counter = 0;
    while (std::filesystem::exists(path)) {
        ++counter;
        path = dir / (prefix + "_" + std::to_string(counter) + ".json");
    }
    writeTextFile(path, payload);
    return path;
}

}  // namespace edgeai
