#pragma once

#include <json/json.h>

#include <filesystem>
#include <string>

namespace edgeai {

class HttpClient {
  public:
    Json::Value getJson(const std::string& url) const;
    Json::Value postJson(const std::string& url, const Json::Value& payload) const;
    void downloadToFile(const std::string& url, const std::filesystem::path& destination) const;
    std::string urlEncode(const std::string& value) const;
};

}  // namespace edgeai
