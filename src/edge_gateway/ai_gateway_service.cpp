#include <edgeai/ai_gateway_service.h>
#include <edgeai/json_utils.h>

#include <iostream>
#include <stdexcept>

namespace edgeai {

namespace {

constexpr const char* kBusName = "com.example.EdgeAI";
constexpr const char* kObjectPath = "/com/example/EdgeAI/Gateway";
constexpr const char* kInterfaceName = "com.example.EdgeAI.Gateway1";

const gchar* kIntrospectionXml = R"XML(
<node>
  <interface name="com.example.EdgeAI.Gateway1">
    <method name="HandleUserRequest">
      <arg type="s" name="user_id" direction="in"/>
      <arg type="s" name="skill" direction="in"/>
      <arg type="s" name="device_capability_json" direction="in"/>
      <arg type="s" name="response_json" direction="out"/>
    </method>
    <method name="QueryPacks">
      <arg type="s" name="user_id" direction="in"/>
      <arg type="s" name="capability" direction="in"/>
      <arg type="s" name="device_capability_json" direction="in"/>
      <arg type="s" name="response_json" direction="out"/>
    </method>
    <method name="InstallPack">
      <arg type="s" name="user_id" direction="in"/>
      <arg type="s" name="pack_id" direction="in"/>
      <arg type="b" name="approve_dependencies" direction="in"/>
      <arg type="s" name="response_json" direction="out"/>
    </method>
    <method name="EnablePack">
      <arg type="s" name="user_id" direction="in"/>
      <arg type="s" name="pack_id" direction="in"/>
      <arg type="s" name="response_json" direction="out"/>
    </method>
    <method name="LoadPack">
      <arg type="s" name="user_id" direction="in"/>
      <arg type="s" name="pack_id" direction="in"/>
      <arg type="s" name="response_json" direction="out"/>
    </method>
    <method name="Invoke">
      <arg type="s" name="user_id" direction="in"/>
      <arg type="s" name="pack_id" direction="in"/>
      <arg type="s" name="prompt" direction="in"/>
      <arg type="s" name="options_json" direction="in"/>
      <arg type="s" name="response_json" direction="out"/>
    </method>
    <method name="UnloadPack">
      <arg type="s" name="user_id" direction="in"/>
      <arg type="s" name="pack_id" direction="in"/>
      <arg type="s" name="response_json" direction="out"/>
    </method>
    <method name="DisablePack">
      <arg type="s" name="user_id" direction="in"/>
      <arg type="s" name="pack_id" direction="in"/>
      <arg type="s" name="response_json" direction="out"/>
    </method>
    <method name="UninstallPack">
      <arg type="s" name="user_id" direction="in"/>
      <arg type="s" name="pack_id" direction="in"/>
      <arg type="b" name="force_shared_users" direction="in"/>
      <arg type="s" name="response_json" direction="out"/>
    </method>
    <method name="RollbackPack">
      <arg type="s" name="user_id" direction="in"/>
      <arg type="s" name="pack_id" direction="in"/>
      <arg type="s" name="response_json" direction="out"/>
    </method>
    <signal name="PackStateChanged">
      <arg type="s" name="event_json"/>
    </signal>
  </interface>
</node>
)XML";

const GDBusInterfaceVTable kInterfaceVTable = {
    &AIGatewayService::handleMethodCall,
    nullptr,
    nullptr,
    {0}
};

}  // namespace

AIGatewayService::AIGatewayService(PackManager& manager) : manager_(manager) {
    std::cerr << "[AIGatewayService::constructor] Initializing AI Gateway Service" << std::endl;
    manager_.setEventSink(this);
    GError* error = nullptr;
    introspection_data_ = g_dbus_node_info_new_for_xml(kIntrospectionXml, &error);
    if (!introspection_data_) {
        std::string message = error ? error->message : "Unknown DBus introspection error";
        if (error) {
            g_error_free(error);
        }
        throw std::runtime_error(message);
    }
    std::cerr << "[AIGatewayService::constructor] Service initialized successfully" << std::endl;
}

AIGatewayService::~AIGatewayService() {
    std::cerr << "[AIGatewayService::destructor] Cleaning up AI Gateway Service" << std::endl;
    if (registration_id_ != 0 && connection_) {
        std::cerr << "[AIGatewayService::destructor] Unregistering DBus object" << std::endl;
        g_dbus_connection_unregister_object(connection_, registration_id_);
    }
    if (owner_id_ != 0) {
        std::cerr << "[AIGatewayService::destructor] Releasing bus name" << std::endl;
        g_bus_unown_name(owner_id_);
    }
    if (loop_) {
        g_main_loop_unref(loop_);
    }
    if (introspection_data_) {
        g_dbus_node_info_unref(introspection_data_);
    }
    std::cerr << "[AIGatewayService::destructor] Cleanup complete" << std::endl;
}

void AIGatewayService::publish(const Json::Value& event) {
    if (!connection_) {
        std::cerr << "[AIGatewayService::publish] No DBus connection, skipping publish" << std::endl;
        return;
    }
    std::cerr << "[AIGatewayService::publish] Publishing PackStateChanged event" << std::endl;
    const auto payload = toJsonString(event);
    g_dbus_connection_emit_signal(connection_,
                                  nullptr,
                                  kObjectPath,
                                  kInterfaceName,
                                  "PackStateChanged",
                                  g_variant_new("(s)", payload.c_str()),
                                  nullptr);
}

void AIGatewayService::run() {
    std::cerr << "[AIGatewayService::run] Starting AI Gateway service" << std::endl;
    loop_ = g_main_loop_new(nullptr, FALSE);
    owner_id_ = g_bus_own_name(G_BUS_TYPE_SESSION,
                               kBusName,
                               G_BUS_NAME_OWNER_FLAGS_NONE,
                               &AIGatewayService::onBusAcquired,
                               &AIGatewayService::onNameAcquired,
                               &AIGatewayService::onNameLost,
                               this,
                               nullptr);
    std::cerr << "[AIGatewayService::run] DBus name owner requested, entering main loop" << std::endl;
    g_main_loop_run(loop_);
    std::cerr << "[AIGatewayService::run] Main loop exited" << std::endl;
}

void AIGatewayService::onBusAcquired(GDBusConnection* connection, const gchar* /*name*/, gpointer user_data) {
    auto* self = static_cast<AIGatewayService*>(user_data);
    std::cerr << "[AIGatewayService::onBusAcquired] Bus acquired, registering DBus object" << std::endl;
    self->connection_ = connection;
    GError* error = nullptr;
    self->registration_id_ = g_dbus_connection_register_object(connection,
                                                               kObjectPath,
                                                               self->introspection_data_->interfaces[0],
                                                               &kInterfaceVTable,
                                                               self,
                                                               nullptr,
                                                               &error);
    if (self->registration_id_ == 0) {
        std::cerr << "Failed to register DBus object: " << (error ? error->message : "unknown") << '\n';
        if (error) {
            g_error_free(error);
        }
    } else {
        std::cerr << "[AIGatewayService::onBusAcquired] DBus object registered successfully" << std::endl;
    }
}

void AIGatewayService::onNameAcquired(GDBusConnection* /*connection*/, const gchar* name, gpointer /*user_data*/) {
    std::cout << "AI Gateway DBus service acquired name: " << name << '\n';
}

void AIGatewayService::onNameLost(GDBusConnection* /*connection*/, const gchar* name, gpointer user_data) {
    std::cerr << "AI Gateway DBus service lost name: " << (name ? name : "(null)") << '\n';
    auto* self = static_cast<AIGatewayService*>(user_data);
    if (self->loop_) {
        g_main_loop_quit(self->loop_);
    }
}

void AIGatewayService::handleMethodCall(GDBusConnection* /*connection*/,
                                        const gchar* /*sender*/,
                                        const gchar* /*object_path*/,
                                        const gchar* /*interface_name*/,
                                        const gchar* method_name,
                                        GVariant* parameters,
                                        GDBusMethodInvocation* invocation,
                                        gpointer user_data) {
    auto* self = static_cast<AIGatewayService*>(user_data);
    std::cerr << "[AIGatewayService::handleMethodCall] Method called: " << method_name << std::endl;
    try {
        Json::Value response = self->dispatch(method_name, parameters);
        std::cerr << "[AIGatewayService::handleMethodCall] Method " << method_name << " completed successfully" << std::endl;
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", toJsonString(response).c_str()));
    } catch (const std::exception& ex) {
        std::cerr << "[AIGatewayService::handleMethodCall] Method " << method_name << " failed: " << ex.what() << std::endl;
        Json::Value error = makeStatus("error", ex.what());
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", toJsonString(error).c_str()));
    }
}

Json::Value AIGatewayService::dispatch(const std::string& method, GVariant* parameters) {
    if (method == "HandleUserRequest") {
        const gchar* user_id = nullptr;
        const gchar* skill = nullptr;
        const gchar* device_json = nullptr;
        g_variant_get(parameters, "(&s&s&s)", &user_id, &skill, &device_json);
        std::cerr << "[dispatch::HandleUserRequest] user_id=" << (user_id ? user_id : "(null)")
                  << " skill=" << (skill ? skill : "(null)") << std::endl;
        Json::Value response = manager_.handleUserRequest(user_id, skill, parseJson(device_json));
        response["user_id"] = user_id;
        return response;
    }
    if (method == "QueryPacks") {
        const gchar* user_id = nullptr;
        const gchar* capability = nullptr;
        const gchar* device_json = nullptr;
        g_variant_get(parameters, "(&s&s&s)", &user_id, &capability, &device_json);
        std::cerr << "[dispatch::QueryPacks] user_id=" << (user_id ? user_id : "(null)") << " capability=" << (capability ? capability : "(null)") << std::endl;
        Json::Value response = manager_.queryPacks(capability, parseJson(device_json));
        response["user_id"] = user_id;
        return response;
    }
    if (method == "InstallPack") {
        const gchar* user_id = nullptr;
        const gchar* pack_id = nullptr;
        gboolean approve = FALSE;
        g_variant_get(parameters, "(&s&sb)", &user_id, &pack_id, &approve);
        std::cerr << "[dispatch::InstallPack] user_id=" << (user_id ? user_id : "(null)") << " pack_id=" << (pack_id ? pack_id : "(null)") << " approve=" << approve << std::endl;
        return manager_.installPack(user_id, pack_id, approve);
    }
    if (method == "EnablePack") {
        const gchar* user_id = nullptr;
        const gchar* pack_id = nullptr;
        g_variant_get(parameters, "(&s&s)", &user_id, &pack_id);
        std::cerr << "[dispatch::EnablePack] user_id=" << (user_id ? user_id : "(null)") << " pack_id=" << (pack_id ? pack_id : "(null)") << std::endl;
        return manager_.enablePack(user_id, pack_id);
    }
    if (method == "LoadPack") {
        const gchar* user_id = nullptr;
        const gchar* pack_id = nullptr;
        g_variant_get(parameters, "(&s&s)", &user_id, &pack_id);
        std::cerr << "[dispatch::LoadPack] user_id=" << (user_id ? user_id : "(null)") << " pack_id=" << (pack_id ? pack_id : "(null)") << std::endl;
        return manager_.loadPack(user_id, pack_id);
    }
    if (method == "Invoke") {
        const gchar* user_id = nullptr;
        const gchar* pack_id = nullptr;
        const gchar* prompt = nullptr;
        const gchar* options_json = nullptr;
        g_variant_get(parameters, "(&s&s&s&s)", &user_id, &pack_id, &prompt, &options_json);
        std::cerr << "[dispatch::Invoke] user_id=" << (user_id ? user_id : "(null)") << " pack_id=" << (pack_id ? pack_id : "(null)") << " prompt=" << (prompt ? prompt : "(null)") << std::endl;
        return manager_.invoke(user_id, pack_id, prompt, options_json);
    }
    if (method == "UnloadPack") {
        const gchar* user_id = nullptr;
        const gchar* pack_id = nullptr;
        g_variant_get(parameters, "(&s&s)", &user_id, &pack_id);
        std::cerr << "[dispatch::UnloadPack] user_id=" << (user_id ? user_id : "(null)") << " pack_id=" << (pack_id ? pack_id : "(null)") << std::endl;
        return manager_.unloadPack(user_id, pack_id);
    }
    if (method == "DisablePack") {
        const gchar* user_id = nullptr;
        const gchar* pack_id = nullptr;
        g_variant_get(parameters, "(&s&s)", &user_id, &pack_id);
        std::cerr << "[dispatch::DisablePack] user_id=" << (user_id ? user_id : "(null)") << " pack_id=" << (pack_id ? pack_id : "(null)") << std::endl;
        return manager_.disablePack(user_id, pack_id);
    }
    if (method == "UninstallPack") {
        const gchar* user_id = nullptr;
        const gchar* pack_id = nullptr;
        gboolean force_shared = FALSE;
        g_variant_get(parameters, "(&s&sb)", &user_id, &pack_id, &force_shared);
        std::cerr << "[dispatch::UninstallPack] user_id=" << (user_id ? user_id : "(null)") << " pack_id=" << (pack_id ? pack_id : "(null)") << " force_shared=" << force_shared << std::endl;
        return manager_.uninstallPack(user_id, pack_id, force_shared);
    }
    if (method == "RollbackPack") {
        const gchar* user_id = nullptr;
        const gchar* pack_id = nullptr;
        g_variant_get(parameters, "(&s&s)", &user_id, &pack_id);
        std::cerr << "[dispatch::RollbackPack] user_id=" << (user_id ? user_id : "(null)") << " pack_id=" << (pack_id ? pack_id : "(null)") << std::endl;
        return manager_.rollbackPack(user_id, pack_id);
    }
    throw std::runtime_error("Unsupported method: " + method);
}

}  // namespace edgeai
