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
    registry["capabilities"] = Json::Value(Json::arrayValue);
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

Json::Value PackManager::handleUserRequest(const std::string& user_id, const std::string& skill, const Json::Value& device_capability) {
    initialize();
    Json::Value registry = loadRegistry();
    const Json::Value actual_device_capability = deviceCapabilityFor(device_capability);

    const Json::Value capability_response = fetchCapabilityList();
    const Json::Value capability_list = capability_response.get("capabilities", Json::Value(Json::arrayValue));
    registry["capabilities"] = capability_list;
    storeRegistry(registry);

    std::string capability;
    try {
        capability = identifyCapability(skill, capability_list);
    } catch (const std::exception&) {
        publishEvent("discovery", "not_capable", "", user_id);
        return makeStatus("error", "Requested skill is not supported");
    }

    Json::Value response(Json::objectValue);
    response["status"] = "ok";
    response["skill"] = skill;
    response["capability"] = capability;
    response["device_capability"] = actual_device_capability;

    Json::Value local_pack = findLocalPackForCapability(registry, capability, actual_device_capability);
    if (local_pack.get("found", false).asBool()) {
        const std::string pack_id = local_pack["pack_id"].asString();
        response["source"] = "local";
        response["pack"] = local_pack["pack"];
        response["pack_id"] = pack_id;

        if (local_pack.get("installed", false).asBool()) {
            response["message"] = "Pack is already installed and ready to use";
            publishEvent("discovery", "local_pack_ready", pack_id, user_id, response["pack"]);
            return response;
        }

        const Json::Value install_result = installPack(user_id, pack_id, true);
        response["message"] = "Local pack installation initiated";
        response["install_result"] = install_result;
        return response;
    }

    const Json::Value cloud_packs = queryPacks(capability, actual_device_capability);
    response["source"] = "cloud";
    response["packs"] = cloud_packs.get("packs", Json::Value(Json::arrayValue));
    response["count"] = response["packs"].size();
    if (response["count"].asUInt() == 0U) {
        response["status"] = "error";
        response["message"] = "No compatible packs found for capability";
        publishEvent("discovery", "no_compatible_pack", "", user_id, response);
    } else {
        response["message"] = "Compatible cloud packs available";
        publishEvent("discovery", "cloud_pack_candidates", "", user_id, response["packs"]);
    }
    return response;
}

Json::Value PackManager::queryPacks(const std::string& capability, const Json::Value& device_capability) {
    Json::Value payload(Json::objectValue);
    payload["capability"] = capability;
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
        const Json::Value dependency_result = installPack(user_id, dependency.asString(), true);
        if (dependency_result.get("status", "").asString() == "error") {
            return dependency_result;
        }
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
    installed["tags"] = metadata["tags"];
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
    if (!isPackInstalled(registry["packs"][pack_id])) {
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

    const std::string state = userPackState(registry, user_id, pack_id);
    if (state == "Disabled" || state == "Installed" || state.empty()) {
        publishEvent("load", "load_failed_disabled", pack_id, user_id);
        return makeStatus("error", "Pack is disabled, cannot load");
    }
    if (state == "Loaded" && runtimes_.find(runtimeKey(user_id, pack_id)) != runtimes_.end()) {
        return makeStatus("ok", "Pack already loaded");
    }

    const Json::Value pack_entry = registry["packs"][pack_id];
    const PackManifest manifest = loadInstalledManifest(pack_entry);
    const std::string key = runtimeKey(user_id, pack_id);

    const int abi_version = PackRuntime::readAbiVersion(packRootFor(pack_entry), manifest);
    if (abi_version != EDGEAI_PACK_ABI_V1) {
        auto& user_entry = userPackEntry(registry, user_id, pack_id);
        user_entry["status"] = "incompatible_abi";
        storeRegistry(registry);
        publishEvent("load", "incompatible_abi", pack_id, user_id);
        return makeStatus("error", "Pack cannot be loaded on this runtime");
    }

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
        auto& user_entry = userPackEntry(registry, user_id, pack_id);
        user_entry["status"] = "activation_failed";
        user_entry["last_error"] = ex.what();
        storeRegistry(registry);
        publishEvent("load", "activation_failed", pack_id, user_id);
        return makeStatus("error", ex.what());
    }
}

Json::Value PackManager::invoke(const std::string& user_id, const std::string& pack_id, const std::string& prompt, const std::string& options_json) {
    Json::Value registry = loadRegistry();
    const std::string key = runtimeKey(user_id, pack_id);
    const std::string state = userPackState(registry, user_id, pack_id);
    if (state != "Loaded" || runtimes_.find(key) == runtimes_.end()) {
        publishEvent("usage", "loading_required", pack_id, user_id);
        return makeStatus("error", "Loading Phase should be initiated before usage");
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
    const std::string state = userPackState(registry, user_id, pack_id);
    if (state != "Loaded") {
        return makeStatus("skipped", "Pack is not loaded");
    }
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
    Json::Value registry = loadRegistry();
    if (userPackState(registry, user_id, pack_id) == "Loaded") {
        unloadPack(user_id, pack_id);
        registry = loadRegistry();
    }
    auto& user_entry = userPackEntry(registry, user_id, pack_id);
    user_entry["status"] = "Disabled";
    storeRegistry(registry);
    publishEvent("disable", "disabled", pack_id, user_id);
    return makeStatus("ok", "Pack disabled");
}

Json::Value PackManager::uninstallPack(const std::string& user_id, const std::string& pack_id, bool force_shared_users) {
    (void)force_shared_users;
    Json::Value registry = loadRegistry();
    if (!registry["packs"].isMember(pack_id)) {
        return makeStatus("skipped", "Pack already uninstalled");
    }
    if (userPackState(registry, user_id, pack_id) == "Loaded") {
        unloadPack(user_id, pack_id);
        registry = loadRegistry();
    }
    const Json::Value pack_entry = registry["packs"][pack_id];

    for (const auto& dependency : pack_entry["dependencies"]) {
        const std::string dependency_id = dependency.asString();
        if (registry["packs"].isMember(dependency_id)) {
            uninstallPack(user_id, dependency_id, true);
            registry = loadRegistry();
        }
    }

    removePath(packRootFor(pack_entry));
    for (auto entry : std::filesystem::directory_iterator(staging_dir_)) {
        const auto filename = entry.path().filename().string();
        if (filename.rfind(pack_id + "-", 0) == 0) {
            removePath(entry.path());
        }
    }
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

Json::Value PackManager::fetchCapabilityList() const {
    return http_client_.getJson(catalog_url_ + "/capabilities");
}

std::string PackManager::identifyCapability(const std::string& skill, const Json::Value& capability_list) const {
    Json::Value payload(Json::objectValue);
    payload["skill"] = skill;
    payload["capability_list"] = capability_list;
    const Json::Value response = http_client_.postJson(catalog_url_ + "/capabilities/identify", payload);
    if (response.get("status", "error").asString() != "ok" || !response.isMember("capability")) {
        throw std::runtime_error("Capability could not be identified");
    }
    return response["capability"].asString();
}

Json::Value PackManager::fetchPackMetadata(const std::string& pack_id) const {
    return http_client_.getJson(catalog_url_ + "/packs/" + pack_id);
}

Json::Value PackManager::findLocalPackForCapability(const Json::Value& registry,
                                                    const std::string& capability,
                                                    const Json::Value& device_capability) const {
    Json::Value result(Json::objectValue);
    result["found"] = false;

    if (!registry.isMember("packs") || !registry["packs"].isObject()) {
        return result;
    }

    for (const auto& pack_id : registry["packs"].getMemberNames()) {
        const Json::Value& pack_entry = registry["packs"][pack_id];
        if (!packSupportsCapability(pack_entry, capability)) {
            continue;
        }
        if (!capabilityMatches(pack_entry["device_capability"], device_capability)) {
            continue;
        }
        result["found"] = true;
        result["pack_id"] = pack_id;
        result["pack"] = pack_entry;
        result["installed"] = isPackInstalled(pack_entry);
        if (result["installed"].asBool()) {
            return result;
        }
    }

    return result;
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

std::string PackManager::userPackState(const Json::Value& registry, const std::string& user_id, const std::string& pack_id) const {
    if (registry.isMember("users") &&
        registry["users"].isMember(user_id) &&
        registry["users"][user_id].isMember(pack_id) &&
        registry["users"][user_id][pack_id].isMember("status")) {
        return registry["users"][user_id][pack_id]["status"].asString();
    }

    if (registry.isMember("packs") && registry["packs"].isMember(pack_id)) {
        return normalizeStatus(registry["packs"][pack_id], "");
    }
    return "";
}

bool PackManager::isPackInstalled(const Json::Value& pack_entry) const {
    const std::string status = normalizeStatus(pack_entry, "");
    if (status.empty() || status == "Uninstalled" || status == "InstallFailed") {
        return false;
    }

    const std::string pack_root = pack_entry.get("pack_root", "").asString();
    const std::string manifest_path = pack_entry.get("manifest_path", "").asString();
    if (!pack_root.empty() && std::filesystem::exists(pack_root)) {
        return true;
    }
    if (!manifest_path.empty() && std::filesystem::exists(manifest_path)) {
        return true;
    }
    return status == "Installed" || status == "Enabled" || status == "Loaded" || status == "Unloaded" || status == "Disabled";
}

bool PackManager::packSupportsCapability(const Json::Value& pack_entry, const std::string& capability) const {
    if (pack_entry.get("intent", "").asString() == capability) {
        return true;
    }
    if (pack_entry.isMember("tags") && jsonArrayContains(pack_entry["tags"], capability)) {
        return true;
    }
    if (pack_entry.isMember("ai_capability")) {
        const Json::Value& ai_capability = pack_entry["ai_capability"];
        if (ai_capability.get("task", "").asString() == capability) {
            return true;
        }
        if (ai_capability.isMember("keywords") && jsonArrayContains(ai_capability["keywords"], capability)) {
            return true;
        }
    }
    return false;
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
