#include "test_support.h"

#include <edgeai/capability_router.h>
#include <edgeai/fs_utils.h>
#include <edgeai/pack_manager.h>

#include <gtest/gtest.h>

#include <memory>

namespace {

using edgeai::testsupport::TempDir;
using edgeai::testsupport::TestCatalogServer;
using edgeai::testsupport::createNextWordPackRoot;
using edgeai::testsupport::createTarGzBundle;
using edgeai::testsupport::makeCatalogSeed;
using edgeai::testsupport::makeDefaultDeviceCapability;

class CapabilityRouterTest : public ::testing::Test {
  protected:
    TempDir temp_dir_;
    TestCatalogServer server_{temp_dir_.path() / "catalog"};
    Json::Value device_capability_ = makeDefaultDeviceCapability();
    std::unique_ptr<edgeai::PackManager> manager_;

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
        ASSERT_EQ(manager_->initialize()["status"].asString(), "ok");
    }
};

TEST_F(CapabilityRouterTest, RoutesToCloudCandidatesWhenNoLocalPackIsInstalled) {
    edgeai::CapabilityRouter router(*manager_, server_.baseUrl(), device_capability_);

    const auto response = router.routeUserRequest("alice", R"({"skill":"next word prediction"})", Json::Value());
    EXPECT_EQ(response["status"].asString(), "ok");
    EXPECT_EQ(response["skill"].asString(), "next word prediction");
    EXPECT_EQ(response["capability"].asString(), "next_word_prediction");
    EXPECT_EQ(response["source"].asString(), "cloud");
    EXPECT_EQ(response["count"].asInt(), 1);
}

TEST_F(CapabilityRouterTest, FallsBackToCachedCapabilitiesWhenRemoteCapabilityLookupFails) {
    ASSERT_EQ(manager_->installPack("alice", "next_word_pack", true)["status"].asString(), "ok");
    ASSERT_EQ(manager_->enablePack("alice", "next_word_pack")["status"].asString(), "ok");

    edgeai::CapabilityRouter router(*manager_, "http://127.0.0.1:1/apiv1", device_capability_);
    const auto response = router.routeUserRequest("alice", "next word prediction", Json::Value());

    EXPECT_EQ(response["status"].asString(), "ok");
    EXPECT_EQ(response["source"].asString(), "local");
    EXPECT_EQ(response["pack_id"].asString(), "next_word_pack");
    EXPECT_EQ(response["prepare_result"]["status"].asString(), "ready");
}

}  // namespace
