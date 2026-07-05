#pragma once

#include <string>

namespace inst::util {
    bool HasLegacyAuthSupport();
    std::string ComputeHauthFromUrl(const std::string& requestUrl);
    std::string ComputeUauthFromUrl(const std::string& requestUrl);
    std::string ComputeUauthFromUrl(const std::string& requestUrl, const std::string& basicAuthUser, const std::string& basicAuthPass);
}

