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
    handle_ = other.handle_;
    instance_ = other.instance_;
    other.handle_ = nullptr;
    other.instance_ = nullptr;
}

PackRuntime& PackRuntime::operator=(PackRuntime&& other) noexcept {
    if (this != &other) {
        unload();
        handle_ = other.handle_;
        instance_ = other.instance_;
        other.handle_ = nullptr;
        other.instance_ = nullptr;
    }
    return *this;
}

void PackRuntime::load(const std::filesystem::path& pack_root, const PackManifest& manifest, const std::filesystem::path& state_dir) {
    unload();

    const auto library_path = manifest.libraryPath(pack_root);
    handle_ = dlopen(library_path.string().c_str(), RTLD_NOW);
    if (!handle_) {
        throw std::runtime_error(std::string("Failed to load pack library: ") + dlerror());
    }

    const auto get_abi = resolveSymbol<GetAbiFn>(handle_, "edgeai_pack_get_abi_version");
    const auto create = resolveSymbol<CreateFn>(handle_, "edgeai_pack_create");
    if (get_abi() != EDGEAI_PACK_ABI_V1) {
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
    if (create(&host, &instance) != 0 || instance == nullptr) {
        unload();
        throw std::runtime_error("Pack factory failed to create instance");
    }

    instance_ = instance;
    if (instance_->activate && instance_->activate(instance_->user_data, "{}") != 0) {
        unload();
        throw std::runtime_error("Pack activation failed");
    }
}

void PackRuntime::configure(const std::string& config_json) {
    if (!instance_ || !instance_->configure) {
        return;
    }
    if (instance_->configure(instance_->user_data, config_json.c_str()) != 0) {
        throw std::runtime_error("Pack configuration failed");
    }
}

Json::Value PackRuntime::predict(const std::string& prompt, const std::string& options_json) {
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

    Json::Value value(Json::objectValue);
    value["result"] = response.output_text;
    value["metadata"] = parseJson(response.metadata_json[0] ? response.metadata_json : "{}");
    return value;
}

void PackRuntime::unload() {
    if (instance_) {
        if (instance_->deactivate) {
            instance_->deactivate(instance_->user_data);
        }
        if (instance_->destroy) {
            instance_->destroy(instance_->user_data);
        }
        delete instance_;
        instance_ = nullptr;
    }
    if (handle_) {
        dlclose(handle_);
        handle_ = nullptr;
    }
}

}  // namespace edgeai
