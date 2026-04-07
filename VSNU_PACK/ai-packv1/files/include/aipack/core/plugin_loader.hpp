// =============================================================================
// AI Packs System - Dynamic Plugin Loader
// Handles loading/unloading shared libraries with ABI checking
// =============================================================================
#pragma once

#include "interfaces.hpp"
#include "logger.hpp"
#include <string>
#include <memory>
#include <unordered_map>
#include <mutex>

#ifdef _WIN32
    #include <windows.h>
    #define AIPACK_DLOPEN(path) LoadLibraryA(path)
    #define AIPACK_DLSYM(handle, sym) GetProcAddress(handle, sym)
    #define AIPACK_DLCLOSE(handle) FreeLibrary(handle)
    #define AIPACK_DLERROR() "DLL load error"
    using DlHandle = HMODULE;
#else
    #include <dlfcn.h>
    #define AIPACK_DLOPEN(path) dlopen(path, RTLD_NOW | RTLD_LOCAL)
    #define AIPACK_DLSYM(handle, sym) dlsym(handle, sym)
    #define AIPACK_DLCLOSE(handle) dlclose(handle)
    #define AIPACK_DLERROR() dlerror()
    using DlHandle = void*;
#endif

namespace aipack {

static const char* TAG_LOADER = "PluginLoader";

// =============================================================================
// Loaded Plugin Handle
// =============================================================================
struct LoadedPlugin {
    std::string packId;
    std::string libraryPath;
    DlHandle handle = nullptr;
    IPlugin* plugin = nullptr;
    PluginFactoryFunc createFunc = nullptr;
    PluginDestroyFunc destroyFunc = nullptr;
    AbiVersionFunc abiFunc = nullptr;
    uint32_t abiVersion = 0;
    PackState state = PackState::Unknown;

    ~LoadedPlugin() {
        unload();
    }

    void unload() {
        if (plugin && destroyFunc) {
            destroyFunc(plugin);
            plugin = nullptr;
        }
        if (handle) {
            AIPACK_DLCLOSE(handle);
            handle = nullptr;
        }
        state = PackState::Uninstalled;
    }
};

// =============================================================================
// Plugin Loader
// =============================================================================
class PluginLoader {
public:
    static PluginLoader& instance() {
        static PluginLoader loader;
        return loader;
    }

    /// Load a plugin from a shared library
    Result<LoadedPlugin*> load(const std::string& packId,
                                const std::string& libraryPath,
                                const std::string& entryPoint = "aipack_create_plugin") {
        std::lock_guard<std::mutex> lock(mutex_);

        // Check if already loaded
        auto it = plugins_.find(packId);
        if (it != plugins_.end()) {
            AIPACK_WARN(TAG_LOADER,
                "Plugin '%s' already loaded, returning existing instance",
                packId.c_str());
            return Result<LoadedPlugin*>::success(it->second.get());
        }

        auto loaded = std::make_unique<LoadedPlugin>();
        loaded->packId = packId;
        loaded->libraryPath = libraryPath;

        // Open shared library
        AIPACK_INFO(TAG_LOADER, "Loading plugin library: %s", libraryPath.c_str());
        loaded->handle = AIPACK_DLOPEN(libraryPath.c_str());
        if (!loaded->handle) {
            std::string err = AIPACK_DLERROR();
            AIPACK_ERROR(TAG_LOADER, "Failed to load library: %s", err.c_str());
            return Result<LoadedPlugin*>::failure(
                ErrorCode::PluginLoadFailed,
                "Failed to load library: " + err,
                libraryPath);
        }

        // Check ABI version
        loaded->abiFunc = reinterpret_cast<AbiVersionFunc>(
            AIPACK_DLSYM(loaded->handle, "aipack_abi_version"));
        if (loaded->abiFunc) {
            loaded->abiVersion = loaded->abiFunc();
            if (loaded->abiVersion != AIPACK_ABI_VERSION) {
                AIPACK_ERROR(TAG_LOADER,
                    "ABI version mismatch: plugin=%u, system=%u",
                    loaded->abiVersion, AIPACK_ABI_VERSION);
                return Result<LoadedPlugin*>::failure(
                    ErrorCode::PluginInterfaceMismatch,
                    "ABI version mismatch",
                    "plugin=" + std::to_string(loaded->abiVersion) +
                    " system=" + std::to_string(AIPACK_ABI_VERSION));
            }
        } else {
            AIPACK_WARN(TAG_LOADER,
                "Plugin '%s' does not export ABI version check",
                packId.c_str());
        }

        // Get factory function
        loaded->createFunc = reinterpret_cast<PluginFactoryFunc>(
            AIPACK_DLSYM(loaded->handle, entryPoint.c_str()));
        if (!loaded->createFunc) {
            std::string err = AIPACK_DLERROR();
            AIPACK_ERROR(TAG_LOADER,
                "Symbol '%s' not found: %s", entryPoint.c_str(), err.c_str());
            return Result<LoadedPlugin*>::failure(
                ErrorCode::PluginSymbolNotFound,
                "Entry point not found: " + entryPoint,
                err);
        }

        // Get destroy function
        loaded->destroyFunc = reinterpret_cast<PluginDestroyFunc>(
            AIPACK_DLSYM(loaded->handle, "aipack_destroy_plugin"));

        // Create plugin instance
        loaded->plugin = loaded->createFunc();
        if (!loaded->plugin) {
            AIPACK_ERROR(TAG_LOADER,
                "Factory function returned null for '%s'", packId.c_str());
            return Result<LoadedPlugin*>::failure(
                ErrorCode::PluginInitFailed,
                "Plugin factory returned null");
        }

        loaded->state = PackState::Loaded;
        AIPACK_INFO(TAG_LOADER,
            "Plugin '%s' loaded successfully (ABI v%u)",
            packId.c_str(), loaded->abiVersion);

        auto* ptr = loaded.get();
        plugins_[packId] = std::move(loaded);
        return Result<LoadedPlugin*>::success(ptr);
    }

    /// Unload a plugin
    Error unload(const std::string& packId) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = plugins_.find(packId);
        if (it == plugins_.end()) {
            return Error::make(ErrorCode::PackNotFound,
                "Plugin not loaded: " + packId);
        }

        AIPACK_INFO(TAG_LOADER, "Unloading plugin: %s", packId.c_str());
        plugins_.erase(it);
        return Error::success();
    }

    /// Reload a plugin (hot-reload)
    Result<LoadedPlugin*> reload(const std::string& packId) {
        std::string libPath;
        std::string entryPoint;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = plugins_.find(packId);
            if (it == plugins_.end()) {
                return Result<LoadedPlugin*>::failure(
                    ErrorCode::PackNotFound,
                    "Plugin not loaded: " + packId);
            }
            libPath = it->second->libraryPath;
            plugins_.erase(it);
        }
        AIPACK_INFO(TAG_LOADER, "Hot-reloading plugin: %s", packId.c_str());
        return load(packId, libPath);
    }

    /// Get a loaded plugin
    LoadedPlugin* get(const std::string& packId) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = plugins_.find(packId);
        return it != plugins_.end() ? it->second.get() : nullptr;
    }

    /// Get plugin instance cast to specific type
    template<typename T>
    T* getAs(const std::string& packId) {
        auto* loaded = get(packId);
        if (!loaded || !loaded->plugin) return nullptr;
        return dynamic_cast<T*>(loaded->plugin);
    }

    /// List all loaded plugins
    std::vector<std::string> listLoaded() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> ids;
        for (auto& [id, _] : plugins_) {
            ids.push_back(id);
        }
        return ids;
    }

    /// Unload all plugins
    void unloadAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        AIPACK_INFO(TAG_LOADER, "Unloading all plugins");
        plugins_.clear();
    }

private:
    PluginLoader() = default;
    std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<LoadedPlugin>> plugins_;
};

} // namespace aipack
