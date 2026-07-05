#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace inst::offline::dbupdate
{
    struct CheckResult {
        bool success = false;
        bool updateAvailable = false;
        std::string localVersion;
        std::string remoteVersion;
        std::string error;
    };

    struct ApplyResult {
        bool success = false;
        bool updated = false;
        std::string version;
        std::string error;
    };

    using ProgressCallback = std::function<void(const std::string& stage, double percent)>;

    std::string GetInstalledVersion();
    bool HasInstalledPacks();
    CheckResult CheckForUpdate(const std::string& manifestUrl);
    ApplyResult ApplyUpdate(const std::string& manifestUrl, bool force = false, ProgressCallback progress = {});
    void ResetStartupCheckState();
    void SetStartupCheckResult(const CheckResult& result);
    bool TryGetStartupCheckResult(CheckResult& outResult);
}
