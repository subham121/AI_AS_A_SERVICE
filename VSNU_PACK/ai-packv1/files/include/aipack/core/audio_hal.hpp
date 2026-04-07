// =============================================================================
// AI Packs System - Audio Output HAL  (PulseAudio backend)
//
// Provides PulseAudioOutput : public IAudioOutput
//
// This is the only file in the core that includes PulseAudio headers.
// All other code that needs audio playback depends solely on IAudioOutput.
//
// Include this header only in translation units (or inline headers) that
// actually perform playback.  Pack code should depend on IAudioOutput only.
// =============================================================================
#pragma once

#include "interfaces.hpp"  // IAudioOutput, AudioBuffer, Error
#include "logger.hpp"       // AIPACK_WARN

#include <pulse/error.h>
#include <pulse/simple.h>

#include <cstdint>
#include <string>
#include <vector>

namespace aipack {

// =============================================================================
// PulseAudioInput  —  IAudioInput backed by libpulse-simple
// =============================================================================
class PulseAudioInput : public IAudioInput {
public:
    PulseAudioInput() = default;

    ~PulseAudioInput() override {
        if (pa_) {
            pa_simple_free(pa_);
            pa_ = nullptr;
        }
    }

    Error open(uint32_t sampleRate,
               uint16_t channels,
               const std::string& appName = "aipack") override
    {
        if (pa_) {
            pa_simple_free(pa_);
            pa_ = nullptr;
        }

        pa_sample_spec ss{};
        ss.format   = PA_SAMPLE_S16LE;
        ss.rate     = sampleRate;
        ss.channels = static_cast<uint8_t>(channels);

        int err = 0;
        pa_ = pa_simple_new(
            nullptr,             // default server
            appName.c_str(),
            PA_STREAM_RECORD,
            nullptr,             // default source (microphone)
            "stt",               // stream description
            &ss,
            nullptr,             // default channel map
            nullptr,             // default buffering attributes
            &err
        );

        if (!pa_) {
            return Error{ErrorCode::InternalError,
                         std::string("PulseAudio input open failed: ") + pa_strerror(err)};
        }

        sampleRate_ = sampleRate;
        channels_   = channels;
        return {};
    }

    /// Read exactly frameCount s16le samples (blocking).
    Error read(int16_t* out, size_t frameCount) override {
        if (!pa_) {
            return Error{ErrorCode::RuntimeNotInitialized,
                         "PulseAudioInput: stream not open"};
        }
        int err = 0;
        if (pa_simple_read(pa_, out, frameCount * sizeof(int16_t), &err) < 0) {
            return Error{ErrorCode::IOError,
                         std::string("pa_simple_read failed: ") + pa_strerror(err)};
        }
        return {};
    }

    Error close() override {
        if (!pa_) return {};
        pa_simple_free(pa_);
        pa_ = nullptr;
        return {};
    }

    bool isOpen() const override { return pa_ != nullptr; }

private:
    pa_simple* pa_       = nullptr;
    uint32_t   sampleRate_ = 0;
    uint16_t   channels_   = 0;
};

// =============================================================================
// Factory helper — returns a heap-allocated IAudioInput for the current
// platform.
// =============================================================================
inline std::unique_ptr<IAudioInput> makeAudioInput() {
    return std::make_unique<PulseAudioInput>();
}

// =============================================================================
// PulseAudioOutput  —  IAudioOutput backed by libpulse-simple
// =============================================================================
class PulseAudioOutput : public IAudioOutput {
public:
    PulseAudioOutput() = default;

    ~PulseAudioOutput() override {
        if (pa_) {
            pa_simple_free(pa_);
            pa_ = nullptr;
        }
    }

    // ── IAudioOutput ──────────────────────────────────────────────────────────

    Error open(uint32_t sampleRate,
               uint16_t channels,
               const std::string& appName = "aipack") override
    {
        if (pa_) {
            // already open — close and reopen with new params
            pa_simple_free(pa_);
            pa_ = nullptr;
        }

        pa_sample_spec ss{};
        ss.format   = PA_SAMPLE_S16LE;
        ss.rate     = sampleRate;
        ss.channels = static_cast<uint8_t>(channels);

        int err = 0;
        pa_ = pa_simple_new(
            nullptr,             // default server
            appName.c_str(),     // application name (shown in PulseAudio mixer)
            PA_STREAM_PLAYBACK,
            nullptr,             // default sink
            "tts",               // stream description
            &ss,
            nullptr,             // default channel map
            nullptr,             // default buffering attributes
            &err
        );

        if (!pa_) {
            return Error{ErrorCode::InternalError,
                         std::string("PulseAudio open failed: ") + pa_strerror(err)};
        }

        sampleRate_ = sampleRate;
        channels_   = channels;
        return {};  // success
    }

    Error play(const AudioBuffer& buffer) override {
        if (buffer.samples.empty()) return {};

        // Convert float32 PCM → interleaved s16le
        std::vector<int16_t> pcm;
        pcm.reserve(buffer.samples.size());
        for (float s : buffer.samples) {
            if (s >  1.0f) s =  1.0f;
            if (s < -1.0f) s = -1.0f;
            pcm.push_back(static_cast<int16_t>(s * 32767.0f));
        }

        return playRaw(pcm.data(), pcm.size());
    }

    Error playRaw(const int16_t* samples, size_t count) override {
        if (!pa_) {
            return Error{ErrorCode::RuntimeNotInitialized,
                         "PulseAudioOutput: stream not open"};
        }
        if (count == 0) return {};

        int err = 0;
        if (pa_simple_write(pa_, samples,
                            count * sizeof(int16_t), &err) < 0) {
            return Error{ErrorCode::IOError,
                         std::string("pa_simple_write failed: ") + pa_strerror(err)};
        }
        // Block until PulseAudio has rendered all buffered samples.
        // Without this drain, pa_simple_write returns as soon as data is
        // copied into the PA buffer — the audio is still playing in the
        // background, making it impossible to know when speech has finished.
        return drain();
    }

    /// Block until all buffered samples have been rendered by the hardware.
    Error drain() override {
        if (!pa_) return {};
        int err = 0;
        if (pa_simple_drain(pa_, &err) < 0) {
            return Error{ErrorCode::IOError,
                         std::string("pa_simple_drain failed: ") + pa_strerror(err)};
        }
        return {};
    }

    Error close() override {
        if (!pa_) return {};
        drain();   // flush remaining audio before tearing down the stream
        pa_simple_free(pa_);
        pa_ = nullptr;
        return {};
    }

    bool isOpen() const override { return pa_ != nullptr; }

private:
    pa_simple* pa_      = nullptr;
    uint32_t sampleRate_ = 0;
    uint16_t channels_   = 0;
};

// =============================================================================
// Factory helper — returns a heap-allocated IAudioOutput for the current
// platform.  Add additional backends here as needed (ALSA, null-sink, …).
// =============================================================================
inline std::unique_ptr<IAudioOutput> makeAudioOutput() {
    return std::make_unique<PulseAudioOutput>();
}

} // namespace aipack
