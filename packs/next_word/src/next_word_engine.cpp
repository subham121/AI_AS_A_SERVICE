#include "next_word_engine.h"

#include <edgeai/fs_utils.h>
#include <edgeai/json_utils.h>

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace edgeai {

namespace {

constexpr const char* kDefaultToken = "hello";
constexpr const char* kDefaultUnknownToken = "world";

Json::Value emptyObject() {
    return Json::Value(Json::objectValue);
}

Json::Value parseObjectOrEmpty(const std::string& payload) {
    if (payload.empty()) {
        return emptyObject();
    }
    Json::Value value = parseJson(payload);
    return value.isObject() ? value : emptyObject();
}

Json::Value loadJsonFile(const std::filesystem::path& path) {
    return parseJson(readTextFile(path));
}

Json::Value mergeObjects(const Json::Value& base, const Json::Value& overrides) {
    if (!base.isObject()) {
        return overrides.isObject() ? overrides : emptyObject();
    }
    Json::Value merged = base;
    if (!overrides.isObject()) {
        return merged;
    }

    for (const auto& name : overrides.getMemberNames()) {
        if (merged.isMember(name) && merged[name].isObject() && overrides[name].isObject()) {
            merged[name] = mergeObjects(merged[name], overrides[name]);
        } else {
            merged[name] = overrides[name];
        }
    }
    return merged;
}

std::string requireString(const Json::Value& value, const char* field) {
    if (!value.isMember(field) || !value[field].isString()) {
        throw std::runtime_error(std::string("Missing string field: ") + field);
    }
    return value[field].asString();
}

std::string normalizeToken(const std::string& prompt) {
    static const std::regex kTokenPattern(R"([A-Za-z']+)");
    std::string last = kDefaultToken;
    for (std::sregex_iterator it(prompt.begin(), prompt.end(), kTokenPattern), end; it != end; ++it) {
        last = it->str();
    }
    for (char& ch : last) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return last;
}

GraphOptimizationLevel parseGraphOptimization(const std::string& value) {
    if (value == "disabled") {
        return GraphOptimizationLevel::ORT_DISABLE_ALL;
    }
    if (value == "basic") {
        return GraphOptimizationLevel::ORT_ENABLE_BASIC;
    }
    if (value == "all") {
        return GraphOptimizationLevel::ORT_ENABLE_ALL;
    }
    return GraphOptimizationLevel::ORT_ENABLE_EXTENDED;
}

Ort::Env& sharedOrtEnv() {
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "edgeai_next_word");
    return env;
}

enum class BackendMode {
    Auto,
    Onnx,
    TransitionTable,
};

BackendMode parseBackendMode(const std::string& value) {
    if (value == "onnx" || value == "onnxruntime") {
        return BackendMode::Onnx;
    }
    if (value == "table" || value == "transition_table" || value == "fallback") {
        return BackendMode::TransitionTable;
    }
    return BackendMode::Auto;
}

std::string backendModeName(BackendMode value) {
    switch (value) {
        case BackendMode::Onnx:
            return "onnxruntime";
        case BackendMode::TransitionTable:
            return "transition_table";
        case BackendMode::Auto:
        default:
            return "auto";
    }
}

struct PredictionResult {
    std::string backend;
    int64_t next_token_id = 0;
    bool fallback_used = false;
};

}  // namespace

class NextWordEngine::Impl {
  public:
    Impl(std::filesystem::path pack_root, std::filesystem::path state_dir, void (*logger)(int, const char*))
        : pack_root_(std::move(pack_root)), state_dir_(std::move(state_dir)), logger_(logger) {}

    void activate(const std::string& activation_json) {
        std::lock_guard<std::mutex> lock(mutex_);
        manifest_ = loadJsonFile(pack_root_ / "manifest.json");
        runtime_meta_ = manifest_.isMember("runtime") && manifest_["runtime"].isObject() ? manifest_["runtime"] : emptyObject();
        if (runtime_meta_.empty()) {
            throw std::runtime_error("Pack manifest is missing runtime metadata");
        }

        const auto vocab_path = pack_root_ / requireString(runtime_meta_, "vocab");
        const auto transitions_path = pack_root_ / requireString(runtime_meta_, "transitions");
        const auto model_path = pack_root_ / requireString(runtime_meta_, "model");
        if (!std::filesystem::exists(vocab_path) || !std::filesystem::exists(transitions_path) || !std::filesystem::exists(model_path)) {
            throw std::runtime_error("Pack runtime artifacts are missing");
        }

        input_name_ = runtime_meta_.get("input_name", "input_token").asString();
        output_name_ = runtime_meta_.get("output_name", "next_token").asString();
        model_relative_path_ = runtime_meta_.get("model", "model/next_word_bigram.onnx").asString();
        fallback_backend_ = runtime_meta_.get("fallback_backend", "transition_table").asString();
        lazy_load_ = runtime_meta_.get("lazy_load", true).asBool();
        warmup_on_activate_ = runtime_meta_.get("warmup_on_activate", false).asBool();
        max_concurrent_inferences_ = std::max(1U, runtime_meta_.get("max_concurrent_inferences", 1).asUInt());

        loadVocabularyLocked(vocab_path);
        loadTransitionsLocked(transitions_path);
        default_config_ = loadDefaultConfigLocked();
        config_ = default_config_;

        const Json::Value activation = parseObjectOrEmpty(activation_json);
        if (activation.isMember("config") && activation["config"].isObject()) {
            config_ = mergeObjects(default_config_, activation["config"]);
        }

        ensureDirectory(state_dir_);
        resetSessionLocked();
        active_ = true;
        last_error_.clear();
        log(1, "Next-word pack activated");

        if (!lazy_load_ || warmup_on_activate_) {
            ensureSessionLoadedLocked();
            if (warmup_on_activate_) {
                (void)predictWithOnnxLocked(tokenIdForLocked(kDefaultToken));
            }
        }
    }

    void configure(const std::string& config_json) {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = mergeObjects(default_config_, parseObjectOrEmpty(config_json));
        resetSessionLocked();

        if (active_ && !lazy_load_) {
            ensureSessionLoadedLocked();
        }
        if (active_ && warmup_on_activate_ && effectiveBackendLocked(emptyObject()) != BackendMode::TransitionTable) {
            (void)predictWithOnnxLocked(tokenIdForLocked(kDefaultToken));
        }
        log(1, "Next-word pack configured");
    }

    Json::Value predict(const std::string& prompt, const std::string& options_json) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_) {
            throw std::runtime_error("Pack is not active");
        }

        const Json::Value options = parseObjectOrEmpty(options_json);
        const unsigned int requested_tokens = options.get("max_tokens", config_.get("max_tokens", 1)).asUInt();
        if (requested_tokens > 1U) {
            throw std::runtime_error("Next-word pack supports only one generated token");
        }
        if (active_inferences_ >= max_concurrent_inferences_) {
            throw std::runtime_error("Maximum concurrent inferences exceeded");
        }

        ++active_inferences_;
        const auto started_at = std::chrono::steady_clock::now();
        try {
            const std::string token = normalizeToken(prompt);
            const int64_t token_id = tokenIdForLocked(token);

            const PredictionResult prediction = predictNextTokenLocked(token, token_id, options);
            const std::string result_token = vocab_.at(static_cast<size_t>(prediction.next_token_id));

            ++total_inferences_;
            last_backend_ = prediction.backend;
            last_error_.clear();
            --active_inferences_;

            const auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - started_at)
                                        .count();

            Json::Value response(Json::objectValue);
            response["result"] = result_token;
            response["metadata"] = buildMetadataLocked(token,
                                                       token_id,
                                                       prediction.next_token_id,
                                                       prediction.backend,
                                                       prediction.fallback_used,
                                                       latency_ms);
            return response;
        } catch (const std::exception& ex) {
            ++failed_inferences_;
            last_error_ = ex.what();
            --active_inferences_;
            throw;
        }
    }

    void deactivate() {
        std::lock_guard<std::mutex> lock(mutex_);
        resetSessionLocked();
        active_ = false;
        log(1, "Next-word pack deactivated");
    }

  private:
    void log(int level, const std::string& message) const {
        if (logger_) {
            logger_(level, message.c_str());
        }
    }

    Json::Value loadDefaultConfigLocked() const {
        if (!manifest_.isMember("entrypoint") || !manifest_["entrypoint"].isObject()) {
            return emptyObject();
        }
        const std::string default_config = manifest_["entrypoint"].get("default_config", "").asString();
        if (default_config.empty()) {
            return emptyObject();
        }

        const auto config_path = pack_root_ / default_config;
        if (!std::filesystem::exists(config_path)) {
            return emptyObject();
        }
        Json::Value config = loadJsonFile(config_path);
        return config.isObject() ? config : emptyObject();
    }

    void loadVocabularyLocked(const std::filesystem::path& path) {
        vocab_.clear();
        vocab_index_.clear();

        const Json::Value value = loadJsonFile(path);
        const Json::Value tokens = value["tokens"];
        if (!tokens.isArray() || tokens.empty()) {
            throw std::runtime_error("Vocabulary file is invalid");
        }

        for (Json::ArrayIndex index = 0; index < tokens.size(); ++index) {
            const std::string token = tokens[index].asString();
            vocab_.push_back(token);
            vocab_index_[token] = static_cast<int64_t>(index);
        }
    }

    void loadTransitionsLocked(const std::filesystem::path& path) {
        transitions_.clear();
        const Json::Value value = loadJsonFile(path);
        if (!value.isObject()) {
            throw std::runtime_error("Transition table file is invalid");
        }

        for (const auto& token : value.getMemberNames()) {
            transitions_[token] = value[token].asString();
        }
    }

    int64_t tokenIdForLocked(const std::string& token) const {
        const auto it = vocab_index_.find(token);
        if (it != vocab_index_.end()) {
            return it->second;
        }

        const auto unknown = config_.get("unknown_token", kDefaultUnknownToken).asString();
        const auto fallback = vocab_index_.find(unknown);
        if (fallback != vocab_index_.end()) {
            return fallback->second;
        }
        return vocab_index_.at(kDefaultToken);
    }

    BackendMode effectiveBackendLocked(const Json::Value& options) const {
        if (options.isObject() && options.isMember("backend") && options["backend"].isString()) {
            return parseBackendMode(options["backend"].asString());
        }
        if (config_.isObject() && config_.isMember("backend") && config_["backend"].isString()) {
            return parseBackendMode(config_["backend"].asString());
        }
        return parseBackendMode(runtime_meta_.get("backend", "auto").asString());
    }

    PredictionResult predictNextTokenLocked(const std::string& token, int64_t token_id, const Json::Value& options) {
        const BackendMode backend = effectiveBackendLocked(options);
        if (backend == BackendMode::TransitionTable) {
            return predictWithTransitionsLocked(token);
        }

        try {
            return predictWithOnnxLocked(token_id);
        } catch (const std::exception& ex) {
            if (backend != BackendMode::Auto || fallback_backend_ == "none") {
                throw;
            }

            log(2, std::string("ONNX inference failed, using transition-table fallback: ") + ex.what());
            PredictionResult result = predictWithTransitionsLocked(token);
            result.fallback_used = true;
            return result;
        }
    }

    PredictionResult predictWithOnnxLocked(int64_t token_id) {
        ensureSessionLoadedLocked();

        std::array<int64_t, 1> input_values{token_id};
        std::array<int64_t, 1> input_shape{1};
        Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value input_tensor = Ort::Value::CreateTensor<int64_t>(memory_info,
                                                                    input_values.data(),
                                                                    input_values.size(),
                                                                    input_shape.data(),
                                                                    input_shape.size());

        Ort::RunOptions run_options;
        auto output_tensors = session_->Run(run_options,
                                            input_names_.data(),
                                            &input_tensor,
                                            1,
                                            output_names_.data(),
                                            1);
        if (output_tensors.empty() || !output_tensors.front().IsTensor()) {
            throw std::runtime_error("ONNX runtime did not return a tensor output");
        }

        const auto* output = output_tensors.front().GetTensorData<int64_t>();
        if (!output) {
            throw std::runtime_error("ONNX runtime returned an empty tensor");
        }
        const int64_t next_token_id = output[0];
        if (next_token_id < 0 || static_cast<size_t>(next_token_id) >= vocab_.size()) {
            throw std::runtime_error("ONNX runtime produced an invalid token id");
        }

        PredictionResult result;
        result.backend = "onnxruntime";
        result.next_token_id = next_token_id;
        return result;
    }

    PredictionResult predictWithTransitionsLocked(const std::string& token) const {
        auto it = transitions_.find(token);
        if (it == transitions_.end()) {
            const std::string unknown = config_.get("unknown_token", kDefaultUnknownToken).asString();
            it = transitions_.find(unknown);
            if (it == transitions_.end()) {
                it = transitions_.find(kDefaultToken);
            }
        }
        if (it == transitions_.end()) {
            throw std::runtime_error("Transition-table fallback is unavailable");
        }

        const auto next_it = vocab_index_.find(it->second);
        if (next_it == vocab_index_.end()) {
            throw std::runtime_error("Transition-table output token is not in the vocabulary");
        }

        PredictionResult result;
        result.backend = "transition_table";
        result.next_token_id = next_it->second;
        return result;
    }

    void ensureSessionLoadedLocked() {
        if (session_) {
            return;
        }

        const auto model_path = pack_root_ / model_relative_path_;
        if (!std::filesystem::exists(model_path)) {
            throw std::runtime_error("ONNX model file is missing");
        }

        Ort::SessionOptions session_options;
        const Json::Value session_config =
            runtime_meta_.isMember("session") && runtime_meta_["session"].isObject() ? runtime_meta_["session"] : emptyObject();

        const int intra_threads = config_.get("intra_op_threads", session_config.get("intra_op_threads", 1)).asInt();
        const int inter_threads = config_.get("inter_op_threads", session_config.get("inter_op_threads", 1)).asInt();
        session_options.SetIntraOpNumThreads(std::max(1, intra_threads));
        session_options.SetInterOpNumThreads(std::max(1, inter_threads));
        session_options.SetGraphOptimizationLevel(
            parseGraphOptimization(config_.get("graph_optimization", session_config.get("graph_optimization", "extended")).asString()));

        session_ = std::make_unique<Ort::Session>(sharedOrtEnv(), model_path.string().c_str(), session_options);
        validateSessionContractLocked();
        log(1, std::string("ONNX model loaded from ") + model_path.string());
    }

    void validateSessionContractLocked() {
        if (!session_) {
            throw std::runtime_error("ONNX session is not initialized");
        }
        if (session_->GetInputCount() != 1 || session_->GetOutputCount() != 1) {
            throw std::runtime_error("Unexpected ONNX model I/O count");
        }

        Ort::AllocatorWithDefaultOptions allocator;
        auto actual_input = session_->GetInputNameAllocated(0, allocator);
        auto actual_output = session_->GetOutputNameAllocated(0, allocator);
        if (input_name_ != actual_input.get()) {
            throw std::runtime_error("ONNX input name does not match the declared pack contract");
        }
        if (output_name_ != actual_output.get()) {
            throw std::runtime_error("ONNX output name does not match the declared pack contract");
        }

        input_names_.assign(1, input_name_.c_str());
        output_names_.assign(1, output_name_.c_str());
    }

    Json::Value buildMetadataLocked(const std::string& token,
                                    int64_t token_id,
                                    int64_t next_token_id,
                                    const std::string& backend,
                                    bool fallback_used,
                                    long long latency_ms) const {
        Json::Value metadata(Json::objectValue);
        metadata["backend"] = backend;
        metadata["requested_backend"] = backendModeName(effectiveBackendLocked(emptyObject()));
        metadata["fallback_used"] = fallback_used;
        metadata["token"] = token;
        metadata["token_id"] = Json::Int64(token_id);
        metadata["next_token_id"] = Json::Int64(next_token_id);
        metadata["model"] = model_relative_path_;
        metadata["session_loaded"] = session_ != nullptr;
        metadata["lazy_load"] = lazy_load_;
        metadata["latency_ms"] = Json::Int64(latency_ms);
        metadata["total_inferences"] = Json::UInt64(total_inferences_);
        metadata["failed_inferences"] = Json::UInt64(failed_inferences_);
        metadata["max_concurrent_inferences"] = max_concurrent_inferences_;
        if (!last_error_.empty()) {
            metadata["last_error"] = last_error_;
        }
        return metadata;
    }

    void resetSessionLocked() {
        output_names_.clear();
        input_names_.clear();
        session_.reset();
    }

    std::filesystem::path pack_root_;
    std::filesystem::path state_dir_;
    void (*logger_)(int, const char*) = nullptr;
    std::mutex mutex_;
    Json::Value manifest_;
    Json::Value runtime_meta_;
    Json::Value default_config_ = emptyObject();
    Json::Value config_ = emptyObject();
    std::vector<std::string> vocab_;
    std::unordered_map<std::string, int64_t> vocab_index_;
    std::unordered_map<std::string, std::string> transitions_;
    std::unique_ptr<Ort::Session> session_;
    std::string input_name_ = "input_token";
    std::string output_name_ = "next_token";
    std::vector<const char*> input_names_;
    std::vector<const char*> output_names_;
    std::string model_relative_path_ = "model/next_word_bigram.onnx";
    std::string fallback_backend_ = "transition_table";
    std::size_t total_inferences_ = 0;
    std::size_t failed_inferences_ = 0;
    std::size_t active_inferences_ = 0;
    uint32_t max_concurrent_inferences_ = 1;
    bool lazy_load_ = true;
    bool warmup_on_activate_ = false;
    bool active_ = false;
    std::string last_backend_;
    std::string last_error_;
};

NextWordEngine::NextWordEngine(std::filesystem::path pack_root,
                               std::filesystem::path state_dir,
                               void (*logger)(int, const char*))
    : impl_(std::make_unique<Impl>(std::move(pack_root), std::move(state_dir), logger)) {}

NextWordEngine::~NextWordEngine() = default;

NextWordEngine::NextWordEngine(NextWordEngine&&) noexcept = default;

NextWordEngine& NextWordEngine::operator=(NextWordEngine&&) noexcept = default;

void NextWordEngine::activate(const std::string& activation_json) {
    impl_->activate(activation_json);
}

void NextWordEngine::configure(const std::string& config_json) {
    impl_->configure(config_json);
}

Json::Value NextWordEngine::predict(const std::string& prompt, const std::string& options_json) {
    return impl_->predict(prompt, options_json);
}

void NextWordEngine::deactivate() {
    impl_->deactivate();
}

}  // namespace edgeai
