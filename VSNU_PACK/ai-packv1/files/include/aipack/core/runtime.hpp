// =============================================================================
// AI Packs System - AI Runtime & Inference Engine
// Model loading, inference execution, pipeline orchestration
// =============================================================================
#pragma once

#include "types.hpp"
#include "manifest.hpp"
#include "interfaces.hpp"
#include "logger.hpp"
#include "event_bus.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <functional>
#include <algorithm>
#include <numeric>
#include <cmath>

namespace aipack {

static const char* TAG_RUNTIME = "AIRuntime";

// =============================================================================
// Model Runtime Backend Interface
// Abstraction over different inference engines (ONNX, TFLite, custom)
// =============================================================================
class IModelBackend {
public:
    virtual ~IModelBackend() = default;
    virtual std::string getName() const = 0;
    virtual std::vector<std::string> getSupportedFormats() const = 0;
    virtual Error loadModel(const std::string& path,
                            const Properties& config) = 0;
    virtual Error unloadModel() = 0;
    virtual bool isLoaded() const = 0;
    virtual Result<std::vector<Tensor>> infer(
        const std::vector<Tensor>& inputs) = 0;
    virtual ResourceUsage getResourceUsage() const = 0;
};

// =============================================================================
// Built-in Dummy Backend (for testing / simulation on embedded)
// =============================================================================
class DummyModelBackend : public IModelBackend {
public:
    std::string getName() const override { return "dummy"; }
    std::vector<std::string> getSupportedFormats() const override {
        return {"dummy", "test"};
    }

    Error loadModel(const std::string& path,
                    const Properties& config) override {
        modelPath_ = path;
        loaded_ = true;
        AIPACK_INFO(TAG_RUNTIME, "Dummy backend loaded model: %s", path.c_str());
        return Error::success();
    }

    Error unloadModel() override {
        loaded_ = false;
        modelPath_.clear();
        return Error::success();
    }

    bool isLoaded() const override { return loaded_; }

    Result<std::vector<Tensor>> infer(
            const std::vector<Tensor>& inputs) override {
        if (!loaded_) {
            return Result<std::vector<Tensor>>::failure(
                ErrorCode::RuntimeNotInitialized, "Model not loaded");
        }

        // Generate dummy output based on input shapes
        std::vector<Tensor> outputs;
        for (auto& input : inputs) {
            Tensor output;
            output.name = input.name + "_output";
            output.dtype = input.dtype;
            output.shape = input.shape;
            output.data.resize(input.byteSize(), 0);

            // Fill with simple pattern for testing
            if (output.dtype == DataType::Float32) {
                float* data = output.dataAs<float>();
                for (size_t i = 0; i < output.elementCount(); ++i) {
                    data[i] = 0.5f;  // Neutral output
                }
            }
            outputs.push_back(std::move(output));
        }
        return Result<std::vector<Tensor>>::success(std::move(outputs));
    }

    ResourceUsage getResourceUsage() const override {
        ResourceUsage usage;
        usage.memoryBytes = loaded_ ? 1024 * 1024 : 0;  // 1MB simulated
        return usage;
    }

private:
    bool loaded_ = false;
    std::string modelPath_;
};

// =============================================================================
// Processing Pipeline
// =============================================================================
class ProcessingPipeline : public IPipeline {
public:
    using StageFunc = std::function<Result<std::vector<Tensor>>(
        const std::vector<Tensor>&)>;

    Error configure(const std::vector<PipelineStage>& stages) override {
        stages_ = stages;
        return Error::success();
    }

    void registerStageHandler(const std::string& name, StageFunc func) {
        handlers_[name] = std::move(func);
    }

    Result<std::vector<Tensor>> process(
            const std::vector<Tensor>& inputs) override {
        auto current = inputs;

        for (auto& stage : stages_) {
            auto it = handlers_.find(stage.name);
            if (it == handlers_.end()) {
                return Result<std::vector<Tensor>>::failure(
                    ErrorCode::RuntimePipelineError,
                    "No handler for pipeline stage: " + stage.name);
            }

            auto result = it->second(current);
            if (!result.ok()) {
                return Result<std::vector<Tensor>>::failure(
                    ErrorCode::RuntimePipelineError,
                    "Pipeline stage '" + stage.name + "' failed: " +
                    result.error.message);
            }
            current = std::move(*result);
        }

        return Result<std::vector<Tensor>>::success(std::move(current));
    }

    Result<Properties> processNamed(const Properties& inputs) override {
        // Convert named properties through pipeline
        Properties current = inputs;
        // Simplified: pass through for named processing
        return Result<Properties>::success(std::move(current));
    }

    std::vector<std::string> getStageNames() const override {
        std::vector<std::string> names;
        for (auto& s : stages_) names.push_back(s.name);
        return names;
    }

private:
    std::vector<PipelineStage> stages_;
    std::unordered_map<std::string, StageFunc> handlers_;
};

// =============================================================================
// Resource Manager
// Tracks and manages memory/compute resources across all packs
// =============================================================================
class ResourceManager {
public:
    ResourceManager() {
#ifdef AIPACK_MAX_MEMORY_MB
        maxMemoryBytes_ = static_cast<size_t>(AIPACK_MAX_MEMORY_MB) * 1024 * 1024;
#endif
    }

    void setMaxMemory(size_t bytes) { maxMemoryBytes_ = bytes; }
    size_t getMaxMemory() const { return maxMemoryBytes_; }

    Error allocate(const std::string& packId, size_t bytes) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (maxMemoryBytes_ > 0 &&
            (totalUsed_ + bytes) > maxMemoryBytes_) {
            AIPACK_WARN(TAG_RUNTIME,
                "Memory allocation denied for '%s': would exceed budget "
                "(%zu + %zu > %zu)",
                packId.c_str(), totalUsed_, bytes, maxMemoryBytes_);
            return Error::make(ErrorCode::RuntimeOutOfMemory,
                "Memory budget exceeded");
        }

        allocations_[packId] += bytes;
        totalUsed_ += bytes;
        if (totalUsed_ > peakUsed_) peakUsed_ = totalUsed_;

        AIPACK_TRACE(TAG_RUNTIME,
            "Allocated %zu bytes for '%s' (total: %zu/%zu)",
            bytes, packId.c_str(), totalUsed_, maxMemoryBytes_);
        return Error::success();
    }

    void release(const std::string& packId, size_t bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = allocations_.find(packId);
        if (it != allocations_.end()) {
            size_t toRelease = std::min(bytes, it->second);
            it->second -= toRelease;
            totalUsed_ -= toRelease;
            if (it->second == 0) allocations_.erase(it);
        }
    }

    void releaseAll(const std::string& packId) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = allocations_.find(packId);
        if (it != allocations_.end()) {
            totalUsed_ -= it->second;
            allocations_.erase(it);
        }
    }

    ResourceUsage getUsage() const {
        std::lock_guard<std::mutex> lock(mutex_);
        ResourceUsage usage;
        usage.memoryBytes = totalUsed_;
        usage.peakMemoryBytes = peakUsed_;
        return usage;
    }

    ResourceUsage getUsage(const std::string& packId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        ResourceUsage usage;
        auto it = allocations_.find(packId);
        if (it != allocations_.end()) {
            usage.memoryBytes = it->second;
        }
        return usage;
    }

private:
    mutable std::mutex mutex_;
    size_t maxMemoryBytes_ = 0;
    size_t totalUsed_ = 0;
    size_t peakUsed_ = 0;
    std::unordered_map<std::string, size_t> allocations_;
};

// =============================================================================
// AI Runtime - Central inference management
// =============================================================================
class AIRuntime {
public:
    AIRuntime() {
        // Register the dummy backend by default
        registerBackend(std::make_unique<DummyModelBackend>());
    }

    /// Register a model backend
    void registerBackend(std::unique_ptr<IModelBackend> backend) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto name = backend->getName();
        for (auto& fmt : backend->getSupportedFormats()) {
            formatToBackend_[fmt] = name;
        }
        backends_[name] = std::move(backend);
        AIPACK_INFO(TAG_RUNTIME, "Registered backend: %s", name.c_str());
    }

    /// Load a model from a pack
    Error loadModel(const std::string& packId,
                    const ModelInfo& modelInfo,
                    const std::string& basePath) {
        std::lock_guard<std::mutex> lock(mutex_);

        std::string modelPath = basePath + "/" + modelInfo.path;
        std::string key = packId + ":" + modelInfo.name;

        // Find appropriate backend
        auto fmtIt = formatToBackend_.find(modelInfo.format);
        std::string backendName = (fmtIt != formatToBackend_.end())
            ? fmtIt->second : "dummy";

        auto backIt = backends_.find(backendName);
        if (backIt == backends_.end()) {
            return Error::make(ErrorCode::RuntimeModelLoadFailed,
                "No backend for format: " + modelInfo.format);
        }

        // Check resource budget
        auto allocErr = resourceManager_.allocate(packId, modelInfo.sizeBytes);
        if (allocErr) return allocErr;

        // Load model
        auto err = backIt->second->loadModel(modelPath, {});
        if (err) {
            resourceManager_.release(packId, modelInfo.sizeBytes);
            return err;
        }

        loadedModels_[key] = backendName;
        AIPACK_INFO(TAG_RUNTIME, "Loaded model '%s' with backend '%s'",
            key.c_str(), backendName.c_str());

        EventBus::instance().publish(EventType::ModelLoaded, packId,
            "Model loaded: " + modelInfo.name,
            {{"model", modelInfo.name}, {"backend", backendName}});
        return Error::success();
    }

    /// Run inference on a model
    Result<std::vector<Tensor>> infer(const std::string& packId,
                                       const std::string& modelName,
                                       const std::vector<Tensor>& inputs) {
        std::lock_guard<std::mutex> lock(mutex_);

        std::string key = packId + ":" + modelName;
        auto it = loadedModels_.find(key);
        if (it == loadedModels_.end()) {
            return Result<std::vector<Tensor>>::failure(
                ErrorCode::RuntimeModelLoadFailed,
                "Model not loaded: " + key);
        }

        auto backIt = backends_.find(it->second);
        if (backIt == backends_.end()) {
            return Result<std::vector<Tensor>>::failure(
                ErrorCode::RuntimeInferenceFailed,
                "Backend not found: " + it->second);
        }

        EventBus::instance().publish(EventType::InferenceStarted, packId,
            "Inference started: " + modelName);

        auto result = backIt->second->infer(inputs);

        EventBus::instance().publish(
            result.ok() ? EventType::InferenceCompleted
                        : EventType::InferenceFailed,
            packId, "Inference " + std::string(result.ok() ? "completed" : "failed"));

        return result;
    }

    /// Unload a model
    Error unloadModel(const std::string& packId,
                      const std::string& modelName) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string key = packId + ":" + modelName;
        auto it = loadedModels_.find(key);
        if (it == loadedModels_.end()) {
            return Error::make(ErrorCode::PackNotFound,
                "Model not loaded: " + key);
        }

        auto backIt = backends_.find(it->second);
        if (backIt != backends_.end()) {
            backIt->second->unloadModel();
        }

        loadedModels_.erase(it);
        resourceManager_.releaseAll(packId);
        return Error::success();
    }

    /// Create a processing pipeline
    std::shared_ptr<ProcessingPipeline> createPipeline(
            const std::string& packId,
            const std::vector<PipelineStage>& stages) {
        auto pipeline = std::make_shared<ProcessingPipeline>();
        pipeline->configure(stages);
        return pipeline;
    }

    ResourceManager& resourceManager() { return resourceManager_; }
    const ResourceManager& resourceManager() const { return resourceManager_; }

    /// Get list of loaded models
    std::vector<std::string> listLoadedModels() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> models;
        for (auto& [key, _] : loadedModels_) {
            models.push_back(key);
        }
        return models;
    }

    /// Get list of registered backends
    std::vector<std::string> listBackends() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> names;
        for (auto& [name, _] : backends_) {
            names.push_back(name);
        }
        return names;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<IModelBackend>> backends_;
    std::unordered_map<std::string, std::string> formatToBackend_;
    std::unordered_map<std::string, std::string> loadedModels_;
    ResourceManager resourceManager_;
};

} // namespace aipack
