#include <edgeai/fs_utils.h>
#include <edgeai/json_utils.h>
#include <edgeai/pack_manager.h>

#include <algorithm>
#include <stdexcept>

namespace edgeai {

namespace {

std::string runtimeKey(const std::string& user_id, const std::string& pack_id) {
    return user_id + "::" + pack_id;
}

Json::Value emptyRegistry() {
    Json::Value registry(Json::objectValue);
    registry["packs"] = Json::Value(Json::objectValue);
    registry["users"] = Json::Value(Json::objectValue);
    return registry;
}

Json::Value emptyRollbackRegistry() {
    Json::Value registry(Json::objectValue);
    registry["history"] = Json::Value(Json::objectValue);
    return registry;
}

std::string normalizeStatus(const Json::Value& entry, const std::string& fallback = "Unknown") {
    return entry.get("status", fallback).asString();
}

Json::Value& userPackEntry(Json::Value& registry, const std::string& user_id, const std::string& pack_id) {
    return registry["users"][user_id][pack_id];
}

bool jsonArrayContains(const Json::Value& array, const std::string& value) {
    if (!array.isArray()) {
        return false;
    }
    for (const auto& item : array) {
        if (item.asString() == value) {
            return true;
        }
    }
    return false;
}

}  // namespace

PackManager::PackManager(std::filesystem::path state_dir,
                         std::filesystem::path staging_dir,
                         std::filesystem::path install_dir,
                         std::string catalog_url,
                         Json::Value default_device_capability)
    : state_dir_(std::move(state_dir)),
      staging_dir_(std::move(staging_dir)),
      install_dir_(std::move(install_dir)),
      registry_path_(state_dir_ / "intent_and_pack_registry.json"),
      rollback_path_(state_dir_ / "rollback_registry.json"),
      catalog_url_(std::move(catalog_url)),
      default_device_capability_(std::move(default_device_capability)) {}

Json::Value PackManager::initialize() {
    ensureDirectory(state_dir_);
    ensureDirectory(staging_dir_);
    ensureDirectory(install_dir_);
    if (!std::filesystem::exists(registry_path_)) {
        storeRegistry(emptyRegistry());
    }
    if (!std::filesystem::exists(rollback_path_)) {
        storeRollbackRegistry(emptyRollbackRegistry());
    }
    return makeStatus("ok", "PackManager initialized");
}

Json::Value PackManager::queryPacks(const std::string& intent, const Json::Value& device_capability) {
    Json::Value payload(Json::objectValue);
    payload["intent"] = intent;
    payload["device_capability"] = deviceCapabilityFor(device_capability);
    return http_client_.postJson(catalog_url_ + "/packs/query", payload);
}

Json::Value PackManager::installPack(const std::string& user_id, const std::string& pack_id, bool approve_dependencies) {
    initialize();
    Json::Value registry = loadRegistry();
    Json::Value metadata = fetchPackMetadata(pack_id);
    Json::Value pack_entry = registry["packs"][pack_id];
    const std::string version = metadata["version"].asString();
    const std::string installed_version = pack_entry.get("version", "").asString();

    if (!installed_version.empty() && installed_version == version && normalizeStatus(pack_entry) != "Uninstalled") {
        return makeStatus("skipped", "Pack already installed at requested version");
    }

    const auto staged_archive = staging_dir_ / (pack_id + "-" + version + ".tar.gz");
    http_client_.downloadToFile(metadata["package_url"].asString(), staged_archive);
    const std::string actual_md5 = computeMd5(staged_archive);
    const std::string expected_md5 = metadata.get("md5", "").asString();
    if (!expected_md5.empty() && expected_md5 != actual_md5) {
        registry["packs"][pack_id]["status"] = "InstallFailed";
        registry["packs"][pack_id]["last_error"] = "checksum mismatch";
        storeRegistry(registry);
        publishEvent("install", "verification_failed", pack_id, user_id);
        return makeStatus("error", "Downloaded pack failed md5 verification");
    }

    if (!capabilityMatches(metadata["device_capability"], default_device_capability_)) {
        registry["packs"][pack_id]["status"] = "Blocked";
        registry["packs"][pack_id]["last_error"] = "device capability mismatch";
        storeRegistry(registry);
        publishEvent("install", "blocked_incompatible_capability", pack_id, user_id);
        return makeStatus("error", "Pack is not compatible with this device");
    }

    if (metadata["dependencies"].isArray() && metadata["dependencies"].size() > 0 && !approve_dependencies) {
        publishEvent("install", "dependency_approval_required", pack_id, user_id);
        return makeStatus("approval_required", "Dependency approval is required");
    }

    for (const auto& dependency : metadata["dependencies"]) {
        installPack(user_id, dependency.asString(), true);
    }

    if (!installed_version.empty() && installed_version != version) {
        Json::Value rollback = loadRollbackRegistry();
        Json::Value history_entry(Json::objectValue);
        history_entry["version"] = installed_version;
        history_entry["pack_root"] = pack_entry["pack_root"];
        history_entry["manifest_path"] = pack_entry["manifest_path"];
        rollback["history"][pack_id].append(history_entry);
        storeRollbackRegistry(rollback);
    }

    const auto install_root = install_dir_ / pack_id / version;
    removePath(install_root);
    if (!extractTarGz(staged_archive, install_root)) {
        throw std::runtime_error("Failed to install bundle archive for " + pack_id);
    }

    const auto manifest_path = install_root / "manifest.json";
    const PackManifest manifest = manifestFromFile(manifest_path);

    Json::Value installed(Json::objectValue);
    installed["pack_id"] = manifest.pack_id;
    installed["name"] = manifest.name;
    installed["version"] = manifest.version;
    installed["status"] = "Installed";
    installed["intent"] = manifest.intent;
    installed["manifest_path"] = manifest_path.string();
    installed["pack_root"] = install_root.string();
    installed["device_capability"] = metadata["device_capability"];
    installed["ai_capability"] = metadata["ai_capability"];
    installed["dependencies"] = metadata["dependencies"];
    installed["license"] = metadata["license"];
    installed["metering_unit"] = metadata["metering_unit"];
    registry["packs"][pack_id] = installed;
    storeRegistry(registry);
    publishEvent("install", "installed", pack_id, user_id, installed);

    Json::Value result = makeStatus("ok", "Pack installed");
    result["pack"] = installed;
    return result;
}

Json::Value PackManager::enablePack(const std::string& user_id, const std::string& pack_id) {
    Json::Value registry = loadRegistry();
    if (!registry["packs"].isMember(pack_id)) {
        return makeStatus("error", "Pack is not installed");
    }
    auto& entry = userPackEntry(registry, user_id, pack_id);
    entry["status"] = "Enabled";
    storeRegistry(registry);
    publishEvent("enable", "enabled", pack_id, user_id);
    return makeStatus("ok", "Pack enabled");
}

Json::Value PackManager::loadPack(const std::string& user_id, const std::string& pack_id) {
    Json::Value registry = loadRegistry();
    if (!registry["packs"].isMember(pack_id)) {
        return makeStatus("error", "Pack is not installed");
    }

    const Json::Value pack_entry = registry["packs"][pack_id];
    const PackManifest manifest = loadInstalledManifest(pack_entry);
    const std::string key = runtimeKey(user_id, pack_id);

    try {
        PackRuntime runtime;
        runtime.load(packRootFor(pack_entry), manifest, state_dir_);
        const auto config_path = manifest.configPath(packRootFor(pack_entry));
        if (std::filesystem::exists(config_path)) {
            runtime.configure(readTextFile(config_path));
        } else {
            runtime.configure("{}");
        }
        runtimes_[key] = std::move(runtime);

        auto& user_entry = userPackEntry(registry, user_id, pack_id);
        user_entry["status"] = "Loaded";
        storeRegistry(registry);
        publishEvent("load", "loaded", pack_id, user_id);
        return makeStatus("ok", "Pack loaded");
    } catch (const std::exception& ex) {
        Json::Value rollback = loadRollbackRegistry();
        if (rollback["history"].isMember(pack_id) && rollback["history"][pack_id].isArray() &&
            !rollback["history"][pack_id].empty()) {
            const auto previous = rollback["history"][pack_id][rollback["history"][pack_id].size() - 1];
            registry["packs"][pack_id]["version"] = previous["version"];
            registry["packs"][pack_id]["pack_root"] = previous["pack_root"];
            registry["packs"][pack_id]["manifest_path"] = previous["manifest_path"];
            registry["packs"][pack_id]["status"] = "RolledBack";
            storeRegistry(registry);
            publishEvent("load", "rolled_back_on_activation_failure", pack_id, user_id);
            return makeStatus("error", std::string("Activation failed, rolled back: ") + ex.what());
        }

        auto& user_entry = userPackEntry(registry, user_id, pack_id);
        user_entry["status"] = "Blocked";
        user_entry["last_error"] = ex.what();
        storeRegistry(registry);
        publishEvent("load", "activation_failed", pack_id, user_id);
        return makeStatus("error", ex.what());
    }
}

Json::Value PackManager::invoke(const std::string& user_id, const std::string& pack_id, const std::string& prompt, const std::string& options_json) {
    Json::Value registry = loadRegistry();
    const std::string key = runtimeKey(user_id, pack_id);
    if (runtimes_.find(key) == runtimes_.end()) {
        const Json::Value load_result = loadPack(user_id, pack_id);
        if (load_result["status"].asString() != "ok") {
            return load_result;
        }
        registry = loadRegistry();
    }

    Json::Value result = runtimes_.at(key).predict(prompt, options_json);
    auto& user_entry = userPackEntry(registry, user_id, pack_id);
    user_entry["last_invoked_prompt"] = prompt;
    storeRegistry(registry);

    Json::Value response = makeStatus("ok", "Invocation completed");
    response["result"] = result["result"];
    response["metadata"] = result["metadata"];
    publishEvent("usage", "invoked", pack_id, user_id, response["metadata"]);
    return response;
}

Json::Value PackManager::unloadPack(const std::string& user_id, const std::string& pack_id) {
    Json::Value registry = loadRegistry();
    const std::string key = runtimeKey(user_id, pack_id);
    auto it = runtimes_.find(key);
    if (it != runtimes_.end()) {
        it->second.unload();
        runtimes_.erase(it);
    }
    auto& user_entry = userPackEntry(registry, user_id, pack_id);
    user_entry["status"] = "Unloaded";
    storeRegistry(registry);
    publishEvent("unload", "unloaded", pack_id, user_id);
    return makeStatus("ok", "Pack unloaded");
}

Json::Value PackManager::disablePack(const std::string& user_id, const std::string& pack_id) {
    unloadPack(user_id, pack_id);
    Json::Value registry = loadRegistry();
    auto& user_entry = userPackEntry(registry, user_id, pack_id);
    user_entry["status"] = "Disabled";
    storeRegistry(registry);
    publishEvent("disable", "disabled", pack_id, user_id);
    return makeStatus("ok", "Pack disabled");
}

Json::Value PackManager::uninstallPack(const std::string& user_id, const std::string& pack_id, bool force_shared_users) {
    Json::Value registry = loadRegistry();
    if (!registry["packs"].isMember(pack_id)) {
        return makeStatus("skipped", "Pack already uninstalled");
    }

    if (registry["users"].isObject() && registry["users"].size() > 1 && !force_shared_users) {
        publishEvent("uninstall", "approval_required", pack_id, user_id);
        return makeStatus("approval_required", "Other users may still be using this pack");
    }

    unloadPack(user_id, pack_id);
    registry = loadRegistry();
    const Json::Value pack_entry = registry["packs"][pack_id];
    removePath(packRootFor(pack_entry));

    for (auto entry : std::filesystem::directory_iterator(staging_dir_)) {
        const auto filename = entry.path().filename().string();
        if (filename.rfind(pack_id + "-", 0) == 0) {
            removePath(entry.path());
        }
    }

    Json::Value rollback = loadRollbackRegistry();
    rollback["history"].removeMember(pack_id);
    storeRollbackRegistry(rollback);

    registry["packs"].removeMember(pack_id);
    for (auto user_name : registry["users"].getMemberNames()) {
        registry["users"][user_name].removeMember(pack_id);
    }
    storeRegistry(registry);
    publishEvent("uninstall", "uninstalled", pack_id, user_id);
    return makeStatus("ok", "Pack uninstalled");
}

Json::Value PackManager::rollbackPack(const std::string& user_id, const std::string& pack_id) {
    Json::Value rollback = loadRollbackRegistry();
    if (!rollback["history"].isMember(pack_id) || rollback["history"][pack_id].empty()) {
        publishEvent("rollback", "unavailable", pack_id, user_id);
        return makeStatus("error", "No rollback entry available");
    }

    const auto previous = rollback["history"][pack_id][rollback["history"][pack_id].size() - 1];
    Json::Value registry = loadRegistry();
    if (runtimes_.find(runtimeKey(user_id, pack_id)) != runtimes_.end()) {
        unloadPack(user_id, pack_id);
        registry = loadRegistry();
    }

    registry["packs"][pack_id]["version"] = previous["version"];
    registry["packs"][pack_id]["pack_root"] = previous["pack_root"];
    registry["packs"][pack_id]["manifest_path"] = previous["manifest_path"];
    registry["packs"][pack_id]["status"] = "RolledBack";
    storeRegistry(registry);
    publishEvent("rollback", "rolled_back", pack_id, user_id, previous);
    return makeStatus("ok", "Pack rolled back to previous version");
}

Json::Value PackManager::loadRegistry() const {
    if (!std::filesystem::exists(registry_path_)) {
        return emptyRegistry();
    }
    return parseJson(readTextFile(registry_path_));
}

void PackManager::storeRegistry(const Json::Value& registry) const {
    writeTextFile(registry_path_, toJsonString(registry, true));
}

Json::Value PackManager::loadRollbackRegistry() const {
    if (!std::filesystem::exists(rollback_path_)) {
        return emptyRollbackRegistry();
    }
    return parseJson(readTextFile(rollback_path_));
}

void PackManager::storeRollbackRegistry(const Json::Value& registry) const {
    writeTextFile(rollback_path_, toJsonString(registry, true));
}

Json::Value PackManager::fetchPackMetadata(const std::string& pack_id) const {
    return http_client_.getJson(catalog_url_ + "/packs/" + pack_id);
}

PackManifest PackManager::loadInstalledManifest(const Json::Value& pack_entry) const {
    return manifestFromFile(manifestPathFor(pack_entry));
}

std::filesystem::path PackManager::manifestPathFor(const Json::Value& pack_entry) const {
    return std::filesystem::path(pack_entry["manifest_path"].asString());
}

std::filesystem::path PackManager::packRootFor(const Json::Value& pack_entry) const {
    return std::filesystem::path(pack_entry["pack_root"].asString());
}

Json::Value PackManager::deviceCapabilityFor(const Json::Value& override_capability) const {
    if (override_capability.isObject() && !override_capability.empty()) {
        return override_capability;
    }
    return default_device_capability_;
}

bool PackManager::capabilityMatches(const Json::Value& required, const Json::Value& actual) const {
    if (!required.isObject()) {
        return true;
    }
    if (required.isMember("architecture")) {
        const auto device_arch = actual.get("architecture", "").asString();
        if (required["architecture"].isString() && required["architecture"].asString() != device_arch) {
            return false;
        }
        if (required["architecture"].isArray() && !jsonArrayContains(required["architecture"], device_arch)) {
            return false;
        }
    }
    if (required.isMember("min_ram_mb")) {
        if (actual.get("ram_mb", 0).asInt() < required["min_ram_mb"].asInt()) {
            return false;
        }
    }
    if (required.isMember("os_family")) {
        const auto device_os = actual.get("os_family", "").asString();
        if (required["os_family"].isString() && required["os_family"].asString() != device_os) {
            return false;
        }
        if (required["os_family"].isArray() && !jsonArrayContains(required["os_family"], device_os)) {
            return false;
        }
    }
    if (required.isMember("accelerators") && required["accelerators"].isArray()) {
        for (const auto& accelerator : required["accelerators"]) {
            if (!jsonArrayContains(actual["accelerators"], accelerator.asString())) {
                return false;
            }
        }
    }
    return true;
}

void PackManager::publishEvent(const std::string& phase,
                               const std::string& status,
                               const std::string& pack_id,
                               const std::string& user_id,
                               const Json::Value& extra) const {
    Json::Value event(Json::objectValue);
    event["phase"] = phase;
    event["status"] = status;
    event["pack_id"] = pack_id;
    event["user_id"] = user_id;
    if (!extra.isNull()) {
        event["details"] = extra;
    }
    if (sink_) {
        sink_->publish(event);
    }
}

}  // namespace edgeai
