#include <gtest/gtest.h>

#include "host/runtime_paths.h"

#include <filesystem>
#include <system_error>

namespace {

namespace fs = std::filesystem;

using remotex::host::EnsureDirectoryExists;

TEST(RuntimePaths, EnsureDirectoryExistsCreatesNestedDirectories) {
    std::error_code temp_error;
    const fs::path temp_root = fs::temp_directory_path(temp_error);
    ASSERT_FALSE(temp_error);

    const fs::path test_dir = temp_root / "remotex_tests" / "runtime_paths" / "nested";
    std::error_code remove_error;
    fs::remove_all(temp_root / "remotex_tests", remove_error);

    const auto result = EnsureDirectoryExists(test_dir, "test directory");

    EXPECT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.path, test_dir);
    EXPECT_TRUE(fs::exists(test_dir));

    fs::remove_all(temp_root / "remotex_tests", remove_error);
}

TEST(RuntimePaths, EnsureDirectoryExistsRejectsEmptyPath) {
    const auto result = EnsureDirectoryExists({}, "test directory");

    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("path is empty"), std::string::npos);
}

} // namespace
