#include <edgeai/ai_gateway_service.h>
#include <edgeai/json_utils.h>
#include <edgeai/pack_manager.h>

#include <iostream>

namespace {

Json::Value defaultDeviceCapability() {
    Json::Value value(Json::objectValue);
#ifdef __APPLE__
    value["os_family"] = "darwin";
#else
    value["os_family"] = "linux";
#endif
#if defined(__aarch64__) || defined(__arm64__) || defined(__APPLE__)
    value["architecture"] = "arm64";
#else
    value["architecture"] = "x86_64";
#endif
    value["ram_mb"] = 2048;
    value["accelerators"] = Json::Value(Json::arrayValue);
    return value;
}

void printHelp() {
    std::cout << "edge_gatewayd\n"
              << "  Sample DBus edge AI gateway and pack manager.\n\n"
              << "Options:\n"
              << "  --help        Show this help message\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--help") {
        printHelp();
        return 0;
    }

    try {
        edgeai::PackManager manager(
            EDGEAI_DEFAULT_STATE_DIR,
            EDGEAI_DEFAULT_STAGING_DIR,
            EDGEAI_DEFAULT_INSTALL_DIR,
            EDGEAI_DEFAULT_CATALOG_URL,
            defaultDeviceCapability());
        manager.initialize();

        edgeai::AIGatewayService service(manager);
        service.run();
    } catch (const std::exception& ex) {
        std::cerr << "edge_gatewayd failed: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
