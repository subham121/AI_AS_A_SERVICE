#include <edgeai/fs_utils.h>
#include <edgeai/json_utils.h>
#include <edgeai/pack_abi.h>

#include "next_word_engine.h"

#include <dlfcn.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>

namespace {

struct NextWordPackState {
    std::filesystem::path pack_root;
    std::unique_ptr<edgeai::NextWordEngine> engine;
    void (*log)(int, const char*) = nullptr;
};

void logError(const NextWordPackState* state, const std::string& message) {
    if (state && state->log) {
        state->log(3, message.c_str());
    }
}

std::filesystem::path selfLibraryPath() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&edgeai_pack_get_abi_version), &info) == 0 || !info.dli_fname) {
        throw std::runtime_error("Unable to resolve plugin library path");
    }
    return std::filesystem::path(info.dli_fname);
}

std::filesystem::path discoverPackRoot() {
    return selfLibraryPath().parent_path().parent_path();
}

const char* manifestJson() {
    static std::string manifest = edgeai::readTextFile(discoverPackRoot() / "manifest.json");
    return manifest.c_str();
}

int activate(void* user_data, const char* activation_json) {
    auto* state = static_cast<NextWordPackState*>(user_data);
    const auto manifest_path = state->pack_root / "manifest.json";
    const auto model_path = state->pack_root / "model" / "next_word_bigram.onnx";
    const auto vocab_path = state->pack_root / "model" / "vocab.json";
    const auto transitions_path = state->pack_root / "model" / "transitions.json";
    if (!state || !state->engine || !std::filesystem::exists(manifest_path) || !std::filesystem::exists(model_path) ||
        !std::filesystem::exists(vocab_path) || !std::filesystem::exists(transitions_path)) {
        return -1;
    }
    try {
        state->engine->activate(activation_json ? activation_json : "{}");
        return 0;
    } catch (const std::exception& ex) {
        logError(state, std::string("activate failed: ") + ex.what());
        return -1;
    } catch (...) {
        logError(state, "activate failed: unknown error");
        return -1;
    }
}

int configure(void* user_data, const char* config_json) {
    auto* state = static_cast<NextWordPackState*>(user_data);
    if (!state || !state->engine) {
        return -1;
    }
    try {
        state->engine->configure(config_json ? config_json : "{}");
        return 0;
    } catch (const std::exception& ex) {
        logError(state, std::string("configure failed: ") + ex.what());
        return -1;
    } catch (...) {
        logError(state, "configure failed: unknown error");
        return -1;
    }
}

int predict(void* user_data, const EdgeAIPackRequestV1* request, EdgeAIPackResponseV1* response) {
    auto* state = static_cast<NextWordPackState*>(user_data);
    if (!state || !state->engine || !response) {
        return -1;
    }
    try {
        const Json::Value result = state->engine->predict(request && request->prompt ? request->prompt : "",
                                                          request && request->options_json ? request->options_json : "{}");
        std::snprintf(response->output_text, sizeof(response->output_text), "%s", result.get("result", "").asCString());
        std::snprintf(response->metadata_json,
                      sizeof(response->metadata_json),
                      "%s",
                      edgeai::toJsonString(result["metadata"]).c_str());
        return 0;
    } catch (const std::exception& ex) {
        logError(state, std::string("predict failed: ") + ex.what());
        return -1;
    } catch (...) {
        logError(state, "predict failed: unknown error");
        return -1;
    }
}

int deactivate(void* user_data) {
    auto* state = static_cast<NextWordPackState*>(user_data);
    if (!state || !state->engine) {
        return 0;
    }
    try {
        state->engine->deactivate();
        return 0;
    } catch (const std::exception& ex) {
        logError(state, std::string("deactivate failed: ") + ex.what());
        return -1;
    } catch (...) {
        logError(state, "deactivate failed: unknown error");
        return -1;
    }
}

void destroy(void* user_data) {
    delete static_cast<NextWordPackState*>(user_data);
}

}  // namespace

extern "C" int edgeai_pack_get_abi_version(void) {
    return EDGEAI_PACK_ABI_V1;
}

extern "C" const char* edgeai_pack_get_manifest_json(void) {
    return manifestJson();
}

extern "C" int edgeai_pack_create(const EdgeAIPackHostV1* host, EdgeAIPackInstanceV1** instance) {
    if (!host || !instance) {
        return -1;
    }
    auto* state = new (std::nothrow) NextWordPackState;
    if (!state) {
        return -1;
    }
    try {
        const auto pack_root = host->pack_root ? std::filesystem::path(host->pack_root) : discoverPackRoot();
        const auto state_dir = host->state_dir ? std::filesystem::path(host->state_dir) : std::filesystem::temp_directory_path();
        state->pack_root = pack_root;
        state->log = host->log;
        state->engine = std::make_unique<edgeai::NextWordEngine>(pack_root, state_dir, host->log);
    } catch (...) {
        delete state;
        return -1;
    }

    auto* pack_instance = new EdgeAIPackInstanceV1{
        .user_data = state,
        .activate = activate,
        .configure = configure,
        .predict = predict,
        .deactivate = deactivate,
        .destroy = destroy,
    };
    *instance = pack_instance;
    return 0;
}
