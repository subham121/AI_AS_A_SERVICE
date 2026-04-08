#pragma once

#include <edgeai/capability_router.h>
#include <edgeai/pack_manager.h>

#include <gio/gio.h>
#include <json/json.h>

#include <string>

namespace edgeai {

class AIGatewayService : public PackEventSink {
  public:
    AIGatewayService(PackManager& manager, CapabilityRouter& capability_router);
    ~AIGatewayService() override;

    AIGatewayService(const AIGatewayService&) = delete;
    AIGatewayService& operator=(const AIGatewayService&) = delete;

    void publish(const Json::Value& event) override;
    void run();

    static void onBusAcquired(GDBusConnection* connection, const gchar* name, gpointer user_data);
    static void onNameAcquired(GDBusConnection* connection, const gchar* name, gpointer user_data);
    static void onNameLost(GDBusConnection* connection, const gchar* name, gpointer user_data);
    static void handleMethodCall(GDBusConnection* connection,
                                 const gchar* sender,
                                 const gchar* object_path,
                                 const gchar* interface_name,
                                 const gchar* method_name,
                                 GVariant* parameters,
                                 GDBusMethodInvocation* invocation,
                                 gpointer user_data);

  private:
    Json::Value dispatch(const std::string& method, GVariant* parameters);

    PackManager& manager_;
    CapabilityRouter& capability_router_;
    GMainLoop* loop_ = nullptr;
    GDBusNodeInfo* introspection_data_ = nullptr;
    GDBusConnection* connection_ = nullptr;
    guint owner_id_ = 0;
    guint registration_id_ = 0;
};

}  // namespace edgeai
