#include "test_support.h"

#include <edgeai/pack_abi.h>
#include <edgeai/pack_manifest.h>
#include <edgeai/pack_runtime.h>

#include <gtest/gtest.h>

namespace {

using edgeai::testsupport::TempDir;
using edgeai::testsupport::createNextWordPackRoot;

TEST(PackRuntimeTest, LoadsConfiguresPredictsAndUnloadsRealPlugin) {
    TempDir temp_dir;
    const auto pack_root = createNextWordPackRoot(temp_dir.path() / "pack");
    const auto manifest = edgeai::manifestFromFile(pack_root / "manifest.json");

    EXPECT_EQ(edgeai::PackRuntime::readAbiVersion(pack_root, manifest), EDGEAI_PACK_ABI_V1);

    edgeai::PackRuntime runtime;
    runtime.load(pack_root, manifest, temp_dir.path() / "state");
    ASSERT_TRUE(runtime.loaded());

    EXPECT_NO_THROW(runtime.configure(R"({"backend":"fallback"})"));

    const auto response = runtime.predict("hello", "{}");
    EXPECT_TRUE(response["result"].isString());
    EXPECT_FALSE(response["result"].asString().empty());
    EXPECT_TRUE(response["metadata"].isObject());
    EXPECT_TRUE(response["metadata"].isMember("model"));

    runtime.unload();
    EXPECT_FALSE(runtime.loaded());
}

}  // namespace
