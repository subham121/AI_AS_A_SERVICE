#pragma once

#include <json/json.h>

#include <filesystem>
#include <memory>
#include <string>

namespace edgeai {

class NextWordEngine {
  public:
    NextWordEngine(std::filesystem::path pack_root,
                   std::filesystem::path state_dir,
                   void (*logger)(int, const char*));
    ~NextWordEngine();

    NextWordEngine(const NextWordEngine&) = delete;
    NextWordEngine& operator=(const NextWordEngine&) = delete;
    NextWordEngine(NextWordEngine&&) noexcept;
    NextWordEngine& operator=(NextWordEngine&&) noexcept;

    void activate(const std::string& activation_json);
    void configure(const std::string& config_json);
    Json::Value predict(const std::string& prompt, const std::string& options_json);
    void deactivate();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace edgeai
