#pragma once

#include <edgeai/http_client.h>
#include <edgeai/pack_manager.h>

#include <json/json.h>

#include <string>

namespace edgeai {

class IntentManager {
  public:
    std::string identifyIntent(const std::string& input) const;
};

class DeviceCapabilityProvider {
  public:
    explicit DeviceCapabilityProvider(Json::Value default_device_capability);

    Json::Value getDeviceCapability(const Json::Value& override_capability = Json::Value()) const;

  private:
    Json::Value default_device_capability_;
};

class CapabilityRouter {
  public:
    CapabilityRouter(PackManager& manager,
                     std::string catalog_url,
                     Json::Value default_device_capability);

    Json::Value routeUserRequest(const std::string& user_id,
                                 const std::string& input,
                                 const Json::Value& device_capability_override = Json::Value());
    Json::Value queryCompatiblePacks(const std::string& capability,
                                     const Json::Value& device_capability_override = Json::Value()) const;
    Json::Value usePack(const std::string& user_id, const std::string& pack_id, bool approve_dependencies);
    Json::Value invoke(const std::string& user_id,
                       const std::string& pack_id,
                       const std::string& prompt,
                       const std::string& options_json);

  private:
    std::string normalizeCapability(const std::string& skill, const Json::Value& capability_list) const;

    PackManager& manager_;
    std::string catalog_url_;
    HttpClient http_client_;
    IntentManager intent_manager_;
    DeviceCapabilityProvider capability_provider_;
};

}  // namespace edgeai
