#include <edgeai/fs_utils.h>
#include <edgeai/json_utils.h>
#include <edgeai/pack_abi.h>
#include <edgeai/process_utils.h>

#include <dlfcn.h>

#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

struct NextWordPackState {
    std::filesystem::path pack_root;
    std::filesystem::path state_dir;
    std::string config_json = "{}";
};

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

int activate(void* user_data, const char* /*activation_json*/) {
    auto* state = static_cast<NextWordPackState*>(user_data);
    const auto manifest_path = state->pack_root / "manifest.json";
    const auto helper_path = state->pack_root / "runtime" / "next_word_helper.py";
    const auto model_path = state->pack_root / "model" / "next_word_bigram.onnx";
    if (!std::filesystem::exists(manifest_path) || !std::filesystem::exists(helper_path) || !std::filesystem::exists(model_path)) {
        return -1;
    }
    return 0;
}

int configure(void* user_data, const char* config_json) {
    auto* state = static_cast<NextWordPackState*>(user_data);
    state->config_json = config_json ? config_json : "{}";
    return 0;
}

int predict(void* user_data, const EdgeAIPackRequestV1* request, EdgeAIPackResponseV1* response) {
    auto* state = static_cast<NextWordPackState*>(user_data);
    const auto work_dir = state->state_dir / "next_word_runtime";
    edgeai::ensureDirectory(work_dir);

    Json::Value request_json(Json::objectValue);
    request_json["prompt"] = request && request->prompt ? request->prompt : "";
    if (request && request->options_json && std::strlen(request->options_json) > 0) {
        request_json["options"] = edgeai::parseJson(request->options_json);
    } else {
        request_json["options"] = Json::Value(Json::objectValue);
    }
    request_json["config"] = edgeai::parseJson(state->config_json);

    const auto request_path = edgeai::makeTempJson(work_dir, "request", edgeai::toJsonString(request_json));
    const auto response_path = work_dir / "response.json";
    const char* python = std::getenv("EDGEAI_PYTHON");
    const std::string python_bin = python ? python : "python3";
    const auto helper = state->pack_root / "runtime" / "next_word_helper.py";

    auto result = edgeai::runCommandCapture({
        python_bin,
        helper.string(),
        "--request",
        request_path.string(),
        "--response",
        response_path.string(),
        "--pack-root",
        state->pack_root.string(),
    });
    if (result.exit_code != 0) {
        return -1;
    }

    const Json::Value helper_response = edgeai::parseJson(edgeai::readTextFile(response_path));
    std::snprintf(response->output_text, sizeof(response->output_text), "%s", helper_response.get("result", "").asCString());
    std::snprintf(response->metadata_json,
                  sizeof(response->metadata_json),
                  "%s",
                  edgeai::toJsonString(helper_response["metadata"]).c_str());
    return 0;
}

int deactivate(void* /*user_data*/) {
    return 0;
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
    auto* state = new NextWordPackState{
        .pack_root = host->pack_root ? std::filesystem::path(host->pack_root) : discoverPackRoot(),
        .state_dir = host->state_dir ? std::filesystem::path(host->state_dir) : std::filesystem::temp_directory_path(),
    };

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
