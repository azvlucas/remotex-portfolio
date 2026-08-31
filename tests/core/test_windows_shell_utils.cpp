// RemoteX - tests/core/test_windows_shell_utils.cpp

#include <gtest/gtest.h>

#include "core/windows_shell_utils.h"

#include <filesystem>
#include <algorithm>
#include <cwctype>

using namespace remotex::core;
namespace fs = std::filesystem;

namespace {

std::wstring PathKey(const fs::path& path) {
    std::wstring key = path.lexically_normal().wstring();
    std::replace(key.begin(), key.end(), L'/', L'\\');
    while (key.size() > 3 && !key.empty() && key.back() == L'\\') {
        key.pop_back();
    }
    std::transform(key.begin(), key.end(), key.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return key;
}

} // namespace

TEST(WindowsShellUtils, RemapsSystemProfileDocumentsIntoUserProfile) {
    const fs::path resolved = LR"(C:\Windows\System32\config\systemprofile\Documents)";
    const fs::path system_profile = LR"(C:\Windows\System32\config\systemprofile)";
    const fs::path user_profile = LR"(C:\Users\Alice)";

    EXPECT_EQ(PathKey(detail::RemapSystemProfilePathToUserProfile(resolved, system_profile, user_profile)),
              PathKey(fs::path(LR"(C:\Users\Alice\Documents)")));
}

TEST(WindowsShellUtils, RemapsExactSystemProfileRootToUserProfileRoot) {
    const fs::path resolved = LR"(C:\Windows\System32\config\systemprofile)";
    const fs::path system_profile = LR"(C:\Windows\System32\config\systemprofile)";
    const fs::path user_profile = LR"(C:\Users\Alice)";

    EXPECT_EQ(PathKey(detail::RemapSystemProfilePathToUserProfile(resolved, system_profile, user_profile)),
              PathKey(user_profile));
}

TEST(WindowsShellUtils, LeavesNonSystemProfilePathUntouched) {
    const fs::path resolved = LR"(C:\Users\Alice\Documents)";
    const fs::path system_profile = LR"(C:\Windows\System32\config\systemprofile)";
    const fs::path user_profile = LR"(C:\Users\Alice)";

    EXPECT_EQ(PathKey(detail::RemapSystemProfilePathToUserProfile(resolved, system_profile, user_profile)),
              PathKey(resolved));
}

TEST(WindowsShellUtils, TreatsPathComparisonAsCaseInsensitive) {
    const fs::path resolved = LR"(c:/windows/system32/config/systemprofile/Desktop)";
    const fs::path system_profile = LR"(C:\WINDOWS\SYSTEM32\CONFIG\SYSTEMPROFILE)";
    const fs::path user_profile = LR"(C:\Users\Alice)";

    EXPECT_EQ(PathKey(detail::RemapSystemProfilePathToUserProfile(resolved, system_profile, user_profile)),
              PathKey(fs::path(LR"(C:\Users\Alice\Desktop)")));
}
