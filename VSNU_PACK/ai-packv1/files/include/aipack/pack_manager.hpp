// =============================================================================
// AI Packs System - Pack Manager
// Central orchestrator for pack lifecycle, discovery, and service resolution
//
// Lifecycle:    install → enable → load → use → unload → disable → uninstall
// Generic Use:  useService<T>()  / PackHandle / ServiceProxy<T>
// =============================================================================
#pragma once

#include "core/types.hpp"
#include "core/manifest.hpp"
#include "core/registry.hpp"
#include "core/plugin_loader.hpp"
#include "core/runtime.hpp"
#include "core/governance.hpp"
#include "core/workflow.hpp"
#include "core/event_bus.hpp"
#include "core/logger.hpp"
#include "core/json.hpp"

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <typeinfo>
#include <type_traits>

namespace aipack {

namespace fs = std::filesystem;

static const char* TAG_MANAGER = "PackManager";

// =============================================================================
// Pack Manager Configuration
// =============================================================================
struct PackManagerConfig {
    std::string storePath;          // Where packs are installed
    std::string registryPath;       // Path to registry database file
    bool enableHotReload = true;
    bool enableGovernance = true;
    bool enableWorkflows = true;
    bool autoDiscoverPacks = true;
    size_t maxMemoryBytes = 0;      // 0 = unlimited
    std::string defaultIdentity = "system";
};

// Forward declaration
class PackManager;

// =============================================================================
// PackHandle — RAII handle for a loaded pack with lifecycle awareness
// Holds a reference to a loaded service. Use for scoped service access with
// governance, health checking, and resource monitoring built in.
// =============================================================================
template<typename T>
class PackHandle {
public:
    PackHandle() = default;

    /// Check if the handle is valid and service is usable
    explicit operator bool() const { return service_ != nullptr && valid_; }

    /// Access the service
    T* operator->() { return service_; }
    const T* operator->() const { return service_; }
    T& operator*() { return *service_; }
    const T& operator*() const { return *service_; }

    /// Get the raw service pointer
    T* get() { return service_; }
    const T* get() const { return service_; }

    /// Get the pack ID this handle refers to
    const std::string& packId() const { return packId_; }

    /// Get the error if handle creation failed
    const Error& error() const { return error_; }

    /// Check if the pack is still healthy
    bool isHealthy() const {
        return service_ != nullptr && valid_ && plugin_ && plugin_->isHealthy();
    }

    /// Get resource usage of the underlying pack
    ResourceUsage resourceUsage() const {
        if (plugin_) return plugin_->getResourceUsage();
        return {};
    }

private:
    friend class PackManager;

    PackHandle(T* service, IPlugin* plugin,
               const std::string& packId, bool valid)
        : service_(service), plugin_(plugin), packId_(packId), valid_(valid) {}

    static PackHandle invalid(const Error& err) {
        PackHandle h;
        h.error_ = err;
        h.valid_ = false;
        return h;
    }

    T* service_ = nullptr;
    IPlugin* plugin_ = nullptr;
    std::string packId_;
    bool valid_ = false;
    Error error_;
};

// =============================================================================
// ServiceProxy — Transparent lazy-loading proxy for a service interface
// Auto-discovers, loads, and governance-checks on first use. Caches result.
//
// Usage:
//   auto tts = mgr.createProxy<ITTSService>("aipack.tts.basic");
//   tts->synthesize("Hello");  // auto-loads on first call
//
// =============================================================================
template<typename T>
class ServiceProxy {
public:
    ServiceProxy() = default;
    explicit ServiceProxy(PackManager* mgr,
                          const std::string& packId = "",
                          const std::string& identity = "system")
        : manager_(mgr), packId_(packId), identity_(identity) {}

    /// Resolve the service (lazy, cached). Returns nullptr on failure.
    T* resolve();

    /// Force re-resolution on next access
    void invalidate() { resolved_ = nullptr; resolvedOnce_ = false; }

    /// Check if resolved successfully
    bool available() { return resolve() != nullptr; }

    /// Arrow operator for transparent use: proxy->synthesize(...)
    T* operator->() { return resolve(); }

    /// Get last resolution error
    const Error& lastError() const { return lastError_; }

    /// Get the pack ID the proxy resolved to
    const std::string& resolvedPackId() const { return resolvedPackId_; }

private:
    PackManager* manager_ = nullptr;
    std::string packId_;         // Empty = auto-discover
    std::string identity_;
    T* resolved_ = nullptr;
    bool resolvedOnce_ = false;
    std::string resolvedPackId_;
    Error lastError_;
};

// =============================================================================
// Install Options — Configuration for pack installation
// =============================================================================
struct InstallOptions {
    bool force = false;             // Overwrite if already installed
    bool skipDependencyCheck = false;
    bool skipPlatformCheck = false;
    bool skipGovernanceCheck = false;
    bool autoEnable = true;         // Enable pack after install
    bool autoLoad = false;          // Load pack immediately after install
    std::string identity;           // Who is performing the install
};

// =============================================================================
// Uninstall Options
// =============================================================================
struct UninstallOptions {
    bool force = false;             // Uninstall even if other packs depend on it
    bool keepFiles = false;         // Keep pack files on disk
    bool skipGovernanceCheck = false;
    std::string identity;
};

// =============================================================================
// Pack Manager
// =============================================================================
class PackManager {
public:
    explicit PackManager(const std::string& storePath)
        : PackManager(PackManagerConfig{storePath,
                                         storePath + "/registry.json"}) {}

    explicit PackManager(const PackManagerConfig& config)
        : config_(config) {
        // Ensure store directory exists
        fs::create_directories(config_.storePath);

        // Configure resource limits
        if (config_.maxMemoryBytes > 0) {
            runtime_.resourceManager().setMaxMemory(config_.maxMemoryBytes);
        }

        // Setup default governance
        if (config_.enableGovernance) {
            policyEngine_.accessControl().grantRole(
                config_.defaultIdentity, "admin");
            policyEngine_.accessControl().grantRole(
                "aipack_cli", "aipack_cli");    

        }

        // Load existing registry
        registry_.loadFromFile(config_.registryPath);

        // Auto-discover packs in store
        if (config_.autoDiscoverPacks) {
            discoverPacks();
        }

        // Setup workflow service invoker
        if (config_.enableWorkflows) {
            workflowEngine_.setServiceInvoker(
                [this](const std::string& target,
                       const Properties& config,
                       WorkflowContext& ctx) -> Error {
                    return invokeService(target, config, ctx);
                });
        }

        AIPACK_INFO(TAG_MANAGER, "PackManager initialized (store: %s)",
            config_.storePath.c_str());
    }

    ~PackManager() {
        shutdown();
    }

    // =========================================================================
    // Pack Installation
    // =========================================================================

    /// Install a pack from a source directory into the pack store.
    ///
    /// Full flow:
    ///   1. Load & validate manifest (pack.json)
    ///   2. Governance check (does caller have install permission?)
    ///   3. Duplicate check (already installed? force overwrite?)
    ///   4. Platform compatibility check (arch, memory, GPU, etc.)
    ///   5. Dependency resolution (are all required packs present?)
    ///   6. Copy pack files to store directory
    ///   7. Register in registry
    ///   8. Apply governance policies from manifest
    ///   9. Register workflows from manifest
    ///  10. Emit PackInstalled event
    ///  11. Optionally auto-enable and auto-load
    ///
    Error install(const std::string& packPath,
                  const InstallOptions& opts = {}) {
        std::string who = opts.identity.empty()
                              ? config_.defaultIdentity : opts.identity;

        AIPACK_INFO(TAG_MANAGER, "Installing pack from: %s (by: %s)",
            packPath.c_str(), who.c_str());

        // --- Step 1: Load and validate manifest ---
        std::string manifestPath = packPath + "/pack.json";
        if (!fs::exists(manifestPath)) {
            return Error::make(ErrorCode::PackInvalidManifest,
                "No pack.json found in: " + packPath);
        }

        auto manifestResult = PackManifest::loadFromFile(manifestPath);
        if (!manifestResult) {
            return manifestResult.error;
        }
        auto& manifest = *manifestResult;

        if (manifest.id.empty()) {
            return Error::make(ErrorCode::PackInvalidManifest,
                "Manifest missing required 'id' field");
        }

        // --- Step 2: Governance check ---
        if (config_.enableGovernance && !opts.skipGovernanceCheck) {
            auto authErr = policyEngine_.authorize(who, manifest.id, "install");
            if (authErr) {
                AIPACK_WARN(TAG_MANAGER,
                    "Install denied by governance for '%s' by '%s': %s",
                    manifest.id.c_str(), who.c_str(),
                    authErr.message.c_str());
                return authErr;
            }
        }

        // --- Step 3: Duplicate check ---
        if (registry_.isInstalled(manifest.id)) {
            if (!opts.force) {
                return Error::make(ErrorCode::PackAlreadyInstalled,
                    "Pack already installed: " + manifest.id +
                    " (use force=true to overwrite)");
            }
            // Force reinstall: unload and unregister first
            AIPACK_INFO(TAG_MANAGER,
                "Force reinstall: removing existing '%s'",
                manifest.id.c_str());
            unloadPackInternal(manifest.id);
            registry_.unregisterPack(manifest.id);
        }

        // --- Step 4: Platform compatibility ---
        if (!opts.skipPlatformCheck) {
            auto platErr = checkPlatformCompatibility(manifest);
            if (platErr) return platErr;
        }

        // --- Step 5: Dependency resolution ---
        if (!opts.skipDependencyCheck) {
            auto depRes = registry_.resolveDependencies(manifest);
            if (!depRes.success) {
                std::string msg = "Dependency resolution failed for "
                                  + manifest.id;
                for (auto& m : depRes.missing)
                    msg += "\n  Missing: " + m;
                for (auto& c : depRes.conflicts)
                    msg += "\n  Conflict: " + c;
                return Error::make(ErrorCode::PackDependencyMissing, msg);
            }
        }

        // --- Step 6: Copy pack files to store ---
        std::string installPath = config_.storePath + "/" + manifest.id;
        if (packPath != installPath) {
            try {
                fs::create_directories(installPath);
                fs::copy(packPath, installPath,
                    fs::copy_options::recursive |
                    fs::copy_options::overwrite_existing);
                AIPACK_DEBUG(TAG_MANAGER,
                    "Copied pack files to: %s", installPath.c_str());
            } catch (const std::exception& e) {
                // Rollback: remove partial copy
                try { fs::remove_all(installPath); }
                catch (...) {}
                return Error::make(ErrorCode::IOError,
                    std::string("Failed to copy pack: ") + e.what());
            }
        }

        // --- Step 7: Register ---
        auto regErr = registry_.registerPack(manifest, installPath);
        if (regErr) {
            // Rollback file copy
            if (packPath != installPath) {
                try { fs::remove_all(installPath); }
                catch (...) {}
            }
            return regErr;
        }

        // --- Step 8: Apply governance policies ---
        if (config_.enableGovernance) {
            policyEngine_.applyPolicies(manifest);
        }

        // --- Step 9: Register workflows ---
        if (config_.enableWorkflows) {
            for (auto& wf : manifest.workflows) {
                workflowEngine_.registerWorkflow(wf);
            }
        }

        // --- Step 10: Emit event & persist ---
        registry_.saveToFile(config_.registryPath);
        {
            // Build capabilities list: join all service interface names so
            // subscriber packs can filter on e.g. data["capabilities"]
            // containing "ISTTService" without knowing the pack ID.
            std::string caps;
            for (auto& svc : manifest.services) {
                if (!caps.empty()) caps += ',';
                caps += svc.interface;
            }
            EventBus::instance().publish(EventType::PackInstalled,
                manifest.id, "Pack installed: " + manifest.name,
                {{"pack_id",      manifest.id},
                 {"version",      manifest.version.toString()},
                 {"type",         packTypeToString(manifest.type)},
                 {"capabilities", caps},
                 {"by",           who}});
        }

        AIPACK_INFO(TAG_MANAGER, "Pack '%s' v%s installed successfully",
            manifest.id.c_str(), manifest.version.toString().c_str());

        // --- Step 11: Auto-enable / auto-load ---
        if (opts.autoEnable) {
            registry_.setEnabled(manifest.id, true);
        }
        if (opts.autoLoad) {
            auto loadErr = loadPack(manifest.id);
            if (loadErr) {
                AIPACK_WARN(TAG_MANAGER,
                    "Auto-load failed for '%s': %s",
                    manifest.id.c_str(), loadErr.message.c_str());
            }
        }

        return Error::success();
    }

    // =========================================================================
    // Pack Uninstallation
    // =========================================================================

    /// Uninstall a pack completely.
    ///
    /// Full flow:
    ///   1. Verify pack exists
    ///   2. Governance check
    ///   3. Check for dependent packs (fail if others depend on this)
    ///   4. Gracefully shutdown & unload plugin
    ///   5. Unregister from registry
    ///   6. Remove files from disk (unless keepFiles)
    ///   7. Emit PackRemoved event
    ///
    Error uninstall(const std::string& packId,
                    const UninstallOptions& opts = {}) {
        std::string who = opts.identity.empty()
                              ? config_.defaultIdentity : opts.identity;

        AIPACK_INFO(TAG_MANAGER, "Uninstalling pack: %s (by: %s)",
            packId.c_str(), who.c_str());

        // --- Step 1: Verify pack exists ---
        auto* entry = registry_.getPack(packId);
        if (!entry) {
            return Error::make(ErrorCode::PackNotFound,
                "Pack not found: " + packId);
        }
        std::string installPath = entry->installPath;

        // --- Step 2: Governance check ---
        if (config_.enableGovernance && !opts.skipGovernanceCheck) {
            auto authErr = policyEngine_.authorize(who, packId, "uninstall");
            if (authErr) return authErr;
        }

        // --- Step 3: Check for dependent packs ---
        if (!opts.force) {
            auto dependents = registry_.findDependents(packId);
            if (!dependents.empty()) {
                std::string msg = "Cannot uninstall '" + packId
                                  + "': required by";
                for (auto* dep : dependents) {
                    msg += " " + dep->manifest.id;
                }
                msg += " (use force=true to override)";
                return Error::make(ErrorCode::PackDependencyMissing, msg);
            }
        }

        // --- Step 4: Graceful shutdown & unload ---
        // Capture capabilities BEFORE unregistering (manifest is gone after step 5)
        std::string removeCaps;
        std::string removeVersion;
        std::string removeType;
        {
            auto* e2 = registry_.getPack(packId);
            if (e2) {
                for (auto& svc : e2->manifest.services) {
                    if (!removeCaps.empty()) removeCaps += ',';
                    removeCaps += svc.interface;
                }
                removeVersion = e2->manifest.version.toString();
                removeType    = packTypeToString(e2->manifest.type);
            }
        }
        unloadPackInternal(packId);

        // --- Step 5: Unregister ---
        auto err = registry_.unregisterPack(packId);
        if (err) return err;

        // --- Step 6: Remove files ---
        if (!opts.keepFiles && !installPath.empty()
            && fs::exists(installPath)) {
            try {
                fs::remove_all(installPath);
                AIPACK_INFO(TAG_MANAGER,
                    "Removed pack files: %s", installPath.c_str());
            } catch (const std::exception& e) {
                AIPACK_WARN(TAG_MANAGER,
                    "Failed to remove pack files: %s", e.what());
            }
        }

        // --- Step 7: Emit event & persist ---
        registry_.saveToFile(config_.registryPath);
        EventBus::instance().publish(EventType::PackRemoved,
            packId, "Pack uninstalled",
            {{"pack_id",      packId},
             {"version",      removeVersion},
             {"type",         removeType},
             {"capabilities", removeCaps},
             {"by",           who}});

        AIPACK_INFO(TAG_MANAGER, "Pack '%s' uninstalled", packId.c_str());
        return Error::success();
    }

    // =========================================================================
    // Pack Enable / Disable
    // =========================================================================

    /// Enable a pack — makes it available for loading and service resolution.
    ///
    /// Flow:
    ///   1. Verify pack is installed
    ///   2. Governance check
    ///   3. Mark as enabled in registry
    ///   4. Emit PackEnabled event
    ///
    Error enablePack(const std::string& packId,
                     const std::string& identity = "") {
        std::string who = identity.empty()
                              ? config_.defaultIdentity : identity;

        AIPACK_INFO(TAG_MANAGER, "Enabling pack: %s", packId.c_str());

        auto* entry = registry_.getPack(packId);
        if (!entry) {
            return Error::make(ErrorCode::PackNotFound,
                "Pack not found: " + packId);
        }

        if (entry->enabled) {
            AIPACK_DEBUG(TAG_MANAGER, "Pack '%s' is already enabled",
                packId.c_str());
            return Error::success();
        }

        // Governance check
        if (config_.enableGovernance) {
            auto authErr = policyEngine_.authorize(who, packId, "enable");
            if (authErr) return authErr;
        }

        // Enable
        registry_.setEnabled(packId, true);
        registry_.updateState(packId, PackState::Installed);

        // Persist & event
        registry_.saveToFile(config_.registryPath);
        EventBus::instance().publish(EventType::PackEnabled,
            packId, "Pack enabled",
            {{"by", who}});

        AIPACK_INFO(TAG_MANAGER, "Pack '%s' enabled", packId.c_str());
        return Error::success();
    }

    /// Disable a pack — stops all services and prevents loading/usage,
    /// but keeps the pack installed on disk.
    ///
    /// Flow:
    ///   1. Verify pack is installed
    ///   2. Governance check
    ///   3. Gracefully shutdown & unload plugin if loaded
    ///   4. Mark as disabled in registry
    ///   5. Emit PackDisabled event
    ///
    Error disablePack(const std::string& packId,
                      const std::string& identity = "") {
        std::string who = identity.empty()
                              ? config_.defaultIdentity : identity;

        AIPACK_INFO(TAG_MANAGER, "Disabling pack: %s", packId.c_str());

        auto* entry = registry_.getPack(packId);
        if (!entry) {
            return Error::make(ErrorCode::PackNotFound,
                "Pack not found: " + packId);
        }

        if (!entry->enabled) {
            AIPACK_DEBUG(TAG_MANAGER, "Pack '%s' is already disabled",
                packId.c_str());
            return Error::success();
        }

        // Governance check
        if (config_.enableGovernance) {
            auto authErr = policyEngine_.authorize(who, packId, "disable");
            if (authErr) return authErr;
        }

        // Unload if loaded
        unloadPackInternal(packId);

        // Disable
        registry_.setEnabled(packId, false);
        registry_.updateState(packId, PackState::Disabled);

        // Persist & event
        registry_.saveToFile(config_.registryPath);
        EventBus::instance().publish(EventType::PackDisabled,
            packId, "Pack disabled",
            {{"by", who}});

        AIPACK_INFO(TAG_MANAGER, "Pack '%s' disabled", packId.c_str());
        return Error::success();
    }

    /// Check if a pack is currently enabled
    bool isPackEnabled(const std::string& packId) const {
        return registry_.isEnabled(packId);
    }

    /// Check if a pack is currently loaded and running
    bool isPackLoaded(const std::string& packId) const {
        return PluginLoader::instance().get(packId) != nullptr;
    }

    // =========================================================================
    // Pack Loading / Unloading
    // =========================================================================

    /// Load a pack's plugin — dynamically load the .so, create the plugin
    /// instance, initialize it, and transition to Running state.
    Error loadPack(const std::string& packId) {
        auto* entry = registry_.getPack(packId);
        if (!entry) {
            return Error::make(ErrorCode::PackNotFound,
                "Pack not found: " + packId);
        }

        // Must be enabled
        if (!entry->enabled) {
            return Error::make(ErrorCode::AccessDenied,
                "Pack '" + packId + "' is disabled, enable it first");
        }

        auto& manifest = entry->manifest;
        if (manifest.libraryPath.empty()) {
            AIPACK_WARN(TAG_MANAGER,
                "Pack '%s' has no library to load", packId.c_str());
            return Error::success();
        }

        std::string libPath = entry->installPath + "/" + manifest.libraryPath;

        // Load the shared library
        auto result = PluginLoader::instance().load(
            packId, libPath, manifest.entryPoint);
        if (!result) {
            registry_.updateState(packId, PackState::Error);
            return result.error;
        }

        // Initialize plugin with default config
        auto* plugin = (*result)->plugin;
        if (plugin) {
            auto initErr = plugin->initialize(manifest.defaultConfig);
            if (initErr) {
                AIPACK_ERROR(TAG_MANAGER,
                    "Plugin init failed for '%s': %s",
                    packId.c_str(), initErr.message.c_str());
                PluginLoader::instance().unload(packId);
                registry_.updateState(packId, PackState::Error);
                return initErr;
            }
        }

        registry_.updateState(packId, PackState::Running);

        // Load models if configured for eager loading
        if (!manifest.lazyLoad) {
            for (auto& model : manifest.models) {
                runtime_.loadModel(packId, model, entry->installPath);
            }
        }

        EventBus::instance().publish(EventType::PackLoaded,
            packId, "Pack loaded: " + manifest.name,
            {{"pack_id",      packId},
             {"version",      manifest.version.toString()},
             {"type",         packTypeToString(manifest.type)},
             {"capabilities", [&]{
                 std::string caps;
                 for (auto& svc : manifest.services) {
                     if (!caps.empty()) caps += ',';
                     caps += svc.interface;
                 }
                 return caps;
             }()}});
        return Error::success();
    }

    /// Unload a pack's plugin — shutdown and release resources but keep
    /// the pack installed and enabled.
    Error unloadPack(const std::string& packId) {
        // Capture capabilities before unloading (manifest stays in registry)
        std::string unloadCaps;
        std::string unloadVersion;
        std::string unloadType;
        if (auto* entry = registry_.getPack(packId)) {
            for (auto& svc : entry->manifest.services) {
                if (!unloadCaps.empty()) unloadCaps += ',';
                unloadCaps += svc.interface;
            }
            unloadVersion = entry->manifest.version.toString();
            unloadType    = packTypeToString(entry->manifest.type);
        }
        unloadPackInternal(packId);
        registry_.updateState(packId, PackState::Installed);
        EventBus::instance().publish(EventType::PackUnloaded,
            packId, "Pack unloaded",
            {{"pack_id",      packId},
             {"version",      unloadVersion},
             {"type",         unloadType},
             {"capabilities", unloadCaps}});
        return Error::success();
    }

    /// Hot-reload a pack's plugin (if supported)
    Error reloadPack(const std::string& packId) {
        if (!config_.enableHotReload) {
            return Error::make(ErrorCode::NotImplemented,
                "Hot reload is disabled");
        }

        auto* entry = registry_.getPack(packId);
        if (!entry) {
            return Error::make(ErrorCode::PackNotFound,
                "Pack not found: " + packId);
        }
        if (!entry->manifest.supportsHotReload) {
            return Error::make(ErrorCode::NotImplemented,
                "Pack does not support hot reload: " + packId);
        }

        AIPACK_INFO(TAG_MANAGER, "Hot-reloading pack: %s", packId.c_str());

        auto result = PluginLoader::instance().reload(packId);
        if (!result) return result.error;

        auto* plugin = (*result)->plugin;
        if (plugin) {
            plugin->initialize(entry->manifest.defaultConfig);
        }

        registry_.updateState(packId, PackState::Running);
        EventBus::instance().publish(EventType::PackUpdated,
            packId, "Pack hot-reloaded");
        return Error::success();
    }

    // =========================================================================
    // Service Resolution — Get typed service from packs
    // =========================================================================

    /// Get a service of type T from a specific pack (loads on demand).
    /// Returns nullptr if pack is disabled, not found, or interface mismatch.
    template<typename T>
    T* getService(const std::string& packId) {
        auto* entry = registry_.getPack(packId);
        if (!entry) return nullptr;

        // Disabled packs cannot provide services
        if (!entry->enabled) {
            AIPACK_WARN(TAG_MANAGER,
                "Cannot get service from disabled pack '%s'",
                packId.c_str());
            return nullptr;
        }

        // Auto-load if not loaded
        auto* loaded = PluginLoader::instance().get(packId);
        if (!loaded) {
            auto err = loadPack(packId);
            if (err) {
                AIPACK_ERROR(TAG_MANAGER,
                    "Failed to load pack '%s': %s",
                    packId.c_str(), err.message.c_str());
                return nullptr;
            }
            loaded = PluginLoader::instance().get(packId);
        }

        if (!loaded || !loaded->plugin) return nullptr;

        return dynamic_cast<T*>(loaded->plugin);
    }

    /// Find the first available service of type T from any enabled pack.
    template<typename T>
    T* findService() {
        auto packs = registry_.listEnabled();
        for (auto* entry : packs) {
            auto* svc = getService<T>(entry->manifest.id);
            if (svc) return svc;
        }
        return nullptr;
    }

    /// Find all services of type T from all enabled packs.
    template<typename T>
    std::vector<std::pair<std::string, T*>> findAllServices() {
        std::vector<std::pair<std::string, T*>> results;
        auto packs = registry_.listEnabled();
        for (auto* entry : packs) {
            auto* svc = getService<T>(entry->manifest.id);
            if (svc) {
                results.emplace_back(entry->manifest.id, svc);
            }
        }
        return results;
    }

    // =========================================================================
    // Generic Pack Usage — PackHandle and ServiceProxy
    // =========================================================================

    /// Get a lifecycle-aware handle to a pack service.
    /// Checks governance, auto-loads, and provides health/resource monitoring.
    ///
    /// Usage:
    ///   auto tts = mgr.useService<ITTSService>("aipack.tts.basic");
    ///   if (tts) {
    ///       auto result = tts->synthesize("Hello");
    ///       std::cout << "Healthy: " << tts.isHealthy() << "\n";
    ///   } else {
    ///       std::cout << "Error: " << tts.error().message << "\n";
    ///   }
    ///
    template<typename T>
    PackHandle<T> useService(const std::string& packId,
                              const std::string& identity = "") {
        std::string who = identity.empty()
                              ? config_.defaultIdentity : identity;

        // Check pack exists
        auto* entry = registry_.getPack(packId);
        if (!entry) {
            return PackHandle<T>::invalid(
                Error::make(ErrorCode::PackNotFound,
                    "Pack not found: " + packId));
        }

        // Check enabled
        if (!entry->enabled) {
            return PackHandle<T>::invalid(
                Error::make(ErrorCode::AccessDenied,
                    "Pack '" + packId + "' is disabled"));
        }

        // Governance check
        if (config_.enableGovernance) {
            auto authErr = policyEngine_.authorize(who, packId, "use");
            if (authErr) {
                return PackHandle<T>::invalid(authErr);
            }
        }

        // Get service (auto-loads if needed)
        T* svc = getService<T>(packId);
        if (!svc) {
            return PackHandle<T>::invalid(
                Error::make(ErrorCode::PluginInterfaceMismatch,
                    "Pack '" + packId +
                    "' does not provide requested interface"));
        }

        // Get plugin for health monitoring
        auto* loaded = PluginLoader::instance().get(packId);
        IPlugin* plugin = loaded ? loaded->plugin : nullptr;

        return PackHandle<T>(svc, plugin, packId, true);
    }

    /// Auto-discover a service by type — finds the best enabled pack
    /// providing interface T and returns a handle.
    ///
    /// Usage:
    ///   auto tts = mgr.discoverService<ITTSService>();
    ///   if (tts) { tts->synthesize("Hello"); }
    ///
    template<typename T>
    PackHandle<T> discoverService(const std::string& identity = "") {
        std::string who = identity.empty()
                              ? config_.defaultIdentity : identity;

        auto packs = registry_.listEnabled();
        for (auto* entry : packs) {
            auto handle = useService<T>(entry->manifest.id, who);
            if (handle) return handle;
        }

        return PackHandle<T>::invalid(
            Error::make(ErrorCode::PackNotFound,
                "No enabled pack provides the requested interface"));
    }

    /// Create a ServiceProxy for lazy, transparent access to a service.
    /// The proxy resolves the service on first use and caches the result.
    ///
    /// Usage:
    ///   auto ttsProxy = mgr.createProxy<ITTSService>("aipack.tts.basic");
    ///   // ... later, auto-resolves on first call ...
    ///   ttsProxy->synthesize("Hello");
    ///
    template<typename T>
    ServiceProxy<T> createProxy(const std::string& packId = "",
                                 const std::string& identity = "") {
        return ServiceProxy<T>(this, packId,
            identity.empty() ? config_.defaultIdentity : identity);
    }

    /// Get the raw plugin for a pack
    IPlugin* getPlugin(const std::string& packId) {
        auto* loaded = PluginLoader::instance().get(packId);
        if (!loaded) {
            loadPack(packId);
            loaded = PluginLoader::instance().get(packId);
        }
        return loaded ? loaded->plugin : nullptr;
    }

    // =========================================================================
    // Discovery & Query
    // =========================================================================

    /// Discover packs in the store directory
    void discoverPacks() {
        AIPACK_INFO(TAG_MANAGER, "Discovering packs in: %s",
            config_.storePath.c_str());

        if (!fs::exists(config_.storePath)) return;

        for (auto& entry : fs::directory_iterator(config_.storePath)) {
            if (!entry.is_directory()) continue;

            std::string manifestPath = entry.path().string() + "/pack.json";
            if (!fs::exists(manifestPath)) continue;

            auto result = PackManifest::loadFromFile(manifestPath);
            if (!result) {
                AIPACK_WARN(TAG_MANAGER,
                    "Invalid manifest in %s: %s",
                    entry.path().string().c_str(),
                    result.error.message.c_str());
                continue;
            }

            if (!registry_.isInstalled(result->id)) {
                registry_.registerPack(*result, entry.path().string());
                EventBus::instance().publish(EventType::PackDiscovered,
                    result->id, "Discovered: " + result->name);
            }
        }
    }

    /// List all installed packs
    std::vector<const PackEntry*> listPacks() const {
        return registry_.listAll();
    }

    /// List only enabled packs
    std::vector<const PackEntry*> listEnabledPacks() const {
        return registry_.listEnabled();
    }

    /// Find packs by type
    std::vector<const PackEntry*> findByType(PackType type) const {
        return registry_.findByType(type);
    }

    /// Find packs by category
    std::vector<const PackEntry*> findByCategory(
            const std::string& category) const {
        return registry_.findByCategory(category);
    }

    /// Find packs providing an interface
    std::vector<const PackEntry*> findByInterface(
            const std::string& iface) const {
        return registry_.findByInterface(iface);
    }

    /// Get pack manifest
    const PackManifest* getManifest(const std::string& packId) const {
        return registry_.getManifest(packId);
    }

    // =========================================================================
    // Runtime & Inference
    // =========================================================================

    AIRuntime& runtime() { return runtime_; }
    const AIRuntime& runtime() const { return runtime_; }

    // =========================================================================
    // Governance
    // =========================================================================

    PolicyEngine& policyEngine() { return policyEngine_; }
    const PolicyEngine& policyEngine() const { return policyEngine_; }

    // =========================================================================
    // Workflow
    // =========================================================================

    WorkflowEngine& workflowEngine() { return workflowEngine_; }
    const WorkflowEngine& workflowEngine() const { return workflowEngine_; }

    /// Execute a workflow by name
    Result<Properties> executeWorkflow(const std::string& name,
                                        const Properties& inputs = {}) {
        if (!config_.enableWorkflows) {
            return Result<Properties>::failure(
                ErrorCode::NotImplemented, "Workflows are disabled");
        }
        return workflowEngine_.execute(name, inputs);
    }

    // =========================================================================
    // Registry
    // =========================================================================

    PackRegistry& registry() { return registry_; }
    const PackRegistry& registry() const { return registry_; }

    /// Get the pack store directory path
    const std::string& getStorePath() const { return config_.storePath; }

    // =========================================================================
    // Lifecycle
    // =========================================================================

    /// Shutdown all packs and release resources
    void shutdown() {
        AIPACK_INFO(TAG_MANAGER, "Shutting down PackManager...");

        EventBus::instance().publish(EventType::SystemShutdown,
            "PackManager", "System shutting down");

        // Unload all plugins
        PluginLoader::instance().unloadAll();

        // Save registry
        registry_.saveToFile(config_.registryPath);

        AIPACK_INFO(TAG_MANAGER, "PackManager shutdown complete");
    }

private:
    /// Internal unload — shutdown plugin and release, no state update or event
    void unloadPackInternal(const std::string& packId) {
        auto* loaded = PluginLoader::instance().get(packId);
        if (loaded) {
            if (loaded->plugin) {
                AIPACK_DEBUG(TAG_MANAGER,
                    "Shutting down plugin: %s", packId.c_str());
                loaded->plugin->shutdown();
            }
            PluginLoader::instance().unload(packId);
        }
    }

    Error checkPlatformCompatibility(const PackManifest& manifest) {
        auto& plat = manifest.platform;

        if (!plat.arch.empty() && plat.arch != "any") {
#if defined(__aarch64__)
            if (plat.arch != "arm64" && plat.arch != "aarch64") {
                return Error::make(ErrorCode::PackIncompatiblePlatform,
                    "Pack requires arch: " + plat.arch + " (running arm64)");
            }
#elif defined(__arm__)
            if (plat.arch != "armv7" && plat.arch != "arm") {
                return Error::make(ErrorCode::PackIncompatiblePlatform,
                    "Pack requires arch: " + plat.arch + " (running arm)");
            }
#elif defined(__x86_64__)
            if (plat.arch != "x86_64" && plat.arch != "amd64") {
                return Error::make(ErrorCode::PackIncompatiblePlatform,
                    "Pack requires arch: " + plat.arch + " (running x86_64)");
            }
#endif
        }

        return Error::success();
    }

    Error invokeService(const std::string& target,
                        const Properties& config,
                        WorkflowContext& ctx) {
        auto* plugin = getPlugin(target);
        if (!plugin) {
            return Error::make(ErrorCode::PackNotFound,
                "Service not found: " + target);
        }
        AIPACK_DEBUG(TAG_MANAGER,
            "Workflow invoking service: %s", target.c_str());
        return Error::success();
    }

    PackManagerConfig config_;
    PackRegistry registry_;
    AIRuntime runtime_;
    PolicyEngine policyEngine_;
    WorkflowEngine workflowEngine_;
};

// =============================================================================
// ServiceProxy implementation (needs PackManager to be fully defined)
// =============================================================================
template<typename T>
T* ServiceProxy<T>::resolve() {
    if (resolvedOnce_ && resolved_) return resolved_;
    if (!manager_) {
        lastError_ = Error::make(ErrorCode::RuntimeNotInitialized,
            "ServiceProxy has no PackManager");
        return nullptr;
    }

    if (!packId_.empty()) {
        // Resolve from specific pack
        auto handle = manager_->useService<T>(packId_, identity_);
        if (handle) {
            resolved_ = handle.get();
            resolvedPackId_ = packId_;
            resolvedOnce_ = true;
            return resolved_;
        }
        lastError_ = handle.error();
    } else {
        // Auto-discover
        auto handle = manager_->template discoverService<T>(identity_);
        if (handle) {
            resolved_ = handle.get();
            resolvedPackId_ = handle.packId();
            resolvedOnce_ = true;
            return resolved_;
        }
        lastError_ = handle.error();
    }
    return nullptr;
}

} // namespace aipack
