#include <edgeai/fs_utils.h>
#include <edgeai/json_utils.h>
#include <edgeai/pack_manifest.h>

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
    auto candidate = pack_root / entry_library;
    if (std::filesystem::exists(candidate)) {
        return candidate;
    }

    if (candidate.has_extension()) {
        for (const auto& extension : {".dylib", ".so", ".dll"}) {
            auto alternate = candidate;
            alternate.replace_extension(extension);
            if (std::filesystem::exists(alternate)) {
                return alternate;
            }
        }
    }
    return candidate;
}

std::filesystem::path PackManifest::configPath(const std::filesystem::path& pack_root) const {
    return pack_root / default_config;
}

PackManifest manifestFromJson(const Json::Value& value) {
    PackManifest manifest;
    manifest.pack_id = requireString(value, "pack_id");
    manifest.name = requireString(value, "name");
    manifest.version = requireString(value, "version");
    manifest.intent = requireString(value, "intent");
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
    return manifestFromJson(parseJson(readTextFile(path)));
}

}  // namespace edgeai
