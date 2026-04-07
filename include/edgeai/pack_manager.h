#pragma once

#include <edgeai/http_client.h>
#include <edgeai/pack_manifest.h>
#include <edgeai/pack_runtime.h>

#include <json/json.h>

#include <filesystem>
#include <map>
#include <string>

namespace edgeai {

class PackEventSink {
  public:
    virtual ~PackEventSink() = default;
    virtual void publish(const Json::Value& event) = 0;
};

class PackManager {
  public:
    PackManager(std::filesystem::path state_dir,
                std::filesystem::path staging_dir,
                std::filesystem::path install_dir,
                std::string catalog_url,
                Json::Value default_device_capability);

    void setEventSink(PackEventSink* sink) { sink_ = sink; }

    Json::Value initialize();
    Json::Value queryPacks(const std::string& intent, const Json::Value& device_capability);
    Json::Value installPack(const std::string& user_id, const std::string& pack_id, bool approve_dependencies);
    Json::Value enablePack(const std::string& user_id, const std::string& pack_id);
    Json::Value loadPack(const std::string& user_id, const std::string& pack_id);
    Json::Value invoke(const std::string& user_id, const std::string& pack_id, const std::string& prompt, const std::string& options_json);
    Json::Value unloadPack(const std::string& user_id, const std::string& pack_id);
    Json::Value disablePack(const std::string& user_id, const std::string& pack_id);
    Json::Value uninstallPack(const std::string& user_id, const std::string& pack_id, bool force_shared_users);
    Json::Value rollbackPack(const std::string& user_id, const std::string& pack_id);

  private:
    Json::Value loadRegistry() const;
    void storeRegistry(const Json::Value& registry) const;
    Json::Value loadRollbackRegistry() const;
    void storeRollbackRegistry(const Json::Value& registry) const;
    Json::Value fetchPackMetadata(const std::string& pack_id) const;
    PackManifest loadInstalledManifest(const Json::Value& pack_entry) const;
    std::filesystem::path manifestPathFor(const Json::Value& pack_entry) const;
    std::filesystem::path packRootFor(const Json::Value& pack_entry) const;
    Json::Value deviceCapabilityFor(const Json::Value& override_capability) const;
    bool capabilityMatches(const Json::Value& required, const Json::Value& actual) const;
    void publishEvent(const std::string& phase,
                      const std::string& status,
                      const std::string& pack_id,
                      const std::string& user_id,
                      const Json::Value& extra = Json::Value()) const;

    std::filesystem::path state_dir_;
    std::filesystem::path staging_dir_;
    std::filesystem::path install_dir_;
    std::filesystem::path registry_path_;
    std::filesystem::path rollback_path_;
    std::string catalog_url_;
    Json::Value default_device_capability_;
    HttpClient http_client_;
    mutable std::map<std::string, PackRuntime> runtimes_;
    PackEventSink* sink_ = nullptr;
};

}  // namespace edgeai
