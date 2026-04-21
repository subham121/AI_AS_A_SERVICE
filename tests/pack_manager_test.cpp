#include "test_support.h"

#include <edgeai/fs_utils.h>
#include <edgeai/pack_manager.h>

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace {

using edgeai::testsupport::TempDir;
using edgeai::testsupport::TestCatalogServer;
using edgeai::testsupport::createNextWordPackRoot;
using edgeai::testsupport::createTarGzBundle;
using edgeai::testsupport::makeCatalogSeed;
using edgeai::testsupport::makeDefaultDeviceCapability;
using edgeai::testsupport::readJsonFile;
using edgeai::testsupport::writeJsonFile;

class RecordingSink : public edgeai::PackEventSink {
  public:
    void publish(const Json::Value& event) override {
        events.push_back(event);
    }

    std::vector<Json::Value> events;
};

class PackManagerTest : public ::testing::Test {
  protected:
    TempDir temp_dir_;
    TestCatalogServer server_{temp_dir_.path() / "catalog"};
    Json::Value device_capability_ = makeDefaultDeviceCapability();
    std::unique_ptr<edgeai::PackManager> manager_;
    RecordingSink sink_;

    void SetUp() override {
        const auto pack_root = createNextWordPackRoot(temp_dir_.path() / "bundle_src");
        const auto bundle_path = server_.root() / "bundles" / "next_word_pack.tar.gz";
        createTarGzBundle(pack_root, bundle_path);
        server_.writeSeed(makeCatalogSeed(server_.bundleUrl("next_word_pack.tar.gz"), edgeai::computeMd5(bundle_path)));
        server_.start();

        manager_ = std::make_unique<edgeai::PackManager>(
            temp_dir_.path() / "state",
            temp_dir_.path() / "staging",
            temp_dir_.path() / "installed",
            server_.baseUrl(),
            device_capability_);
        manager_->setEventSink(&sink_);
        ASSERT_EQ(manager_->initialize()["status"].asString(), "ok");
    }

    std::filesystem::path registryPath() const {
        return temp_dir_.path() / "state" / "intent_and_pack_registry.json";
    }

    std::filesystem::path rollbackPath() const {
        return temp_dir_.path() / "state" / "rollback_registry.json";
    }
};

TEST_F(PackManagerTest, QueryPacksFetchesAndCachesCompatiblePackDetails) {
    const auto response = manager_->queryPacks("next_word_prediction", device_capability_);

    EXPECT_EQ(response["count"].asInt(), 1);
    EXPECT_EQ(response["details_cached_count"].asInt(), 1);
    ASSERT_TRUE(response["pack_details"].isMember("next_word_pack"));
    EXPECT_EQ(response["pack_details"]["next_word_pack"]["pack_id"].asString(), "next_word_pack");

    const auto registry = readJsonFile(registryPath());
    EXPECT_TRUE(registry["pack_server_cache"]["compatible_packs"].isMember("next_word_prediction"));
    EXPECT_TRUE(registry["pack_server_cache"]["pack_details"].isMember("next_word_pack"));
}

TEST_F(PackManagerTest, ExecutesInstallEnableLoadInvokeUnloadDisableUninstallLifecycleInOrder) {
    ASSERT_EQ(manager_->installPack("alice", "next_word_pack", true)["status"].asString(), "ok");
    ASSERT_EQ(manager_->enablePack("alice", "next_word_pack")["status"].asString(), "ok");
    ASSERT_EQ(manager_->loadPack("alice", "next_word_pack")["status"].asString(), "ok");

    const auto invoke = manager_->invoke("alice", "next_word_pack", "hello", "{}");
    EXPECT_EQ(invoke["status"].asString(), "ok");
    EXPECT_TRUE(invoke["result"].isString());
    EXPECT_FALSE(invoke["result"].asString().empty());

    ASSERT_EQ(manager_->unloadPack("alice", "next_word_pack")["status"].asString(), "ok");
    ASSERT_EQ(manager_->disablePack("alice", "next_word_pack")["status"].asString(), "ok");
    ASSERT_EQ(manager_->uninstallPack("alice", "next_word_pack", true)["status"].asString(), "ok");

    std::vector<std::string> statuses;
    for (const auto& event : sink_.events) {
        statuses.push_back(event["status"].asString());
    }

    const std::vector<std::string> expected{
        "installed",
        "enabled",
        "loaded",
        "invoked",
        "unloaded",
        "disabled",
        "uninstalled",
    };
    EXPECT_EQ(statuses, expected);

    const auto registry = readJsonFile(registryPath());
    EXPECT_FALSE(registry["packs"].isMember("next_word_pack"));
}

TEST_F(PackManagerTest, InvokeRequiresLoadingPhase) {
    ASSERT_EQ(manager_->installPack("alice", "next_word_pack", true)["status"].asString(), "ok");
    ASSERT_EQ(manager_->enablePack("alice", "next_word_pack")["status"].asString(), "ok");

    const auto invoke = manager_->invoke("alice", "next_word_pack", "hello", "{}");
    EXPECT_EQ(invoke["status"].asString(), "error");
    EXPECT_EQ(invoke["message"].asString(), "Loading Phase should be initiated before usage");
}

TEST_F(PackManagerTest, HandleUserRequestPrefersInstalledLocalPack) {
    ASSERT_EQ(manager_->installPack("alice", "next_word_pack", true)["status"].asString(), "ok");
    ASSERT_EQ(manager_->enablePack("alice", "next_word_pack")["status"].asString(), "ok");

    const auto response = manager_->handleUserRequest("alice", "next word prediction", device_capability_);
    EXPECT_EQ(response["status"].asString(), "ok");
    EXPECT_EQ(response["source"].asString(), "local");
    EXPECT_EQ(response["pack_id"].asString(), "next_word_pack");
    EXPECT_EQ(response["prepare_result"]["status"].asString(), "ready");
}

TEST_F(PackManagerTest, RollbackRestoresPreviousPackVersionMetadata) {
    const auto install = manager_->installPack("alice", "next_word_pack", true);
    ASSERT_EQ(install["status"].asString(), "ok");

    auto registry = readJsonFile(registryPath());
    const auto previous_root = registry["packs"]["next_word_pack"]["pack_root"].asString();
    const auto previous_manifest = registry["packs"]["next_word_pack"]["manifest_path"].asString();

    registry["packs"]["next_word_pack"]["version"] = "2.0.0";
    registry["packs"]["next_word_pack"]["pack_root"] = "/tmp/current-pack-root";
    registry["packs"]["next_word_pack"]["manifest_path"] = "/tmp/current-pack-root/manifest.json";
    writeJsonFile(registryPath(), registry);

    auto rollback = readJsonFile(rollbackPath());
    Json::Value history(Json::objectValue);
    history["version"] = "1.0.0";
    history["pack_root"] = previous_root;
    history["manifest_path"] = previous_manifest;
    rollback["history"]["next_word_pack"].append(history);
    writeJsonFile(rollbackPath(), rollback);

    const auto result = manager_->rollbackPack("alice", "next_word_pack");
    EXPECT_EQ(result["status"].asString(), "ok");

    registry = readJsonFile(registryPath());
    EXPECT_EQ(registry["packs"]["next_word_pack"]["version"].asString(), "1.0.0");
    EXPECT_EQ(registry["packs"]["next_word_pack"]["pack_root"].asString(), previous_root);
    EXPECT_EQ(registry["packs"]["next_word_pack"]["manifest_path"].asString(), previous_manifest);
    EXPECT_EQ(registry["packs"]["next_word_pack"]["status"].asString(), "RolledBack");
}

}  // namespace
