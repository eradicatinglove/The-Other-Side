#pragma once

#include <cstdint>
#include <vector>
#include <string>

namespace inst::offline
{
    struct TitleMetadata {
        std::string name;
        std::string publisher;
        std::string intro;
        std::string description;
        std::uint64_t size = 0;
        std::uint32_t version = 0;
        std::uint32_t releaseDate = 0;
        bool hasSize = false;
        bool hasVersion = false;
        bool hasReleaseDate = false;
        bool isDemo = false;
        bool hasIsDemo = false;
    };

    std::string GetOfflineDbDir();
    void Invalidate();
    bool TryGetMetadata(std::uint64_t baseTitleId, TitleMetadata& outMeta);
    bool HasPackedIcons();
    bool HasIcon(std::uint64_t baseTitleId);
    bool TryGetIconData(std::uint64_t baseTitleId, std::vector<std::uint8_t>& outData);
    bool TryGetIconPath(std::uint64_t baseTitleId, std::string& outPath);
}
