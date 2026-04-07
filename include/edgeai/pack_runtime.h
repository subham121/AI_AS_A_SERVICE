#pragma once

#include <edgeai/pack_abi.h>
#include <edgeai/pack_manifest.h>

#include <filesystem>
#include <memory>
#include <string>

namespace edgeai {

class PackRuntime {
  public:
    PackRuntime() = default;
    ~PackRuntime();

    PackRuntime(const PackRuntime&) = delete;
    PackRuntime& operator=(const PackRuntime&) = delete;
    PackRuntime(PackRuntime&&) noexcept;
    PackRuntime& operator=(PackRuntime&&) noexcept;

    void load(const std::filesystem::path& pack_root, const PackManifest& manifest, const std::filesystem::path& state_dir);
    void configure(const std::string& config_json);
    Json::Value predict(const std::string& prompt, const std::string& options_json);
    void unload();
    bool loaded() const { return handle_ != nullptr; }

  private:
    void* handle_ = nullptr;
    EdgeAIPackInstanceV1* instance_ = nullptr;
};

}  // namespace edgeai
