#include "test_support.h"

#include <edgeai/pack_manifest.h>

#include <gtest/gtest.h>

namespace {

using edgeai::testsupport::TempDir;
using edgeai::testsupport::createNextWordPackRoot;
using edgeai::testsupport::projectRoot;

TEST(PackManifestTest, ParsesManifestFromFileAndUsesDefaults) {
    const auto manifest = edgeai::manifestFromFile(projectRoot() / "packs/next_word/manifest.json");

    EXPECT_EQ(manifest.pack_id, "next_word_pack");
    EXPECT_EQ(manifest.name, "Next Word Predictor");
    EXPECT_EQ(manifest.version, "1.0.0");
    EXPECT_EQ(manifest.intent, "next_word_prediction");
    EXPECT_EQ(manifest.entry_library, "lib/libnext_word_pack.so");
}

TEST(PackManifestTest, ThrowsWhenRequiredFieldsAreMissing) {
    Json::Value invalid(Json::objectValue);
    invalid["pack_id"] = "broken";
    invalid["version"] = "1.0.0";
    invalid["intent"] = "broken";
    invalid["entrypoint"]["library"] = "lib/libbroken.so";

    EXPECT_THROW(edgeai::manifestFromJson(invalid), std::runtime_error);
}

TEST(PackManifestTest, LibraryPathResolvesBuiltPluginAndConfigPathUsesManifestSetting) {
    TempDir temp_dir;
    const auto pack_root = createNextWordPackRoot(temp_dir.path() / "pack");
    const auto manifest = edgeai::manifestFromFile(pack_root / "manifest.json");

    const auto library_path = manifest.libraryPath(pack_root);
    EXPECT_TRUE(std::filesystem::exists(library_path));
    EXPECT_EQ(manifest.configPath(pack_root), pack_root / "config/default.json");
}

}  // namespace
