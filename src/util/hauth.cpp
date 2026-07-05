#include "util/hauth.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cctype>
#include <string>

extern "C" {
    bool z9f2(const void* authInput, std::size_t authInputLen, char* outAuthHex, std::size_t outAuthHexCap, std::size_t* outAuthHexLen) __attribute__((weak));
    bool z9f3(const void* authInput, std::size_t authInputLen, char* outAuthHex, std::size_t outAuthHexCap, std::size_t* outAuthHexLen) __attribute__((weak));
}

namespace {
    bool TryBuildHostPart(const std::string& url, std::string& outHostPart)
    {
        outHostPart.clear();
        if (url.empty())
            return false;

        const std::size_t schemeSep = url.find("://");
        if (schemeSep == std::string::npos || schemeSep == 0)
            return false;

        const std::size_t netlocStart = schemeSep + 3;
        if (netlocStart >= url.size())
            return false;

        const std::size_t netlocEnd = url.find_first_of("/?#", netlocStart);
        const std::size_t netlocLen = (netlocEnd == std::string::npos) ? (url.size() - netlocStart) : (netlocEnd - netlocStart);
        if (netlocLen == 0)
            return false;

        std::string scheme = url.substr(0, schemeSep);
        for (char& c : scheme)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        std::string netloc = url.substr(netlocStart, netlocLen);
        const std::size_t atPos = netloc.rfind('@');
        const std::string hostPort = (atPos == std::string::npos) ? netloc : netloc.substr(atPos + 1);
        if (hostPort.empty())
            return false;

        std::string host;
        if (!hostPort.empty() && hostPort[0] == '[') {
            const std::size_t bracketEnd = hostPort.find(']');
            if (bracketEnd == std::string::npos || bracketEnd <= 1)
                return false;
            host = hostPort.substr(1, bracketEnd - 1);
        } else {
            const std::size_t colonPos = hostPort.find(':');
            host = (colonPos == std::string::npos) ? hostPort : hostPort.substr(0, colonPos);
        }

        if (host.empty())
            return false;
        for (char& c : host)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        outHostPart = scheme + "://" + host;
        return true;
    }

    bool TryBuildUauthPart(const std::string& url, const std::string& basicAuthUserInfo, std::string& outUauthPart)
    {
        outUauthPart.clear();
        if (url.empty())
            return false;

        const std::size_t schemeSep = url.find("://");
        if (schemeSep == std::string::npos || schemeSep == 0)
            return false;

        const std::size_t netlocStart = schemeSep + 3;
        if (netlocStart >= url.size())
            return false;

        const std::size_t netlocEnd = url.find_first_of("/?#", netlocStart);
        const std::size_t netlocLen = (netlocEnd == std::string::npos) ? (url.size() - netlocStart) : (netlocEnd - netlocStart);
        if (netlocLen == 0)
            return false;

        std::string scheme = url.substr(0, schemeSep);
        for (char& c : scheme)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        const std::string netloc = url.substr(netlocStart, netlocLen);
        const std::size_t atPos = netloc.rfind('@');
        std::string userInfo;
        const std::string hostPort = (atPos == std::string::npos) ? netloc : netloc.substr(atPos + 1);
        if (atPos != std::string::npos)
            userInfo = netloc.substr(0, atPos);
        else
            userInfo = basicAuthUserInfo;

        if (hostPort.empty())
            return false;

        std::string host;
        std::string port;
        if (!hostPort.empty() && hostPort[0] == '[') {
            const std::size_t bracketEnd = hostPort.find(']');
            if (bracketEnd == std::string::npos || bracketEnd <= 1)
                return false;
            host = hostPort.substr(1, bracketEnd - 1);
            if ((bracketEnd + 1) < hostPort.size()) {
                if (hostPort[bracketEnd + 1] != ':')
                    return false;
                port = hostPort.substr(bracketEnd + 2);
            }
        } else {
            const std::size_t colonPos = hostPort.rfind(':');
            if (colonPos == std::string::npos) {
                host = hostPort;
            } else {
                const std::string maybePort = hostPort.substr(colonPos + 1);
                const bool allDigits = !maybePort.empty() &&
                    std::all_of(maybePort.begin(), maybePort.end(), [](unsigned char c) {
                        return std::isdigit(c) != 0;
                    });
                if (allDigits) {
                    host = hostPort.substr(0, colonPos);
                    port = maybePort;
                } else {
                    host = hostPort;
                }
            }
        }
        if (host.empty())
            return false;
        for (char& c : host)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string hostOut = host;
        if (hostOut.find(':') != std::string::npos && (hostOut.empty() || hostOut.front() != '['))
            hostOut = "[" + hostOut + "]";

        std::string path = "/";
        std::string query;
        std::string fragment;
        if (netlocEnd != std::string::npos) {
            if (url[netlocEnd] == '/') {
                const std::size_t pathEnd = url.find_first_of("?#", netlocEnd);
                path = (pathEnd == std::string::npos)
                    ? url.substr(netlocEnd)
                    : url.substr(netlocEnd, pathEnd - netlocEnd);
                if (path.empty())
                    path = "/";
            }

            const std::size_t queryPos = url.find('?', netlocEnd);
            const std::size_t fragmentPos = url.find('#', netlocEnd);
            if (queryPos != std::string::npos && (fragmentPos == std::string::npos || queryPos < fragmentPos)) {
                const std::size_t queryEnd = (fragmentPos == std::string::npos) ? url.size() : fragmentPos;
                query = url.substr(queryPos + 1, queryEnd - (queryPos + 1));
            }
            if (fragmentPos != std::string::npos)
                fragment = url.substr(fragmentPos + 1);
        }

        outUauthPart = scheme + "://";
        if (!userInfo.empty())
            outUauthPart += userInfo + "@";
        outUauthPart += hostOut;
        if (!port.empty())
            outUauthPart += ":" + port;
        outUauthPart += path;
        if (!query.empty())
            outUauthPart += "?" + query;
        if (!fragment.empty())
            outUauthPart += "#" + fragment;
        return true;
    }

    std::string ComputeAuthFromInput(const std::string& authInput, bool useUauthSeed)
    {
        std::string msg = authInput;
        msg.push_back('\0');

        std::array<char, 65> authHex{};
        std::size_t authHexLen = 0;
        const bool loaded = useUauthSeed
            ? (z9f3 != nullptr && z9f3(msg.data(), msg.size(), authHex.data(), authHex.size(), &authHexLen))
            : (z9f2 != nullptr && z9f2(msg.data(), msg.size(), authHex.data(), authHex.size(), &authHexLen));

        if (!loaded || authHexLen == 0 || authHexLen >= authHex.size())
            return "0";
        return std::string(authHex.data(), authHexLen);
    }
}

namespace inst::util {
    bool HasLegacyAuthSupport()
    {
        return z9f2 != nullptr && z9f3 != nullptr;
    }

    std::string ComputeHauthFromUrl(const std::string& requestUrl)
    {
        std::string hostPart;
        if (!TryBuildHostPart(requestUrl, hostPart))
            return "0";

        return ComputeAuthFromInput(hostPart, false);
    }

    std::string ComputeUauthFromUrl(const std::string& requestUrl)
    {
        return ComputeUauthFromUrl(requestUrl, "", "");
    }

    std::string ComputeUauthFromUrl(const std::string& requestUrl, const std::string& basicAuthUser, const std::string& basicAuthPass)
    {
        std::string basicAuthUserInfo;
        if (!basicAuthUser.empty() || !basicAuthPass.empty())
            basicAuthUserInfo = basicAuthUser + ":" + basicAuthPass;

        std::string uauthPart;
        if (!TryBuildUauthPart(requestUrl, basicAuthUserInfo, uauthPart))
            return "0";

        return ComputeAuthFromInput(uauthPart, true);
    }
}
