#include "test_support.h"

#include <edgeai/fs_utils.h>
#include <edgeai/http_client.h>

#include <gtest/gtest.h>

namespace {

using edgeai::testsupport::TempDir;
using edgeai::testsupport::TestCatalogServer;

class HttpClientTest : public ::testing::Test {
  protected:
    TempDir temp_dir_;
    TestCatalogServer server_{temp_dir_.path() / "catalog"};

    void SetUp() override {
        edgeai::writeTextFile(server_.root() / "bundles" / "sample.txt", "downloaded-content");

        Json::Value seed(Json::objectValue);
        seed["capabilities"] = Json::Value(Json::arrayValue);
        seed["capabilities"].append("next_word_prediction");
        seed["compatible"] = Json::Value(Json::objectValue);
        seed["pack_details"] = Json::Value(Json::objectValue);
        server_.writeSeed(seed);
        server_.start();
    }
};

TEST_F(HttpClientTest, SupportsJsonGetPostAndDownloadFlows) {
    edgeai::HttpClient client;

    const auto get_response = client.getJson(server_.baseUrl() + "/getCapabilityList");
    EXPECT_EQ(get_response["count"].asInt(), 1);
    EXPECT_EQ(get_response["capabilities"][0].asString(), "next_word_prediction");

    Json::Value payload(Json::objectValue);
    payload["hello"] = "world";
    const auto post_response = client.postJson(server_.baseUrl() + "/echo", payload);
    EXPECT_EQ(post_response["hello"].asString(), "world");

    const auto destination = temp_dir_.path() / "download" / "sample.txt";
    client.downloadToFile(server_.bundleUrl("sample.txt"), destination);
    EXPECT_EQ(edgeai::readTextFile(destination), "downloaded-content");

    EXPECT_EQ(client.urlEncode("next word+prediction"), "next%20word%2Bprediction");
}

}  // namespace
