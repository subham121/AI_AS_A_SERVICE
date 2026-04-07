// =============================================================================
// AI Packs System - Pack Registry
// Stores and indexes installed packs for discovery and dependency resolution
// =============================================================================
#pragma once

#include "types.hpp"
#include "manifest.hpp"
#include "json.hpp"
#include "logger.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <mutex>
#include <fstream>
#include <sstream>

namespace aipack {

static const char* TAG_REGISTRY = "PackRegistry";

// =============================================================================
// Pack Entry in Registry
// =============================================================================
struct PackEntry {
    PackManifest manifest;
    std::string installPath;    // Absolute path to pack directory
    PackState state = PackState::Installed;
    bool enabled = true;        // Whether pack is enabled (can be loaded/used)
    std::chrono::system_clock::time_point installedAt;
    std::chrono::system_clock::time_point lastUsed;
    ResourceUsage lastResourceUsage;
};

// =============================================================================
// Pack Registry
// =============================================================================
class PackRegistry {
public:
    PackRegistry() = default;

    /// Register a pack
    Error registerPack(const PackManifest& manifest,
                       const std::string& installPath) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (packs_.find(manifest.id) != packs_.end()) {
            return Error::make(ErrorCode::PackAlreadyInstalled,
                "Pack already registered: " + manifest.id);
        }

        PackEntry entry;
        entry.manifest = manifest;
        entry.installPath = installPath;
        entry.state = PackState::Installed;
        entry.installedAt = std::chrono::system_clock::now();

        AIPACK_INFO(TAG_REGISTRY, "Registered pack: %s v%s",
            manifest.id.c_str(), manifest.version.toString().c_str());

        packs_[manifest.id] = std::move(entry);
        return Error::success();
    }

    /// Unregister a pack
    Error unregisterPack(const std::string& packId) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = packs_.find(packId);
        if (it == packs_.end()) {
            return Error::make(ErrorCode::PackNotFound,
                "Pack not found: " + packId);
        }
        packs_.erase(it);
        AIPACK_INFO(TAG_REGISTRY, "Unregistered pack: %s", packId.c_str());
        return Error::success();
    }

    /// Update pack state
    Error updateState(const std::string& packId, PackState state) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = packs_.find(packId);
        if (it == packs_.end()) {
            return Error::make(ErrorCode::PackNotFound, "Pack not found");
        }
        it->second.state = state;
        return Error::success();
    }

    /// Get pack entry
    const PackEntry* getPack(const std::string& packId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = packs_.find(packId);
        return it != packs_.end() ? &it->second : nullptr;
    }

    /// Get pack manifest
    const PackManifest* getManifest(const std::string& packId) const {
        auto* entry = getPack(packId);
        return entry ? &entry->manifest : nullptr;
    }

    /// Check if pack is installed
    bool isInstalled(const std::string& packId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return packs_.find(packId) != packs_.end();
    }

    /// Find packs by type
    std::vector<const PackEntry*> findByType(PackType type) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<const PackEntry*> result;
        for (auto& [id, entry] : packs_) {
            if (entry.manifest.type == type)
                result.push_back(&entry);
        }
        return result;
    }

    /// Find packs by category
    std::vector<const PackEntry*> findByCategory(
            const std::string& category) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<const PackEntry*> result;
        for (auto& [id, entry] : packs_) {
            if (entry.manifest.category == category)
                result.push_back(&entry);
        }
        return result;
    }

    /// Find packs by tag
    std::vector<const PackEntry*> findByTag(const std::string& tag) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<const PackEntry*> result;
        for (auto& [id, entry] : packs_) {
            auto& tags = entry.manifest.tags;
            if (std::find(tags.begin(), tags.end(), tag) != tags.end())
                result.push_back(&entry);
        }
        return result;
    }

    /// Find packs providing a specific service interface
    std::vector<const PackEntry*> findByInterface(
            const std::string& iface) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<const PackEntry*> result;
        for (auto& [id, entry] : packs_) {
            for (auto& svc : entry.manifest.services) {
                if (svc.interface == iface) {
                    result.push_back(&entry);
                    break;
                }
            }
        }
        return result;
    }

    /// Check if pack is enabled
    bool isEnabled(const std::string& packId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = packs_.find(packId);
        return it != packs_.end() && it->second.enabled;
    }

    /// Set enabled/disabled state
    Error setEnabled(const std::string& packId, bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = packs_.find(packId);
        if (it == packs_.end()) {
            return Error::make(ErrorCode::PackNotFound, "Pack not found: " + packId);
        }
        it->second.enabled = enabled;
        return Error::success();
    }

    /// List all installed packs
    std::vector<const PackEntry*> listAll() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<const PackEntry*> result;
        for (auto& [id, entry] : packs_) {
            result.push_back(&entry);
        }
        return result;
    }

    /// List enabled packs only
    std::vector<const PackEntry*> listEnabled() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<const PackEntry*> result;
        for (auto& [id, entry] : packs_) {
            if (entry.enabled) result.push_back(&entry);
        }
        return result;
    }

    /// Find all packs that depend on the given pack
    std::vector<const PackEntry*> findDependents(const std::string& packId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<const PackEntry*> result;
        for (auto& [id, entry] : packs_) {
            for (auto& dep : entry.manifest.dependencies) {
                if (dep.packId == packId && !dep.optional) {
                    result.push_back(&entry);
                    break;
                }
            }
        }
        return result;
    }

    /// Resolve dependencies for a pack
    struct DependencyResolution {
        std::vector<std::string> resolved;      // In install order
        std::vector<std::string> missing;       // Missing dependencies
        std::vector<std::string> conflicts;     // Version conflicts
        bool success = false;
    };

    DependencyResolution resolveDependencies(
            const PackManifest& manifest) const {
        std::lock_guard<std::mutex> lock(mutex_);
        DependencyResolution result;

        for (auto& dep : manifest.dependencies) {
            auto it = packs_.find(dep.packId);
            if (it == packs_.end()) {
                if (!dep.optional) {
                    result.missing.push_back(dep.packId);
                }
            } else {
                if (!dep.satisfiedBy(it->second.manifest.version)) {
                    result.conflicts.push_back(
                        dep.packId + " requires " + dep.versionRange +
                        " but has " +
                        it->second.manifest.version.toString());
                } else {
                    result.resolved.push_back(dep.packId);
                }
            }
        }

        // Check for conflicts with other installed packs
        for (auto& conflictId : manifest.conflicts) {
            if (packs_.find(conflictId) != packs_.end()) {
                result.conflicts.push_back(
                    "Conflicts with installed pack: " + conflictId);
            }
        }

        result.success = result.missing.empty() && result.conflicts.empty();
        return result;
    }

    /// Save registry to file
    Error saveToFile(const std::string& path) const {
        std::lock_guard<std::mutex> lock(mutex_);

        json::Array entries;
        for (auto& [id, entry] : packs_) {
            json::Object obj;
            obj["manifest"] = entry.manifest.toJson();
            obj["install_path"] = json::Value(entry.installPath);
            obj["state"] = json::Value(packStateToString(entry.state));
            obj["enabled"] = json::Value(entry.enabled);
            entries.push_back(json::Value(std::move(obj)));
        }

        json::Object root;
        root["version"] = json::Value("1.0");
        root["packs"] = json::Value(std::move(entries));

        std::ofstream file(path);
        if (!file.is_open()) {
            return Error::make(ErrorCode::IOError,
                "Cannot write registry: " + path);
        }
        file << json::Value(std::move(root)).dump(2);
        return Error::success();
    }

    /// Load registry from file
    Error loadFromFile(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);

        std::ifstream file(path);
        if (!file.is_open()) {
            AIPACK_WARN(TAG_REGISTRY,
                "Registry file not found: %s (starting fresh)", path.c_str());
            return Error::success();
        }

        std::ostringstream ss;
        ss << file.rdbuf();
        std::string content = ss.str();

        try {
            auto j = json::parse(content);
            if (j.has("packs") && j["packs"].isArray()) {
                for (auto& entry : j["packs"].asArray()) {
                    PackEntry pe;
                    pe.manifest = PackManifest::fromJson(entry["manifest"]);
                    pe.installPath = entry.getString("install_path");
                    pe.enabled = entry.getBool("enabled", true);
                    // Parse state string back
                    pe.state = PackState::Installed;
                    packs_[pe.manifest.id] = std::move(pe);
                }
            }
            AIPACK_INFO(TAG_REGISTRY,
                "Loaded registry with %zu packs", packs_.size());
        } catch (const std::exception& e) {
            return Error::make(ErrorCode::PackInvalidManifest,
                std::string("Failed to load registry: ") + e.what());
        }
        return Error::success();
    }

    size_t count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return packs_.size();
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, PackEntry> packs_;
};

} // namespace aipack
