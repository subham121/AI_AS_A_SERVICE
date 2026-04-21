#pragma once

#include <json/json.h>

#include <filesystem>
#include <string>

namespace edgeai::testsupport {

class TempDir {
  public:
    TempDir();
    ~TempDir();

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
};

class TestCatalogServer {
  public:
    explicit TestCatalogServer(std::filesystem::path root_dir);
    ~TestCatalogServer();

    TestCatalogServer(const TestCatalogServer&) = delete;
    TestCatalogServer& operator=(const TestCatalogServer&) = delete;

    const std::filesystem::path& root() const { return root_dir_; }
    int port() const { return port_; }
    std::string baseUrl() const;
    std::string bundleUrl(const std::string& file_name) const;
    void writeSeed(const Json::Value& seed) const;
    void start();
    void stop();

  private:
    void writeServerScript() const;
    void waitUntilReady() const;

    std::filesystem::path root_dir_;
    std::filesystem::path script_path_;
    std::filesystem::path data_path_;
    int port_;
    int pid_ = -1;
};

std::filesystem::path projectRoot();
std::filesystem::path nextWordPackLibrary();
std::string pythonExecutable();
Json::Value makeDefaultDeviceCapability();
Json::Value readJsonFile(const std::filesystem::path& path);
void writeJsonFile(const std::filesystem::path& path, const Json::Value& value);
std::filesystem::path createNextWordPackRoot(const std::filesystem::path& target_root);
std::filesystem::path createTarGzBundle(const std::filesystem::path& source_dir,
                                        const std::filesystem::path& archive_path);
Json::Value makePackServerDetails(const std::string& pack_url, const std::string& md5_checksum);
Json::Value makeCompatiblePackResponse(const std::string& pack_url);
Json::Value makeCatalogSeed(const std::string& pack_url, const std::string& md5_checksum);

}  // namespace edgeai::testsupport
