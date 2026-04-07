// =============================================================================
// AI Packs System - Plugin Interface Contracts
// Abstract interfaces that all AI Pack plugins must implement
// =============================================================================
#pragma once

#include "types.hpp"
#include "manifest.hpp"
#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <functional>

namespace aipack {

// =============================================================================
// Base Plugin Interface - All plugins must implement this
// =============================================================================
class IPlugin {
public:
    virtual ~IPlugin() = default;

    /// Get the unique identifier for this plugin
    virtual const std::string& getId() const = 0;

    /// Get the plugin version
    virtual Version getVersion() const = 0;

    /// Initialize the plugin with configuration
    virtual Error initialize(const Properties& config) = 0;

    /// Shutdown the plugin and release resources
    virtual Error shutdown() = 0;

    /// Get the current state
    virtual PackState getState() const = 0;

    /// Get resource usage
    virtual ResourceUsage getResourceUsage() const = 0;

    /// Health check
    virtual bool isHealthy() const = 0;

    /// Get supported capabilities
    virtual std::vector<std::string> getCapabilities() const = 0;
};

// =============================================================================
// AI Model Interface - For model loading and inference
// =============================================================================
class IModel {
public:
    virtual ~IModel() = default;

    /// Load model from file
    virtual Error load(const std::string& modelPath,
                       const Properties& config = {}) = 0;

    /// Unload model and free resources
    virtual Error unload() = 0;

    /// Check if model is loaded
    virtual bool isLoaded() const = 0;

    /// Run inference
    virtual Result<std::vector<Tensor>> infer(
        const std::vector<Tensor>& inputs) = 0;

    /// Get model metadata
    virtual Properties getMetadata() const = 0;

    /// Get input specifications
    virtual std::vector<ModelInfo::IOSpec> getInputSpecs() const = 0;

    /// Get output specifications
    virtual std::vector<ModelInfo::IOSpec> getOutputSpecs() const = 0;
};

// =============================================================================
// Pipeline Interface - For multi-stage processing
// =============================================================================
class IPipeline {
public:
    virtual ~IPipeline() = default;

    /// Configure the pipeline with stages
    virtual Error configure(const std::vector<PipelineStage>& stages) = 0;

    /// Process data through the pipeline
    virtual Result<std::vector<Tensor>> process(
        const std::vector<Tensor>& inputs) = 0;

    /// Process with named I/O
    virtual Result<Properties> processNamed(
        const Properties& inputs) = 0;

    /// Get pipeline stage names
    virtual std::vector<std::string> getStageNames() const = 0;
};

// =============================================================================
// TTS Service Interface - Text-to-Speech
// =============================================================================
class ITTSService {
public:
    virtual ~ITTSService() = default;

    /// Synthesize text to audio
    virtual Result<AudioBuffer> synthesize(
        const std::string& text,
        const Properties& options = {}) = 0;

    /// Get available voices
    virtual std::vector<std::string> getAvailableVoices() const = 0;

    /// Set active voice
    virtual Error setVoice(const std::string& voiceId) = 0;

    /// Get supported languages
    virtual std::vector<std::string> getSupportedLanguages() const = 0;

    /// Set speech parameters
    virtual Error setParameter(const std::string& name,
                               const std::string& value) = 0;
};

// =============================================================================
// STT Service Interface - Speech-to-Text
// =============================================================================
class ISTTService {
public:
    virtual ~ISTTService() = default;

    /// Transcription result
    struct TranscriptionResult {
        std::string text;
        float confidence = 0.0f;
        std::string language;

        struct WordInfo {
            std::string word;
            float startTime = 0.0f;
            float endTime = 0.0f;
            float confidence = 0.0f;
        };
        std::vector<WordInfo> words;
    };

    /// Transcribe audio buffer
    virtual Result<TranscriptionResult> transcribe(
        const AudioBuffer& audio,
        const Properties& options = {}) = 0;

    /// Start streaming transcription
    virtual Error startStreaming(
        std::function<void(const TranscriptionResult&)> callback) = 0;

    /// Feed audio chunk for streaming
    virtual Error feedAudio(const float* samples, size_t count) = 0;

    /// Stop streaming transcription
    virtual Error stopStreaming() = 0;

    /// Get supported languages
    virtual std::vector<std::string> getSupportedLanguages() const = 0;
};

// =============================================================================
// NLP Service Interface - Natural Language Processing
// =============================================================================
class INLPService {
public:
    virtual ~INLPService() = default;

    struct NLPResult {
        std::string text;
        float score = 0.0f;
        Properties attributes;
    };

    /// Text classification
    virtual Result<std::vector<NLPResult>> classify(
        const std::string& text,
        const Properties& options = {}) = 0;

    /// Named entity recognition
    virtual Result<std::vector<NLPResult>> extractEntities(
        const std::string& text) = 0;

    /// Text embedding
    virtual Result<std::vector<float>> embed(
        const std::string& text) = 0;

    /// Sentiment analysis
    virtual Result<NLPResult> analyzeSentiment(
        const std::string& text) = 0;

    /// Text generation / completion
    virtual Result<std::string> generate(
        const std::string& prompt,
        const Properties& options = {}) = 0;
};

// =============================================================================
// Vision Service Interface
// =============================================================================
class IVisionService {
public:
    virtual ~IVisionService() = default;

    struct Detection {
        std::string label;
        float confidence = 0.0f;
        float x = 0, y = 0, width = 0, height = 0; // Bounding box
        Properties attributes;
    };

    /// Classify an image
    virtual Result<std::vector<Detection>> classify(
        const Tensor& image,
        const Properties& options = {}) = 0;

    /// Detect objects in image
    virtual Result<std::vector<Detection>> detect(
        const Tensor& image,
        float confidenceThreshold = 0.5f) = 0;

    /// Segment image
    virtual Result<Tensor> segment(
        const Tensor& image,
        const Properties& options = {}) = 0;
};

// =============================================================================
// Agent Interface
// =============================================================================
class IAgent {
public:
    virtual ~IAgent() = default;

    struct AgentMessage {
        std::string role;       // "user", "assistant", "system", "tool"
        std::string content;
        Properties metadata;
    };

    struct AgentResponse {
        std::string content;
        std::vector<std::string> toolCalls;
        Properties metadata;
        bool complete = true;
    };

    /// Process a message and generate response
    virtual Result<AgentResponse> process(
        const AgentMessage& message,
        const std::vector<AgentMessage>& history = {}) = 0;

    /// Get available tools
    virtual std::vector<AgentToolDef> getTools() const = 0;

    /// Execute a tool
    virtual Result<std::string> executeTool(
        const std::string& toolName,
        const Properties& args) = 0;

    /// Reset agent state
    virtual Error reset() = 0;
};

// =============================================================================
// Skill Interface - Reusable capability
// =============================================================================
class ISkill {
public:
    virtual ~ISkill() = default;

    virtual const std::string& getName() const = 0;
    virtual std::string getDescription() const = 0;

    /// Execute the skill with given input
    virtual Result<Properties> execute(const Properties& input) = 0;

    /// Check if the skill can handle the given input
    virtual bool canHandle(const Properties& input) const = 0;
};

// =============================================================================
// Knowledge Base Interface
// =============================================================================
class IKnowledgeBase {
public:
    virtual ~IKnowledgeBase() = default;

    struct QueryResult {
        std::string content;
        float relevance = 0.0f;
        Properties metadata;
    };

    /// Query the knowledge base
    virtual Result<std::vector<QueryResult>> query(
        const std::string& question,
        uint32_t maxResults = 5) = 0;

    /// Add content to knowledge base
    virtual Error addContent(const std::string& content,
                             const Properties& metadata = {}) = 0;

    /// Get knowledge base statistics
    virtual Properties getStats() const = 0;
};

// =============================================================================
// Audio Input HAL Interface
// Abstracts microphone / line-in capture so callers are decoupled from any
// specific backend (PulseAudio, ALSA, JACK, CoreAudio, WASAPI, …).
// =============================================================================
class IAudioInput {
public:
    virtual ~IAudioInput() = default;

    /// Open a capture stream.
    /// @param sampleRate  samples per second (e.g. 16000)
    /// @param channels    number of channels (1 = mono)
    /// @param appName     human-readable application name shown in the mixer
    /// @return Error (empty on success)
    virtual Error open(uint32_t sampleRate,
                       uint16_t channels,
                       const std::string& appName = "aipack") = 0;

    /// Read exactly `frameCount` interleaved s16le samples into `out`.
    /// Blocks until the requested number of samples is available.
    /// @param out        destination buffer; must hold at least frameCount elements
    /// @param frameCount number of *samples* to read (not bytes)
    /// @return Error (empty on success)
    virtual Error read(int16_t* out, size_t frameCount) = 0;

    /// Drain any internal buffer and close the stream.
    virtual Error close() = 0;

    /// True if the stream is currently open.
    virtual bool isOpen() const = 0;
};

// =============================================================================
// Audio Output HAL Interface
// Abstracts audio playback so callers are decoupled from any specific backend
// (PulseAudio, ALSA, JACK, CoreAudio, WASAPI, null-sink, …).
// =============================================================================
class IAudioOutput {
public:
    virtual ~IAudioOutput() = default;

    /// Open a playback stream for the given buffer format.
    /// Must be called before play().
    /// @param sampleRate  samples per second (e.g. 22050, 44100)
    /// @param channels    number of interleaved channels (1 = mono, 2 = stereo)
    /// @param appName     human-readable application name shown in the mixer
    /// @return Error (empty on success)
    virtual Error open(uint32_t sampleRate,
                       uint16_t channels,
                       const std::string& appName = "aipack") = 0;

    /// Play a fully-synthesised AudioBuffer (float32 PCM, any layout).
    /// Blocks until all audio has been rendered by the hardware (i.e. drains
    /// the backend buffer before returning).  Callers may rely on this to
    /// determine when it is safe to re-enable the microphone.
    virtual Error play(const AudioBuffer& buffer) = 0;

    /// Play raw interleaved s16le PCM data directly (no float conversion).
    /// Also blocks until playback is complete (drains before returning).
    virtual Error playRaw(const int16_t* samples, size_t count) = 0;

    /// Block until all previously written audio has been rendered by the
    /// hardware.  Useful when batching multiple writes before waiting.
    virtual Error drain() = 0;

    /// Drain any buffered audio then close the stream.
    virtual Error close() = 0;

    /// True if the stream is currently open.
    virtual bool isOpen() const = 0;
};

// =============================================================================
// Plugin Factory Function Types
// =============================================================================

/// Entry point function that every pack shared library must export
using PluginFactoryFunc = IPlugin* (*)();

/// Destroy function for cleanup
using PluginDestroyFunc = void (*)(IPlugin*);

/// ABI version check
using AbiVersionFunc = uint32_t (*)();

// Current ABI version
constexpr uint32_t AIPACK_ABI_VERSION = 1;

} // namespace aipack

// =============================================================================
// Macros for declaring pack plugins
// =============================================================================

/// Declare a plugin implementation in a shared library
#define AIPACK_DECLARE_PLUGIN(PluginClass)                                 \
    extern "C" {                                                           \
        aipack::IPlugin* aipack_create_plugin() {                          \
            return new PluginClass();                                       \
        }                                                                  \
        void aipack_destroy_plugin(aipack::IPlugin* p) {                   \
            delete p;                                                      \
        }                                                                  \
        uint32_t aipack_abi_version() {                                    \
            return aipack::AIPACK_ABI_VERSION;                             \
        }                                                                  \
    }
