#include <edgeai/fs_utils.h>
#include <edgeai/json_utils.h>
#include <edgeai/pack_manager.h>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <stdexcept>
#include <tuple>
#include <vector>

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
    registry["pack_server_cache"] = Json::Value(Json::objectValue);
    registry["pack_server_cache"]["compatible_packs"] = Json::Value(Json::objectValue);
    registry["pack_server_cache"]["pack_details"] = Json::Value(Json::objectValue);
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

std::string normalizeText(const std::string& text) {
    std::string normalized;
    normalized.reserve(text.size());
    for (char ch : text) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
            normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        } else {
            normalized.push_back(' ');
        }
    }

    std::string collapsed;
    collapsed.reserve(normalized.size());
    bool previous_space = true;
    for (char ch : normalized) {
        if (ch == ' ') {
            if (!previous_space) {
                collapsed.push_back(ch);
            }
            previous_space = true;
        } else {
            collapsed.push_back(ch);
            previous_space = false;
        }
    }
    while (!collapsed.empty() && collapsed.front() == ' ') {
        collapsed.erase(collapsed.begin());
    }
    while (!collapsed.empty() && collapsed.back() == ' ') {
        collapsed.pop_back();
    }
    return collapsed;
}

std::vector<std::string> splitTerms(const std::string& normalized) {
    std::vector<std::string> terms;
    std::string current;
    for (char ch : normalized) {
        if (ch == ' ') {
            if (!current.empty()) {
                terms.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        terms.push_back(current);
    }
    return terms;
}

bool containsTerm(const std::vector<std::string>& haystack, const std::string& needle) {
    return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

int intersectionScore(const std::vector<std::string>& left, const std::vector<std::string>& right) {
    int matches = 0;
    for (const auto& term : left) {
        if (containsTerm(right, term)) {
            ++matches;
        }
    }
    return matches;
}

void logPackManager(const std::string& message) {
    std::cerr << "[PackManager] " << message << std::endl;
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
    logPackManager("Initializing state_dir=" + state_dir_.string() +
                   " staging_dir=" + staging_dir_.string() +
                   " install_dir=" + install_dir_.string());
    ensureDirectory(state_dir_);
    ensureDirectory(staging_dir_);
    ensureDirectory(install_dir_);
    if (!std::filesystem::exists(registry_path_)) {
        storeRegistry(emptyRegistry());
    }
    if (!std::filesystem::exists(rollback_path_)) {
        storeRollbackRegistry(emptyRollbackRegistry());
    }
    try {
        Json::Value registry = loadRegistry();
        if (!registry.isMember("pack_server_cache") || !registry["pack_server_cache"].isObject()) {
            registry["pack_server_cache"] = Json::Value(Json::objectValue);
            registry["pack_server_cache"]["compatible_packs"] = Json::Value(Json::objectValue);
            registry["pack_server_cache"]["pack_details"] = Json::Value(Json::objectValue);
            storeRegistry(registry);
        }
        if (!registry.isMember("capabilities") || !registry["capabilities"].isArray() || registry["capabilities"].empty()) {
            logPackManager("Capability cache empty, refreshing from pack server");
            refreshCapabilityList(&registry);
        }
    } catch (const std::exception& ex) {
        logPackManager(std::string("Initialization continued without remote refresh: ") + ex.what());
    }
    return makeStatus("ok", "PackManager initialized");
}

Json::Value PackManager::getCapabilityList() {
    initialize();
    Json::Value registry = loadRegistry();
    Json::Value capabilities = registry.get("capabilities", Json::Value(Json::arrayValue));
    if (!capabilities.isArray() || capabilities.empty()) {
        const Json::Value refreshed = refreshCapabilityList(&registry);
        capabilities = refreshed.get("capabilities", Json::Value(Json::arrayValue));
    }
    logPackManager("Returning capability list count=" + std::to_string(capabilities.size()));

    Json::Value response(Json::objectValue);
    response["capabilities"] = capabilities;
    response["count"] = capabilities.size();
    return response;
}

Json::Value PackManager::cacheCapabilityList(const Json::Value& capability_response) const {
    Json::Value registry = loadRegistry();
    registry["capabilities"] = capability_response.get("capabilities", Json::Value(Json::arrayValue));
    registry["pack_server_cache"]["capability_list"] = capability_response;
    storeRegistry(registry);
    logPackManager("Cached capability list count=" +
                   std::to_string(capability_response.get("count", registry["capabilities"].size()).asUInt()));
    return capability_response;
}

Json::Value PackManager::cacheCompatiblePackList(const std::string& capability,
                                                 const Json::Value& device_capability,
                                                 const Json::Value& compatible_response) const {
    Json::Value registry = loadRegistry();
    Json::Value cached(Json::objectValue);
    cached["capability"] = capability;
    cached["device_capability"] = device_capability;
    cached["response"] = compatible_response;
    registry["pack_server_cache"]["compatible_packs"][capability] = cached;
    const Json::Value packs = compatible_response.get("packs", Json::Value(Json::arrayValue));
    for (const auto& pack : packs) {
        if (pack.isObject() && pack.isMember("pack_id")) {
            registry["pack_server_cache"]["pack_summaries"][pack["pack_id"].asString()] = pack;
        }
    }
    storeRegistry(registry);
    logPackManager("Cached compatible pack list capability=" + capability +
                   " count=" + std::to_string(compatible_response.get("count", packs.size()).asUInt()));
    return compatible_response;
}

Json::Value PackManager::getLocalPacks(const std::string& capability) {
    initialize();
    logPackManager("Checking local packs for capability=" + capability);
    Json::Value registry = loadRegistry();
    std::vector<Json::Value> matches;
    if (registry.isMember("packs") && registry["packs"].isObject()) {
        for (const auto& pack_id : registry["packs"].getMemberNames()) {
            const Json::Value& pack_entry = registry["packs"][pack_id];
            if (!packSupportsCapability(pack_entry, capability)) {
                continue;
            }

            Json::Value pack(Json::objectValue);
            pack["pack_id"] = pack_id;
            pack["pack"] = pack_entry;
            pack["installed"] = isPackInstalled(pack_entry);
            pack["state"] = normalizeStatus(pack_entry, "");
            matches.push_back(pack);
        }
    }

    std::sort(matches.begin(), matches.end(), [](const Json::Value& left, const Json::Value& right) {
        const bool left_installed = left.get("installed", false).asBool();
        const bool right_installed = right.get("installed", false).asBool();
        if (left_installed != right_installed) {
            return left_installed && !right_installed;
        }
        return left.get("pack_id", "").asString() < right.get("pack_id", "").asString();
    });

    Json::Value packs(Json::arrayValue);
    for (const auto& pack : matches) {
        packs.append(pack);
    }

    Json::Value response(Json::objectValue);
    response["packs"] = packs;
    response["count"] = packs.size();
    logPackManager("Local pack lookup capability=" + capability + " count=" + std::to_string(packs.size()));
    return response;
}

Json::Value PackManager::preparePackForUse(const std::string& user_id, const std::string& pack_id, bool approve_dependencies) {
    initialize();
    logPackManager("Preparing pack for use pack_id=" + pack_id + " user_id=" + user_id);
    Json::Value registry = loadRegistry();
    if (!registry["packs"].isMember(pack_id)) {
        return makeStatus("error", "Pack is not registered locally");
    }

    const Json::Value pack_entry = registry["packs"][pack_id];
    Json::Value response(Json::objectValue);
    response["pack_id"] = pack_id;
    response["pack"] = pack_entry;

    if (!isPackInstalled(pack_entry)) {
        response["status"] = "install_started";
        response["install_result"] = installPack(user_id, pack_id, approve_dependencies);
        return response;
    }

    const std::string state = userPackState(registry, user_id, pack_id);
    if (isPackEnabledState(state)) {
        response["status"] = "ready";
        response["message"] = "Pack is already installed and ready to use";
        return response;
    }

    publishEvent("discovery", "pack_disabled", pack_id, user_id);
    response["status"] = "disabled";
    response["message"] = "Pack is currently disabled, enable it to use";
    return response;
}

Json::Value PackManager::handleUserRequest(const std::string& user_id, const std::string& skill, const Json::Value& device_capability) {
    initialize();
    const Json::Value actual_device_capability = deviceCapabilityFor(device_capability);

    const Json::Value capability_response = getCapabilityList();
    const Json::Value capability_list = capability_response.get("capabilities", Json::Value(Json::arrayValue));

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

    Json::Value local_packs = getLocalPacks(capability);
    if (local_packs.get("count", 0).asUInt() > 0U) {
        const Json::Value local_pack = local_packs["packs"][0];
        const std::string pack_id = local_pack.get("pack_id", "").asString();
        response["source"] = "local";
        response["pack"] = local_pack["pack"];
        response["pack_id"] = pack_id;
        response["prepare_result"] = preparePackForUse(user_id, pack_id, true);
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
    const Json::Value actual_device_capability = deviceCapabilityFor(device_capability);
    const std::string url = catalog_url_ + "/getCompatiblePackList?capability=" + http_client_.urlEncode(capability) +
                            "&device_capability=" + http_client_.urlEncode(toJsonString(actual_device_capability));
    logPackManager("Querying compatible packs capability=" + capability +
                   " device_capability=" + toJsonString(actual_device_capability));
    Json::Value response = http_client_.getJson(url);
    cacheCompatiblePackList(capability, actual_device_capability, response);

    Json::Value details_by_pack(Json::objectValue);
    const Json::Value packs = response.get("packs", Json::Value(Json::arrayValue));
    for (const auto& pack : packs) {
        if (!pack.isObject() || !pack.isMember("pack_id")) {
            continue;
        }
        const std::string pack_id = pack["pack_id"].asString();
        try {
            details_by_pack[pack_id] = fetchPackMetadata(pack_id);
        } catch (const std::exception& ex) {
            Json::Value error(Json::objectValue);
            error["status"] = "error";
            error["message"] = ex.what();
            details_by_pack[pack_id] = error;
            logPackManager("Failed to prefetch pack details pack_id=" + pack_id + " error=" + ex.what());
        }
    }
    response["pack_details"] = details_by_pack;
    response["details_cached_count"] = details_by_pack.size();
    logPackManager("Compatible pack query completed capability=" + capability +
                   " summaries=" + std::to_string(packs.size()) +
                   " details_cached=" + std::to_string(details_by_pack.size()));
    return response;
}

Json::Value PackManager::installPack(const std::string& user_id, const std::string& pack_id, bool approve_dependencies) {
    initialize();
    logPackManager("Starting install phase pack_id=" + pack_id + " user_id=" + user_id);
    Json::Value registry = loadRegistry();
    Json::Value metadata = fetchPackMetadata(pack_id);
    Json::Value pack_entry = registry["packs"][pack_id];
    const std::string version = metadata["version"].asString();
    const std::string installed_version = pack_entry.get("version", "").asString();

    if (!installed_version.empty() && installed_version == version && normalizeStatus(pack_entry) != "Uninstalled") {
        return makeStatus("skipped", "Pack already installed at requested version");
    }

    const std::string package_url = metadata.get("package_url", "").asString();
    if (package_url.empty()) {
        publishEvent("install", "package_url_missing", pack_id, user_id);
        return makeStatus("error", "Pack details did not include a package URL");
    }
    const auto staged_archive = staging_dir_ / (pack_id + "-" + version + ".tar.gz");
    logPackManager("Downloading pack archive pack_id=" + pack_id + " url=" + package_url);
    http_client_.downloadToFile(package_url, staged_archive);
    const std::string actual_md5 = computeMd5(staged_archive);
    const std::string expected_md5 = metadata.get("md5", "").asString();
    if (!expected_md5.empty() && expected_md5 != actual_md5) {
        registry["packs"][pack_id]["status"] = "InstallFailed";
        registry["packs"][pack_id]["last_error"] = "checksum mismatch";
        storeRegistry(registry);
        publishEvent("install", "verification_failed", pack_id, user_id);
        return makeStatus("error", "Downloaded pack failed md5 verification");
    }
    if (expected_md5.empty()) {
        logPackManager("Pack server did not provide md5 for pack_id=" + pack_id + ", skipping md5 verification");
    } else {
        logPackManager("Verified md5 for pack_id=" + pack_id);
    }

    if (!capabilityMatches(metadata["device_capability"], default_device_capability_)) {
        registry["packs"][pack_id]["status"] = "Blocked";
        registry["packs"][pack_id]["last_error"] = "device capability mismatch";
        storeRegistry(registry);
        publishEvent("install", "blocked_incompatible_capability", pack_id, user_id);
        return makeStatus("error", "Pack is not compatible with this device");
    }

    if (metadata["dependencies"].isArray() && metadata["dependencies"].size() > 0 && !approve_dependencies) {
        logPackManager("Dependency approval required for pack_id=" + pack_id);
        publishEvent("install", "dependency_approval_required", pack_id, user_id);
        return makeStatus("approval_required", "Dependency approval is required");
    }

    for (const auto& dependency : metadata["dependencies"]) {
        logPackManager("Installing dependency pack_id=" + dependency.asString() + " for parent_pack=" + pack_id);
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
    logPackManager("Installed bundle into " + install_root.string());

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
    installed["pack_description"] = metadata.get("pack_description", "");
    installed["pack_monetization"] = metadata.get("pack_monetization", Json::Value(Json::objectValue));
    installed["pack_server_details"] = metadata.get("pack_server_details", Json::Value(Json::objectValue));
    registry["packs"][pack_id] = installed;
    storeRegistry(registry);
    publishEvent("install", "installed", pack_id, user_id, installed);

    Json::Value result = makeStatus("ok", "Pack installed");
    result["pack"] = installed;
    return result;
}

Json::Value PackManager::enablePack(const std::string& user_id, const std::string& pack_id) {
    logPackManager("Enable requested pack_id=" + pack_id + " user_id=" + user_id);
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
    logPackManager("Pack state changed pack_id=" + pack_id + " user_id=" + user_id + " -> Enabled");
    publishEvent("enable", "enabled", pack_id, user_id);
    return makeStatus("ok", "Pack enabled");
}

Json::Value PackManager::loadPack(const std::string& user_id, const std::string& pack_id) {
    logPackManager("Load requested pack_id=" + pack_id + " user_id=" + user_id);
    Json::Value registry = loadRegistry();
    if (!registry["packs"].isMember(pack_id)) {
        return makeStatus("error", "Pack is not installed");
    }

    const std::string state = userPackState(registry, user_id, pack_id);
    if (state == "Disabled" || state == "Installed" || state.empty()) {
        publishEvent("load", "load_failed_disabled", pack_id, user_id);
        return makeStatus("error", "Pack is disabled, cannot load");
    }
    const std::string key = runtimeKey(user_id, pack_id);
    if (state == "Loaded") {
        std::lock_guard<std::mutex> lock(runtimes_mutex_);
        if (runtimes_.find(key) != runtimes_.end()) {
            return makeStatus("ok", "Pack already loaded");
        }
    }

    const Json::Value pack_entry = registry["packs"][pack_id];
    const PackManifest manifest = loadInstalledManifest(pack_entry);

    const int abi_version = PackRuntime::readAbiVersion(packRootFor(pack_entry), manifest);
    if (abi_version != EDGEAI_PACK_ABI_V1) {
        auto& user_entry = userPackEntry(registry, user_id, pack_id);
        user_entry["status"] = "incompatible_abi";
        storeRegistry(registry);
        logPackManager("Pack state changed pack_id=" + pack_id + " user_id=" + user_id + " -> incompatible_abi");
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
        {
            std::lock_guard<std::mutex> lock(runtimes_mutex_);
            runtimes_[key] = std::move(runtime);
        }

        auto& user_entry = userPackEntry(registry, user_id, pack_id);
        user_entry["status"] = "Loaded";
        storeRegistry(registry);
        logPackManager("Pack state changed pack_id=" + pack_id + " user_id=" + user_id + " -> Loaded");
        publishEvent("load", "loaded", pack_id, user_id);
        return makeStatus("ok", "Pack loaded");
    } catch (const std::exception& ex) {
        auto& user_entry = userPackEntry(registry, user_id, pack_id);
        user_entry["status"] = "activation_failed";
        user_entry["last_error"] = ex.what();
        storeRegistry(registry);
        logPackManager("Pack activation failed pack_id=" + pack_id + " user_id=" + user_id + " error=" + ex.what());
        publishEvent("load", "activation_failed", pack_id, user_id);
        return makeStatus("error", ex.what());
    }
}

Json::Value PackManager::invoke(const std::string& user_id, const std::string& pack_id, const std::string& prompt, const std::string& options_json) {
    logPackManager("Invoke requested pack_id=" + pack_id + " user_id=" + user_id + " prompt=" + prompt);
    Json::Value registry = loadRegistry();
    const std::string key = runtimeKey(user_id, pack_id);
    const std::string state = userPackState(registry, user_id, pack_id);
    Json::Value result;
    {
        std::lock_guard<std::mutex> lock(runtimes_mutex_);
        if (state != "Loaded" || runtimes_.find(key) == runtimes_.end()) {
            publishEvent("usage", "loading_required", pack_id, user_id);
            return makeStatus("error", "Loading Phase should be initiated before usage");
        }
        result = runtimes_.at(key).predict(prompt, options_json);
    }
    auto& user_entry = userPackEntry(registry, user_id, pack_id);
    user_entry["last_invoked_prompt"] = prompt;
    storeRegistry(registry);
    logPackManager("Invocation completed pack_id=" + pack_id + " user_id=" + user_id);

    Json::Value response = makeStatus("ok", "Invocation completed");
    response["result"] = result["result"];
    response["metadata"] = result["metadata"];
    publishEvent("usage", "invoked", pack_id, user_id, response["metadata"]);
    return response;
}

Json::Value PackManager::unloadPack(const std::string& user_id, const std::string& pack_id) {
    logPackManager("Unload requested pack_id=" + pack_id + " user_id=" + user_id);
    Json::Value registry = loadRegistry();
    const std::string state = userPackState(registry, user_id, pack_id);
    if (state != "Loaded") {
        return makeStatus("skipped", "Pack is not loaded");
    }
    const std::string key = runtimeKey(user_id, pack_id);
    {
        std::lock_guard<std::mutex> lock(runtimes_mutex_);
        auto it = runtimes_.find(key);
        if (it != runtimes_.end()) {
            it->second.unload();
            runtimes_.erase(it);
        }
    }
    auto& user_entry = userPackEntry(registry, user_id, pack_id);
    user_entry["status"] = "Unloaded";
    storeRegistry(registry);
    logPackManager("Pack state changed pack_id=" + pack_id + " user_id=" + user_id + " -> Unloaded");
    publishEvent("unload", "unloaded", pack_id, user_id);
    return makeStatus("ok", "Pack unloaded");
}

Json::Value PackManager::disablePack(const std::string& user_id, const std::string& pack_id) {
    logPackManager("Disable requested pack_id=" + pack_id + " user_id=" + user_id);
    Json::Value registry = loadRegistry();
    if (userPackState(registry, user_id, pack_id) == "Loaded") {
        unloadPack(user_id, pack_id);
        registry = loadRegistry();
    }
    auto& user_entry = userPackEntry(registry, user_id, pack_id);
    user_entry["status"] = "Disabled";
    storeRegistry(registry);
    logPackManager("Pack state changed pack_id=" + pack_id + " user_id=" + user_id + " -> Disabled");
    publishEvent("disable", "disabled", pack_id, user_id);
    return makeStatus("ok", "Pack disabled");
}

Json::Value PackManager::uninstallPack(const std::string& user_id, const std::string& pack_id, bool force_shared_users) {
    logPackManager("Uninstall requested pack_id=" + pack_id + " user_id=" + user_id +
                   " force_shared_users=" + std::string(force_shared_users ? "true" : "false"));
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
    logPackManager("Pack state changed pack_id=" + pack_id + " -> Uninstalled");
    publishEvent("uninstall", "uninstalled", pack_id, user_id);
    return makeStatus("ok", "Pack uninstalled");
}

Json::Value PackManager::rollbackPack(const std::string& user_id, const std::string& pack_id) {
    logPackManager("Rollback requested pack_id=" + pack_id + " user_id=" + user_id);
    Json::Value rollback = loadRollbackRegistry();
    if (!rollback["history"].isMember(pack_id) || rollback["history"][pack_id].empty()) {
        publishEvent("rollback", "unavailable", pack_id, user_id);
        return makeStatus("error", "No rollback entry available");
    }

    const auto previous = rollback["history"][pack_id][rollback["history"][pack_id].size() - 1];
    Json::Value registry = loadRegistry();
    bool loaded = false;
    {
        std::lock_guard<std::mutex> lock(runtimes_mutex_);
        loaded = runtimes_.find(runtimeKey(user_id, pack_id)) != runtimes_.end();
    }
    if (loaded) {
        unloadPack(user_id, pack_id);
        registry = loadRegistry();
    }

    registry["packs"][pack_id]["version"] = previous["version"];
    registry["packs"][pack_id]["pack_root"] = previous["pack_root"];
    registry["packs"][pack_id]["manifest_path"] = previous["manifest_path"];
    registry["packs"][pack_id]["status"] = "RolledBack";
    storeRegistry(registry);
    logPackManager("Pack state changed pack_id=" + pack_id + " -> RolledBack");
    publishEvent("rollback", "rolled_back", pack_id, user_id, previous);
    return makeStatus("ok", "Pack rolled back to previous version");
}

Json::Value PackManager::loadRegistry() const {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    if (!std::filesystem::exists(registry_path_)) {
        return emptyRegistry();
    }
    return parseJson(readTextFile(registry_path_));
}

void PackManager::storeRegistry(const Json::Value& registry) const {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    writeTextFile(registry_path_, toJsonString(registry, true));
}

Json::Value PackManager::loadRollbackRegistry() const {
    std::lock_guard<std::mutex> lock(rollback_mutex_);
    if (!std::filesystem::exists(rollback_path_)) {
        return emptyRollbackRegistry();
    }
    return parseJson(readTextFile(rollback_path_));
}

void PackManager::storeRollbackRegistry(const Json::Value& registry) const {
    std::lock_guard<std::mutex> lock(rollback_mutex_);
    writeTextFile(rollback_path_, toJsonString(registry, true));
}

Json::Value PackManager::refreshCapabilityList(Json::Value* registry) const {
    Json::Value working = registry ? *registry : loadRegistry();
    const Json::Value response = fetchCapabilityList();
    if (response.isMember("capabilities") && response["capabilities"].isArray()) {
        working["capabilities"] = response["capabilities"];
        working["pack_server_cache"]["capability_list"] = response;
        storeRegistry(working);
        if (registry) {
            *registry = working;
        }
        logPackManager("Refreshed capability list from pack server count=" + std::to_string(response["capabilities"].size()));
    }
    return response;
}

Json::Value PackManager::fetchCapabilityList() const {
    const std::string url = catalog_url_ + "/getCapabilityList";
    logPackManager("Fetching capability list url=" + url);
    return http_client_.getJson(url);
}

std::string PackManager::identifyCapability(const std::string& skill, const Json::Value& capability_list) const {
    const std::string normalized_skill = normalizeText(skill);
    if (normalized_skill.empty()) {
        throw std::runtime_error("Capability could not be identified");
    }

    const auto skill_terms = splitTerms(normalized_skill);
    std::vector<std::tuple<int, std::size_t, std::string>> ranked;
    for (const auto& item : capability_list) {
        if (!item.isString()) {
            continue;
        }
        const std::string capability = item.asString();
        const std::string normalized_capability = normalizeText(capability);
        if (normalized_capability.empty()) {
            continue;
        }

        int score = 0;
        if (normalized_capability == normalized_skill) {
            score += 100;
        }
        if (normalized_skill.find(normalized_capability) != std::string::npos) {
            score += 20;
        }
        score += intersectionScore(skill_terms, splitTerms(normalized_capability)) * 5;
        if (score > 0) {
            ranked.emplace_back(score, normalized_capability.size(), capability);
        }
    }

    if (ranked.empty()) {
        throw std::runtime_error("Capability could not be identified");
    }

    std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
        if (std::get<0>(left) != std::get<0>(right)) {
            return std::get<0>(left) > std::get<0>(right);
        }
        if (std::get<1>(left) != std::get<1>(right)) {
            return std::get<1>(left) < std::get<1>(right);
        }
        return std::get<2>(left) < std::get<2>(right);
    });

    const std::string capability = std::get<2>(ranked.front());
    logPackManager("Identified capability='" + capability + "' from skill='" + skill + "'");
    return capability;
}

Json::Value PackManager::fetchPackMetadata(const std::string& pack_id) const {
    const std::string url = catalog_url_ + "/getPackDetails?pack_id=" + http_client_.urlEncode(pack_id);
    logPackManager("Fetching pack details pack_id=" + pack_id + " url=" + url);
    const Json::Value pack_details = http_client_.getJson(url);
    cachePackServerDetails(pack_id, pack_details);
    return normalizePackServerDetails(pack_details);
}

Json::Value PackManager::cachePackServerDetails(const std::string& pack_id, const Json::Value& pack_details) const {
    Json::Value registry = loadRegistry();
    registry["pack_server_cache"]["pack_details"][pack_id] = pack_details;
    storeRegistry(registry);
    logPackManager("Cached pack details pack_id=" + pack_id);
    return pack_details;
}

Json::Value PackManager::normalizePackServerDetails(const Json::Value& pack_details) const {
    const Json::Value capability = pack_details.get("capability", Json::Value(Json::objectValue));
    const Json::Value package = pack_details.get("package", Json::Value(Json::objectValue));
    const Json::Value monetization = pack_details.get("monetization", Json::Value(Json::objectValue));
    const Json::Value runtime = pack_details.get("runtime_descriptor", Json::Value(Json::objectValue));

    Json::Value normalized(Json::objectValue);
    normalized["pack_id"] = package.get("pack_id", "");
    normalized["name"] = package.get("pack_name", capability.get("name", ""));
    normalized["version"] = capability.get("version", package.get("version", ""));
    normalized["package_url"] = package.get("pack_url", package.get("bundle_path", package.get("bundle_file", "")));
    const std::string checksum = package.get("checksum", "").asString();
    normalized["checksum"] = checksum;
    if (checksum.rfind("md5:", 0) == 0) {
        normalized["md5"] = checksum.substr(4);
    } else {
        normalized["md5"] = "";
    }
    normalized["license"] = capability.get("license", "");
    normalized["intent"] = capability.get("slug", "");
    normalized["metering_unit"] = monetization.get("model", "usage");
    normalized["dependencies"] = Json::Value(Json::arrayValue);

    Json::Value device_capability(Json::objectValue);
    if (runtime.isMember("memory_required_mb")) {
        device_capability["min_ram_mb"] = runtime["memory_required_mb"];
    }
    if (runtime.isMember("cpu_cores_recommended")) {
        device_capability["min_cpu_cores"] = runtime["cpu_cores_recommended"];
    }
    Json::Value accelerators(Json::arrayValue);
    if (runtime.get("gpu_required", false).asBool()) {
        accelerators.append("gpu");
    }
    device_capability["accelerators"] = accelerators;
    normalized["device_capability"] = device_capability;

    Json::Value ai_capability(Json::objectValue);
    ai_capability["task"] = capability.get("slug", "");
    ai_capability["name"] = capability.get("name", "");
    ai_capability["description"] = capability.get("description", "");
    ai_capability["category"] = capability.get("category", "");
    ai_capability["keywords"] = capability.get("tags", Json::Value(Json::arrayValue));
    normalized["ai_capability"] = ai_capability;

    Json::Value runtime_dependencies(Json::arrayValue);
    if (runtime.isMember("dependencies") && runtime["dependencies"].isArray()) {
        runtime_dependencies = runtime["dependencies"];
    }
    normalized["runtime_dependencies"] = runtime_dependencies;

    Json::Value services(Json::arrayValue);
    if (runtime.isMember("interface")) {
        services.append(runtime["interface"]);
    }
    normalized["services"] = services;
    normalized["tags"] = capability.get("tags", Json::Value(Json::arrayValue));
    normalized["pack_description"] = capability.get("description", "");
    normalized["pack_monetization"] = monetization;
    normalized["pack_server_details"] = pack_details;
    return normalized;
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

bool PackManager::isPackEnabledState(const std::string& state) const {
    return state == "Enabled" || state == "Loaded" || state == "Unloaded";
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
    if (required.isMember("min_cpu_cores")) {
        if (actual.get("cpu_cores", 0).asInt() < required["min_cpu_cores"].asInt()) {
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
    logPackManager("Publishing event phase=" + phase +
                   " status=" + status +
                   " pack_id=" + pack_id +
                   " user_id=" + user_id +
                   (!extra.isNull() ? " details=" + toJsonString(extra) : ""));
    if (sink_) {
        sink_->publish(event);
    }
}

}  // namespace edgeai
