#include <edgeai/fs_utils.h>
#include <edgeai/json_utils.h>
#include <edgeai/pack_runtime.h>

#include <dlfcn.h>

#include <cstring>
#include <iostream>
#include <stdexcept>

namespace edgeai {

namespace {

using GetAbiFn = int (*)();
using GetManifestFn = const char* (*)();
using CreateFn = int (*)(const EdgeAIPackHostV1*, EdgeAIPackInstanceV1**);

void hostLog(int level, const char* message) {
    std::cerr << "[pack-host][" << level << "] " << (message ? message : "") << '\n';
}

template <typename Fn>
Fn resolveSymbol(void* handle, const char* symbol) {
    void* address = dlsym(handle, symbol);
    if (!address) {
        throw std::runtime_error(std::string("Missing pack symbol: ") + symbol);
    }
    return reinterpret_cast<Fn>(address);
}

}  // namespace

PackRuntime::~PackRuntime() {
    unload();
}

PackRuntime::PackRuntime(PackRuntime&& other) noexcept {
    std::cerr << "[PackRuntime::move_constructor] Moving pack runtime" << std::endl;
    handle_ = other.handle_;
    instance_ = other.instance_;
    other.handle_ = nullptr;
    other.instance_ = nullptr;
}

PackRuntime& PackRuntime::operator=(PackRuntime&& other) noexcept {
    std::cerr << "[PackRuntime::move_assignment] Move assigning pack runtime" << std::endl;
    if (this != &other) {
        unload();
        handle_ = other.handle_;
        instance_ = other.instance_;
        other.handle_ = nullptr;
        other.instance_ = nullptr;
    }
    return *this;
}

int PackRuntime::readAbiVersion(const std::filesystem::path& pack_root, const PackManifest& manifest) {
    const auto library_path = manifest.libraryPath(pack_root);
    void* handle = dlopen(library_path.string().c_str(), RTLD_NOW);
    if (!handle) {
        throw std::runtime_error(std::string("Failed to load pack library for ABI validation: ") + dlerror());
    }
    const auto get_abi = resolveSymbol<GetAbiFn>(handle, "edgeai_pack_get_abi_version");
    const int abi_version = get_abi();
    dlclose(handle);
    return abi_version;
}

void PackRuntime::load(const std::filesystem::path& pack_root, const PackManifest& manifest, const std::filesystem::path& state_dir) {
    std::cerr << "[PackRuntime::load] Starting pack load from " << pack_root << std::endl;
    unload();

    const auto library_path = manifest.libraryPath(pack_root);
    std::cerr << "[PackRuntime::load] Loading library: " << library_path << std::endl;
    handle_ = dlopen(library_path.string().c_str(), RTLD_NOW);
    if (!handle_) {
        throw std::runtime_error(std::string("Failed to load pack library: ") + dlerror());
    }
    std::cerr << "[PackRuntime::load] Library loaded successfully" << std::endl;

    const auto get_abi = resolveSymbol<GetAbiFn>(handle_, "edgeai_pack_get_abi_version");
    const auto create = resolveSymbol<CreateFn>(handle_, "edgeai_pack_create");
    int abi_version = get_abi();
    std::cerr << "[PackRuntime::load] ABI version: " << abi_version << std::endl;
    if (abi_version != EDGEAI_PACK_ABI_V1) {
        unload();
        throw std::runtime_error("Pack ABI mismatch");
    }

    const std::string pack_root_string = pack_root.string();
    const std::string state_dir_string = state_dir.string();
    EdgeAIPackHostV1 host{
        .log = hostLog,
        .state_dir = state_dir_string.c_str(),
        .pack_root = pack_root_string.c_str(),
    };

    EdgeAIPackInstanceV1* instance = nullptr;
    std::cerr << "[PackRuntime::load] Creating pack instance" << std::endl;
    if (create(&host, &instance) != 0 || instance == nullptr) {
        unload();
        throw std::runtime_error("Pack factory failed to create instance");
    }
    std::cerr << "[PackRuntime::load] Pack instance created" << std::endl;

    instance_ = instance;
    if (instance_->activate && instance_->activate(instance_->user_data, "{}") != 0) {
        unload();
        throw std::runtime_error("Pack activation failed");
    }
    std::cerr << "[PackRuntime::load] Pack activated successfully" << std::endl;
}

void PackRuntime::configure(const std::string& config_json) {
    std::cerr << "[PackRuntime::configure] Starting pack configuration" << std::endl;
    if (!instance_ || !instance_->configure) {
        std::cerr << "[PackRuntime::configure] Configure not available, skipping" << std::endl;
        return;
    }
    if (instance_->configure(instance_->user_data, config_json.c_str()) != 0) {
        throw std::runtime_error("Pack configuration failed");
    }
    std::cerr << "[PackRuntime::configure] Configuration completed successfully" << std::endl;
}

Json::Value PackRuntime::predict(const std::string& prompt, const std::string& options_json) {
    std::cerr << "[PackRuntime::predict] Starting prediction with prompt: " << prompt << std::endl;
    if (!instance_ || !instance_->predict) {
        throw std::runtime_error("Pack runtime is not loaded");
    }

    EdgeAIPackRequestV1 request{
        .prompt = prompt.c_str(),
        .options_json = options_json.c_str(),
    };
    EdgeAIPackResponseV1 response{};
    if (instance_->predict(instance_->user_data, &request, &response) != 0) {
        throw std::runtime_error("Pack prediction failed");
    }
    std::cerr << "[PackRuntime::predict] Prediction completed, output: " << response.output_text << std::endl;

    Json::Value value(Json::objectValue);
    value["result"] = response.output_text;
    value["metadata"] = parseJson(response.metadata_json[0] ? response.metadata_json : "{}");
    return value;
}

void PackRuntime::unload() {
    std::cerr << "[PackRuntime::unload] Starting pack unload" << std::endl;
    if (instance_) {
        if (instance_->deactivate) {
            std::cerr << "[PackRuntime::unload] Deactivating instance" << std::endl;
            instance_->deactivate(instance_->user_data);
        }
        if (instance_->destroy) {
            std::cerr << "[PackRuntime::unload] Destroying instance" << std::endl;
            instance_->destroy(instance_->user_data);
        }
        delete instance_;
        instance_ = nullptr;
        std::cerr << "[PackRuntime::unload] Instance cleaned up" << std::endl;
    }
    if (handle_) {
        std::cerr << "[PackRuntime::unload] Unloading library" << std::endl;
        dlclose(handle_);
        handle_ = nullptr;
        std::cerr << "[PackRuntime::unload] Library unloaded successfully" << std::endl;
    }
}

}  // namespace edgeai
