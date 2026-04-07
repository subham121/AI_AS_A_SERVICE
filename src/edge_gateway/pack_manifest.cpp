#include <edgeai/fs_utils.h>
#include <edgeai/json_utils.h>
#include <edgeai/pack_manifest.h>

#include <iostream>
#include <stdexcept>

namespace edgeai {

namespace {

std::string requireString(const Json::Value& value, const char* field) {
    if (!value.isMember(field) || !value[field].isString()) {
        throw std::runtime_error(std::string("Missing string field in manifest: ") + field);
    }
    return value[field].asString();
}

}  // namespace

std::filesystem::path PackManifest::libraryPath(const std::filesystem::path& pack_root) const {
    std::cerr << "[PackManifest::libraryPath] Resolving library path for entry: " << entry_library << std::endl;
    auto candidate = pack_root / entry_library;
    if (std::filesystem::exists(candidate)) {
        std::cerr << "[PackManifest::libraryPath] Library found: " << candidate << std::endl;
        return candidate;
    }

    if (candidate.has_extension()) {
        for (const auto& extension : {".dylib", ".so", ".dll"}) {
            auto alternate = candidate;
            alternate.replace_extension(extension);
            if (std::filesystem::exists(alternate)) {
                std::cerr << "[PackManifest::libraryPath] Library found with alternate extension: " << alternate << std::endl;
                return alternate;
            }
        }
    }
    std::cerr << "[PackManifest::libraryPath] Using candidate path (may not exist): " << candidate << std::endl;
    return candidate;
}

std::filesystem::path PackManifest::configPath(const std::filesystem::path& pack_root) const {
    auto config = pack_root / default_config;
    std::cerr << "[PackManifest::configPath] Config path: " << config << std::endl;
    return config;
}

PackManifest manifestFromJson(const Json::Value& value) {
    std::cerr << "[manifestFromJson] Parsing manifest from JSON" << std::endl;
    PackManifest manifest;
    manifest.pack_id = requireString(value, "pack_id");
    manifest.name = requireString(value, "name");
    manifest.version = requireString(value, "version");
    manifest.intent = requireString(value, "intent");
    std::cerr << "[manifestFromJson] Parsed pack: " << manifest.pack_id << " (" << manifest.name 
              << " v" << manifest.version << ", intent: " << manifest.intent << ")" << std::endl;
    manifest.license = value.get("license", "").asString();
    manifest.metering_unit = value.get("metering_unit", "request").asString();
    manifest.entry_library = requireString(value["entrypoint"], "library");
    manifest.default_config = value["entrypoint"].get("default_config", "config/default.json").asString();
    manifest.device_capability = value["device_capability"];
    manifest.ai_capability = value["ai_capability"];
    manifest.dependencies = value["dependencies"];
    manifest.services = value["services"];
    manifest.tags = value["tags"];
    manifest.raw = value;
    return manifest;
}

PackManifest manifestFromFile(const std::filesystem::path& path) {
    std::cerr << "[manifestFromFile] Loading manifest from: " << path << std::endl;
    auto manifest = manifestFromJson(parseJson(readTextFile(path)));
    std::cerr << "[manifestFromFile] Manifest loaded successfully" << std::endl;
    return manifest;
}

}  // namespace edgeai
