#include "util/offline_title_db.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "util/config.hpp"
#include "util/error.hpp"
#include "util/json.hpp"

namespace inst::offline
{
    namespace {
        constexpr std::uintmax_t kMaxMetadataParseBytes = 48ULL * 1024ULL * 1024ULL;
        constexpr std::uintmax_t kMaxIconIndexParseBytes = 8ULL * 1024ULL * 1024ULL;
        constexpr std::uintmax_t kMaxIconPackIndexBytes = 16ULL * 1024ULL * 1024ULL;
        constexpr std::uintmax_t kMaxTitlePackIndexBytes = 32ULL * 1024ULL * 1024ULL;
        constexpr std::uintmax_t kMaxTitlePackStringsBytes = 128ULL * 1024ULL * 1024ULL;
        constexpr std::uint32_t kMaxIconBytes = 16U * 1024U * 1024U;

        constexpr std::array<char, 8> kTitlePackMagic = {'C', 'F', 'T', 'I', 'T', 'L', 'E', '1'};
        constexpr std::uint32_t kTitlePackVersion = 1;
        constexpr std::uint32_t kTitlePackEntrySize = 48;
        constexpr std::uint32_t kTitleFlagHasName = 1U << 0;
        constexpr std::uint32_t kTitleFlagHasPublisher = 1U << 1;
        constexpr std::uint32_t kTitleFlagHasIntro = 1U << 2;
        constexpr std::uint32_t kTitleFlagHasDescription = 1U << 3;
        constexpr std::uint32_t kTitleFlagHasSize = 1U << 4;
        constexpr std::uint32_t kTitleFlagHasVersion = 1U << 5;
        constexpr std::uint32_t kTitleFlagHasReleaseDate = 1U << 6;
        constexpr std::uint32_t kTitleFlagHasIsDemo = 1U << 7;

        struct TitlePackHeader {
            char magic[8];
            std::uint32_t version;
            std::uint32_t entrySize;
            std::uint32_t entryCount;
            std::uint32_t flags;
            std::uint64_t stringsOffset;
        };

        struct TitlePackEntryRecord {
            std::uint64_t titleId;
            std::uint32_t nameOffset;
            std::uint32_t publisherOffset;
            std::uint32_t introOffset;
            std::uint32_t descriptionOffset;
            std::uint64_t size;
            std::uint32_t version;
            std::uint32_t releaseDate;
            std::int32_t isDemo;
            std::uint32_t flags;
        };

        constexpr std::array<char, 8> kIconPackMagic = {'C', 'F', 'I', 'C', 'O', 'N', 'P', '1'};
        constexpr std::uint32_t kIconPackVersion = 1;
        constexpr std::uint32_t kIconPackEntrySize = 32;

        struct IconPackHeader {
            char magic[8];
            std::uint32_t version;
            std::uint32_t entrySize;
            std::uint32_t entryCount;
            std::uint32_t flags;
            std::uint64_t dataOffset;
        };

        struct IconPackEntryRecord {
            std::uint64_t titleId;
            std::uint64_t offset;
            std::uint32_t size;
            char ext[8];
            std::uint32_t reserved;
        };

        static_assert(sizeof(TitlePackHeader) == 32, "Unexpected title pack header size.");
        static_assert(sizeof(TitlePackEntryRecord) == 48, "Unexpected title pack entry size.");
        static_assert(sizeof(IconPackHeader) == 32, "Unexpected icon pack header size.");
        static_assert(sizeof(IconPackEntryRecord) == 32, "Unexpected icon pack entry size.");

        struct PackedIconEntry {
            std::uint64_t offset = 0;
            std::uint32_t size = 0;
            std::string ext;
        };

        std::unordered_map<std::uint64_t, TitleMetadata> g_metadataById;
        bool g_metadataAttempted = false;
        bool g_metadataAvailable = false;

        std::unordered_map<std::uint64_t, PackedIconEntry> g_iconPackEntries;
        bool g_iconPackAttempted = false;
        bool g_iconPackAvailable = false;
        std::string g_iconPackPath;
        std::uint64_t g_iconPackDataOffset = 0;

        // Legacy fallback for folder-based icons.
        std::unordered_map<std::uint64_t, std::string> g_legacyIconExtById;
        bool g_legacyIconIndexAttempted = false;

        std::vector<std::string> GetMetadataBinaryCandidates()
        {
            const std::string base = inst::config::appDir;
            return {
                base + "/offline_db/titles.pack",
                base + "/offline_db/title.pack",
                base + "/titles.pack",
                base + "/artefacts/titles.pack"
            };
        }

        std::vector<std::string> GetMetadataJsonCandidates()
        {
            const std::string base = inst::config::appDir;
            return {
                base + "/offline_db/titles.min.json",
                base + "/offline_db/titles.US.en.min.json",
                base + "/offline_db/titles.US.en.json",
                base + "/titles.min.json",
                base + "/titles.US.en.json",
                base + "/artefacts/titles.US.en.json",
                base + "/offline_db/title.us.en.json",
                base + "/artefacts/title.us.en.json"
            };
        }

        std::vector<std::string> GetIconPackCandidates()
        {
            const std::string base = inst::config::appDir;
            return {
                base + "/offline_db/icons.pack",
                base + "/offline_db/icon.pack",
                base + "/icons.pack",
                base + "/artefacts/icons.pack"
            };
        }

        std::vector<std::string> GetLegacyIconIndexCandidates()
        {
            const std::string base = inst::config::appDir;
            return {
                base + "/offline_db/icons.index.json",
                base + "/offline_db/icon.index.json",
                base + "/icons.index.json",
                base + "/artefacts/icons.index.json"
            };
        }

        std::vector<std::string> GetLegacyIconDirectories()
        {
            const std::string base = inst::config::appDir;
            return {
                base + "/offline_db/icons",
                base + "/offline_icons",
                base + "/artefacts/icons"
            };
        }

        bool FileExists(const std::string& path)
        {
            std::error_code ec;
            return std::filesystem::exists(path, ec) && std::filesystem::is_regular_file(path, ec);
        }

        std::string NormalizeExtension(std::string ext)
        {
            if (!ext.empty() && ext.front() == '.')
                ext.erase(ext.begin());
            std::string out;
            out.reserve(ext.size());
            for (unsigned char c : ext) {
                if (std::isalnum(c))
                    out.push_back(static_cast<char>(std::tolower(c)));
            }
            return out;
        }

        std::string FormatTitleIdHex(std::uint64_t titleId)
        {
            char buf[17] = {0};
            std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(titleId));
            return std::string(buf);
        }

        bool TryParseHexU64(const std::string& text, std::uint64_t& out)
        {
            if (text.empty())
                return false;
            char* end = nullptr;
            unsigned long long parsed = std::strtoull(text.c_str(), &end, 16);
            if (end == text.c_str() || (end != nullptr && *end != '\0'))
                return false;
            out = static_cast<std::uint64_t>(parsed);
            return true;
        }

        bool TryGetNumericU64(const nlohmann::json& value, std::uint64_t& out)
        {
            if (value.is_number_unsigned()) {
                out = value.get<std::uint64_t>();
                return true;
            }
            if (value.is_number_integer()) {
                const auto parsed = value.get<std::int64_t>();
                if (parsed < 0)
                    return false;
                out = static_cast<std::uint64_t>(parsed);
                return true;
            }
            if (value.is_string()) {
                const std::string text = value.get<std::string>();
                char* end = nullptr;
                const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
                if (end == text.c_str() || (end != nullptr && *end != '\0'))
                    return false;
                out = static_cast<std::uint64_t>(parsed);
                return true;
            }
            return false;
        }

        bool TryGetNumericU32(const nlohmann::json& value, std::uint32_t& out)
        {
            std::uint64_t parsed = 0;
            if (!TryGetNumericU64(value, parsed))
                return false;
            out = static_cast<std::uint32_t>(parsed);
            return true;
        }

        void ApplyMetadataString(const nlohmann::json& src, const char* key, std::string& out)
        {
            if (src.contains(key) && src[key].is_string())
                out = src[key].get<std::string>();
        }

        bool ParseMetadataFromObject(const nlohmann::json& src, TitleMetadata& out)
        {
            if (!src.is_object())
                return false;
            ApplyMetadataString(src, "name", out.name);
            ApplyMetadataString(src, "publisher", out.publisher);
            ApplyMetadataString(src, "intro", out.intro);
            ApplyMetadataString(src, "description", out.description);

            std::uint64_t sizeBytes = 0;
            if (src.contains("size") && TryGetNumericU64(src["size"], sizeBytes)) {
                out.size = sizeBytes;
                out.hasSize = true;
            }

            std::uint32_t version = 0;
            if (src.contains("version") && TryGetNumericU32(src["version"], version)) {
                out.version = version;
                out.hasVersion = true;
            }

            std::uint32_t releaseDate = 0;
            if (src.contains("releaseDate") && TryGetNumericU32(src["releaseDate"], releaseDate)) {
                out.releaseDate = releaseDate;
                out.hasReleaseDate = true;
            }

            if (src.contains("isDemo") && src["isDemo"].is_boolean()) {
                out.isDemo = src["isDemo"].get<bool>();
                out.hasIsDemo = true;
            }

            return !(out.name.empty() && out.publisher.empty() && !out.hasSize && !out.hasVersion && !out.hasReleaseDate && !out.hasIsDemo);
        }

        // Dense row format used by exporter:
        // [id, name, publisher, intro, size, version, releaseDate, isDemoFlag]
        // isDemoFlag: -1 unknown, 0 false, 1 true
        bool ParseMetadataFromDenseRow(const nlohmann::json& row, std::uint64_t& titleId, TitleMetadata& out)
        {
            if (!row.is_array() || row.size() < 2 || !row[0].is_string())
                return false;
            if (!TryParseHexU64(row[0].get<std::string>(), titleId))
                return false;

            if (row.size() > 1 && row[1].is_string())
                out.name = row[1].get<std::string>();
            if (row.size() > 2 && row[2].is_string())
                out.publisher = row[2].get<std::string>();
            if (row.size() > 3 && row[3].is_string())
                out.intro = row[3].get<std::string>();

            if (row.size() > 4 && row[4].is_number_integer()) {
                const auto v = row[4].get<std::int64_t>();
                if (v >= 0) {
                    out.size = static_cast<std::uint64_t>(v);
                    out.hasSize = true;
                }
            }
            if (row.size() > 5 && row[5].is_number_integer()) {
                const auto v = row[5].get<std::int64_t>();
                if (v >= 0) {
                    out.version = static_cast<std::uint32_t>(v);
                    out.hasVersion = true;
                }
            }
            if (row.size() > 6 && row[6].is_number_integer()) {
                const auto v = row[6].get<std::int64_t>();
                if (v >= 0) {
                    out.releaseDate = static_cast<std::uint32_t>(v);
                    out.hasReleaseDate = true;
                }
            }
            if (row.size() > 7 && row[7].is_number_integer()) {
                const auto v = row[7].get<std::int64_t>();
                if (v == 0 || v == 1) {
                    out.isDemo = (v == 1);
                    out.hasIsDemo = true;
                }
            }

            return !(out.name.empty() && out.publisher.empty() && !out.hasSize && !out.hasVersion && !out.hasReleaseDate && !out.hasIsDemo);
        }

        bool ParseMetadataEntryMap(const nlohmann::json& entries, std::unordered_map<std::uint64_t, TitleMetadata>& parsed)
        {
            if (!entries.is_object())
                return false;

            bool parsedAny = false;
            for (const auto& it : entries.items()) {
                const nlohmann::json& value = it.value();
                if (!value.is_object())
                    continue;

                std::uint64_t titleId = 0;
                if (!TryParseHexU64(it.key(), titleId)) {
                    if (!value.contains("id") || !value["id"].is_string())
                        continue;
                    if (!TryParseHexU64(value["id"].get<std::string>(), titleId))
                        continue;
                }

                TitleMetadata meta;
                if (ParseMetadataFromObject(value, meta)) {
                    parsed[titleId] = std::move(meta);
                    parsedAny = true;
                }
            }

            return parsedAny;
        }

        std::string ReadPackedString(const std::vector<char>& blob, std::uint32_t offset)
        {
            if (offset == 0)
                return std::string();
            if (offset >= blob.size())
                return std::string();
            const char* start = blob.data() + offset;
            const std::size_t remaining = blob.size() - offset;
            const void* endPtr = std::memchr(start, '\0', remaining);
            if (endPtr == nullptr)
                return std::string();
            const std::size_t len = static_cast<const char*>(endPtr) - start;
            return std::string(start, len);
        }

        bool TryLoadMetadataFromPackedFile(const std::string& path)
        {
            std::error_code ec;
            const auto fileSize = std::filesystem::file_size(path, ec);
            if (ec || fileSize < sizeof(TitlePackHeader))
                return false;

            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;

            TitlePackHeader header = {};
            in.read(reinterpret_cast<char*>(&header), sizeof(header));
            if (!in)
                return false;

            if (std::memcmp(header.magic, kTitlePackMagic.data(), kTitlePackMagic.size()) != 0)
                return false;
            if (header.version != kTitlePackVersion || header.entrySize != kTitlePackEntrySize || header.entryCount == 0)
                return false;

            const std::uint64_t tableBytes = static_cast<std::uint64_t>(header.entryCount) * static_cast<std::uint64_t>(header.entrySize);
            if (tableBytes > kMaxTitlePackIndexBytes)
                return false;
            if (header.stringsOffset < (sizeof(TitlePackHeader) + tableBytes))
                return false;
            if (header.stringsOffset > fileSize)
                return false;

            const std::uint64_t stringsBytes = fileSize - header.stringsOffset;
            if (stringsBytes > kMaxTitlePackStringsBytes)
                return false;

            std::vector<TitlePackEntryRecord> entries(header.entryCount);
            in.read(reinterpret_cast<char*>(entries.data()), static_cast<std::streamsize>(tableBytes));
            if (!in)
                return false;

            std::vector<char> stringBlob(static_cast<std::size_t>(stringsBytes));
            if (!stringBlob.empty()) {
                in.read(stringBlob.data(), static_cast<std::streamsize>(stringBlob.size()));
                if (!in)
                    return false;
            }

            std::unordered_map<std::uint64_t, TitleMetadata> parsed;
            parsed.reserve(entries.size());
            for (const auto& rec : entries) {
                TitleMetadata meta;

                if (rec.flags & kTitleFlagHasName)
                    meta.name = ReadPackedString(stringBlob, rec.nameOffset);
                if (rec.flags & kTitleFlagHasPublisher)
                    meta.publisher = ReadPackedString(stringBlob, rec.publisherOffset);
                if (rec.flags & kTitleFlagHasIntro)
                    meta.intro = ReadPackedString(stringBlob, rec.introOffset);
                if (rec.flags & kTitleFlagHasDescription)
                    meta.description = ReadPackedString(stringBlob, rec.descriptionOffset);
                if (rec.flags & kTitleFlagHasSize) {
                    meta.size = rec.size;
                    meta.hasSize = true;
                }
                if (rec.flags & kTitleFlagHasVersion) {
                    meta.version = rec.version;
                    meta.hasVersion = true;
                }
                if (rec.flags & kTitleFlagHasReleaseDate) {
                    meta.releaseDate = rec.releaseDate;
                    meta.hasReleaseDate = true;
                }
                if (rec.flags & kTitleFlagHasIsDemo) {
                    meta.isDemo = (rec.isDemo != 0);
                    meta.hasIsDemo = true;
                }

                if (meta.name.empty() && meta.publisher.empty() && meta.intro.empty() && meta.description.empty() &&
                    !meta.hasSize && !meta.hasVersion && !meta.hasReleaseDate && !meta.hasIsDemo) {
                    continue;
                }
                parsed[rec.titleId] = std::move(meta);
            }

            if (parsed.empty())
                return false;

            g_metadataById = std::move(parsed);
            LOG_DEBUG("Offline DB: loaded %llu metadata entries from packed file %s\n",
                static_cast<unsigned long long>(g_metadataById.size()), path.c_str());
            return true;
        }

        bool TryLoadMetadataFromJsonFile(const std::string& path)
        {
            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);
            if (ec || size > kMaxMetadataParseBytes)
                return false;

            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;

            nlohmann::json root;
            try {
                in >> root;
            } catch (...) {
                return false;
            }

            std::unordered_map<std::uint64_t, TitleMetadata> parsed;

            if (root.is_object() && root.contains("titledb") && root["titledb"].is_object()) {
                if (ParseMetadataEntryMap(root["titledb"], parsed)) {
                    g_metadataById = std::move(parsed);
                    LOG_DEBUG("Offline DB: loaded %llu metadata entries from titledb wrapper %s\n",
                        static_cast<unsigned long long>(g_metadataById.size()), path.c_str());
                    return true;
                }
            }

            auto pushDenseRows = [&](const nlohmann::json& rows) {
                if (!rows.is_array())
                    return;
                parsed.reserve(rows.size());
                for (const auto& row : rows) {
                    std::uint64_t titleId = 0;
                    TitleMetadata meta;
                    if (ParseMetadataFromDenseRow(row, titleId, meta))
                        parsed[titleId] = std::move(meta);
                }
            };

            if (root.is_object() && root.contains("d") && root["d"].is_array()) {
                pushDenseRows(root["d"]);
            } else if (root.is_array()) {
                pushDenseRows(root);
            } else if (root.is_object()) {
                parsed.reserve(root.size());
                ParseMetadataEntryMap(root, parsed);
            }

            if (parsed.empty())
                return false;

            g_metadataById = std::move(parsed);
            LOG_DEBUG("Offline DB: loaded %llu metadata entries from %s\n",
                static_cast<unsigned long long>(g_metadataById.size()), path.c_str());
            return true;
        }

        bool EnsureMetadataLoaded()
        {
            if (g_metadataAttempted)
                return g_metadataAvailable;
            g_metadataAttempted = true;
            for (const auto& path : GetMetadataBinaryCandidates()) {
                if (!FileExists(path))
                    continue;
                if (TryLoadMetadataFromPackedFile(path)) {
                    g_metadataAvailable = true;
                    return true;
                }
            }
            for (const auto& path : GetMetadataJsonCandidates()) {
                if (!FileExists(path))
                    continue;
                if (TryLoadMetadataFromJsonFile(path)) {
                    g_metadataAvailable = true;
                    return true;
                }
            }
            g_metadataAvailable = false;
            return false;
        }

        bool TryLoadLegacyIconIndexFromFile(const std::string& path)
        {
            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);
            if (ec || size > kMaxIconIndexParseBytes)
                return false;

            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;

            nlohmann::json root;
            try {
                in >> root;
            } catch (...) {
                return false;
            }
            if (!root.is_object())
                return false;

            std::unordered_map<std::uint64_t, std::string> parsed;
            parsed.reserve(root.size());
            for (const auto& it : root.items()) {
                if (!it.value().is_string())
                    continue;
                std::uint64_t titleId = 0;
                if (!TryParseHexU64(it.key(), titleId))
                    continue;
                const std::string ext = NormalizeExtension(it.value().get<std::string>());
                if (ext.empty())
                    continue;
                parsed[titleId] = ext;
            }

            if (parsed.empty())
                return false;
            g_legacyIconExtById = std::move(parsed);
            return true;
        }

        void EnsureLegacyIconIndexLoaded()
        {
            if (g_legacyIconIndexAttempted)
                return;
            g_legacyIconIndexAttempted = true;
            for (const auto& path : GetLegacyIconIndexCandidates()) {
                if (!FileExists(path))
                    continue;
                if (TryLoadLegacyIconIndexFromFile(path))
                    return;
            }
        }

        bool TryLoadIconPackFromFile(const std::string& path)
        {
            std::error_code ec;
            const auto fileSize = std::filesystem::file_size(path, ec);
            if (ec || fileSize < sizeof(IconPackHeader))
                return false;

            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;

            IconPackHeader header = {};
            in.read(reinterpret_cast<char*>(&header), sizeof(header));
            if (!in)
                return false;

            if (std::memcmp(header.magic, kIconPackMagic.data(), kIconPackMagic.size()) != 0)
                return false;
            if (header.version != kIconPackVersion || header.entrySize != kIconPackEntrySize || header.entryCount == 0)
                return false;

            const std::uint64_t tableBytes = static_cast<std::uint64_t>(header.entryCount) * static_cast<std::uint64_t>(header.entrySize);
            if (tableBytes > kMaxIconPackIndexBytes)
                return false;
            if (header.dataOffset < (sizeof(IconPackHeader) + tableBytes))
                return false;
            if (header.dataOffset > fileSize)
                return false;

            std::unordered_map<std::uint64_t, PackedIconEntry> parsed;
            parsed.reserve(header.entryCount);

            for (std::uint32_t i = 0; i < header.entryCount; i++) {
                IconPackEntryRecord rec = {};
                in.read(reinterpret_cast<char*>(&rec), sizeof(rec));
                if (!in)
                    return false;

                std::size_t extLen = 0;
                while (extLen < sizeof(rec.ext) && rec.ext[extLen] != '\0')
                    extLen++;
                const std::string ext = NormalizeExtension(std::string(rec.ext, extLen));
                if (ext.empty() || rec.size == 0 || rec.size > kMaxIconBytes)
                    continue;

                const std::uint64_t absStart = header.dataOffset + rec.offset;
                const std::uint64_t absEnd = absStart + static_cast<std::uint64_t>(rec.size);
                if (absStart < header.dataOffset || absEnd > fileSize)
                    continue;

                parsed[rec.titleId] = PackedIconEntry{rec.offset, rec.size, ext};
            }

            if (parsed.empty())
                return false;

            g_iconPackEntries = std::move(parsed);
            g_iconPackPath = path;
            g_iconPackDataOffset = header.dataOffset;
            return true;
        }

        bool EnsureIconPackLoaded()
        {
            if (g_iconPackAttempted)
                return g_iconPackAvailable;
            g_iconPackAttempted = true;
            for (const auto& path : GetIconPackCandidates()) {
                if (!FileExists(path))
                    continue;
                if (TryLoadIconPackFromFile(path)) {
                    g_iconPackAvailable = true;
                    return true;
                }
            }
            g_iconPackAvailable = false;
            return false;
        }

        bool TryReadFileBytes(const std::string& path, std::vector<std::uint8_t>& outData)
        {
            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);
            if (ec || size == 0 || size > kMaxIconBytes)
                return false;

            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;
            outData.resize(static_cast<std::size_t>(size));
            in.read(reinterpret_cast<char*>(outData.data()), static_cast<std::streamsize>(outData.size()));
            return static_cast<std::size_t>(in.gcount()) == outData.size();
        }

        bool TryReadPackedIcon(std::uint64_t baseTitleId, std::vector<std::uint8_t>& outData)
        {
            if (!EnsureIconPackLoaded())
                return false;
            const auto it = g_iconPackEntries.find(baseTitleId);
            if (it == g_iconPackEntries.end())
                return false;

            std::ifstream in(g_iconPackPath, std::ios::binary);
            if (!in)
                return false;

            const std::uint64_t absOffset = g_iconPackDataOffset + it->second.offset;
            in.seekg(static_cast<std::streamoff>(absOffset), std::ios::beg);
            if (!in)
                return false;

            outData.resize(it->second.size);
            in.read(reinterpret_cast<char*>(outData.data()), static_cast<std::streamsize>(outData.size()));
            return static_cast<std::size_t>(in.gcount()) == outData.size();
        }

        bool TryFindLegacyIconPath(std::uint64_t baseTitleId, std::string& outPath)
        {
            const std::string titleHex = FormatTitleIdHex(baseTitleId);
            const auto dirs = GetLegacyIconDirectories();
            EnsureLegacyIconIndexLoaded();

            auto tryWithExt = [&](const std::string& ext) -> bool {
                if (ext.empty())
                    return false;
                for (const auto& dir : dirs) {
                    std::string candidate = dir + "/" + titleHex + "." + ext;
                    if (FileExists(candidate)) {
                        outPath = candidate;
                        g_legacyIconExtById[baseTitleId] = ext;
                        return true;
                    }
                }
                return false;
            };

            const auto cached = g_legacyIconExtById.find(baseTitleId);
            if (cached != g_legacyIconExtById.end() && tryWithExt(cached->second))
                return true;

            constexpr std::array<std::string_view, 7> probeExts = {
                "webp", "png", "jpg", "jpeg", "bmp", "tif", "tiff"
            };
            for (const auto ext : probeExts) {
                if (tryWithExt(std::string(ext)))
                    return true;
            }
            return false;
        }
    }

    std::string GetOfflineDbDir()
    {
        return inst::config::appDir + "/offline_db";
    }

    void Invalidate()
    {
        g_metadataById.clear();
        g_metadataAttempted = false;
        g_metadataAvailable = false;

        g_iconPackEntries.clear();
        g_iconPackAttempted = false;
        g_iconPackAvailable = false;
        g_iconPackPath.clear();
        g_iconPackDataOffset = 0;

        g_legacyIconExtById.clear();
        g_legacyIconIndexAttempted = false;
    }

    bool TryGetMetadata(std::uint64_t baseTitleId, TitleMetadata& outMeta)
    {
        if (!EnsureMetadataLoaded())
            return false;
        const auto it = g_metadataById.find(baseTitleId);
        if (it == g_metadataById.end())
            return false;
        outMeta = it->second;
        return true;
    }

    bool HasPackedIcons()
    {
        return EnsureIconPackLoaded();
    }

    bool HasIcon(std::uint64_t baseTitleId)
    {
        if (EnsureIconPackLoaded() && g_iconPackEntries.find(baseTitleId) != g_iconPackEntries.end())
            return true;
        std::string path;
        return TryFindLegacyIconPath(baseTitleId, path);
    }

    bool TryGetIconData(std::uint64_t baseTitleId, std::vector<std::uint8_t>& outData)
    {
        if (TryReadPackedIcon(baseTitleId, outData))
            return true;
        std::string path;
        if (TryFindLegacyIconPath(baseTitleId, path))
            return TryReadFileBytes(path, outData);
        return false;
    }

    bool TryGetIconPath(std::uint64_t baseTitleId, std::string& outPath)
    {
        // Path-based lookup remains as legacy fallback only.
        return TryFindLegacyIconPath(baseTitleId, outPath);
    }
}
