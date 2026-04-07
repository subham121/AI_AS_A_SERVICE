#pragma once

#include <json/json.h>

#include <filesystem>
#include <string>
#include <vector>

namespace edgeai {

struct PackManifest {
    std::string pack_id;
    std::string name;
    std::string version;
    std::string intent;
    std::string license;
    std::string metering_unit;
    std::string entry_library;
    std::string default_config;
    Json::Value device_capability;
    Json::Value ai_capability;
    Json::Value dependencies;
    Json::Value services;
    Json::Value tags;
    Json::Value raw;

    std::filesystem::path libraryPath(const std::filesystem::path& pack_root) const;
    std::filesystem::path configPath(const std::filesystem::path& pack_root) const;
};

PackManifest manifestFromJson(const Json::Value& value);
PackManifest manifestFromFile(const std::filesystem::path& path);

}  // namespace edgeai
