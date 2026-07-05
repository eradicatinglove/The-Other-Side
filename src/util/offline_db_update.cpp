#include "util/offline_db_update.hpp"

#include <array>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <algorithm>
#include <sstream>
#include <thread>
#include <vector>
#include <switch.h>
#include "util/config.hpp"
#include "util/curl.hpp"
#include "util/error.hpp"
#include "util/json.hpp"
#include "util/offline_title_db.hpp"

namespace inst::offline::dbupdate
{
    namespace {
        struct ManifestFile {
            std::string url;
            std::string sha256;
            std::uint64_t size = 0;
        };

        struct ManifestData {
            std::string version;
            ManifestFile titlesPack;
            ManifestFile iconsPack;
        };

        struct ReplaceState {
            std::string target;
            std::string tempPath;
            std::string backupPath;
            bool movedOriginal = false;
            bool installedNew = false;
        };

        std::mutex g_startupCheckMutex;
        bool g_startupCheckReady = false;
        CheckResult g_startupCheckResult;
        std::mutex g_offlineDbTraceMutex;
        std::atomic<std::uint64_t> g_offlineDbTraceCount{0};
        constexpr std::uint64_t kOfflineDbTraceMaxLines = 40000;
        constexpr const char* kOfflineDbTracePath = "sdmc:/switch/CyberFoil/offline_db_update.log";
        constexpr std::uint64_t kParallelDownloadMinSize = 16ULL * 1024ULL * 1024ULL;
        constexpr std::size_t kParallelDownloadParts = 4;

        void OfflineDbTrace(const char* fmt, ...)
        {
#ifndef APP_DEBUG_LOG
            (void)fmt;
            return;
#else
            const std::uint64_t idx = g_offlineDbTraceCount.fetch_add(1, std::memory_order_relaxed);
            if (idx >= kOfflineDbTraceMaxLines) {
                return;
            }

            char msg[640] = {};
            va_list args;
            va_start(args, fmt);
            std::vsnprintf(msg, sizeof(msg), fmt, args);
            va_end(args);

            std::lock_guard<std::mutex> lock(g_offlineDbTraceMutex);
            FILE* f = std::fopen(kOfflineDbTracePath, "ab");
            if (!f) {
                return;
            }
            std::fprintf(f, "%llu %s\n", static_cast<unsigned long long>(idx), msg);
            std::fclose(f);
#endif
        }

        void ResetOfflineDbTrace()
        {
#ifndef APP_DEBUG_LOG
            return;
#else
            std::lock_guard<std::mutex> lock(g_offlineDbTraceMutex);
            std::remove(kOfflineDbTracePath);
            g_offlineDbTraceCount.store(0, std::memory_order_relaxed);
#endif
        }

        std::string Trim(const std::string& text)
        {
            if (text.empty())
                return "";
            const std::size_t begin = text.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos)
                return "";
            const std::size_t end = text.find_last_not_of(" \t\r\n");
            return text.substr(begin, (end - begin) + 1);
        }

        std::string ToLower(std::string text)
        {
            for (char& c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        std::string StripWhitespace(std::string text)
        {
            text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) {
                return std::isspace(c) != 0;
            }), text.end());
            return text;
        }

        bool FileExists(const std::string& path)
        {
            std::error_code ec;
            return std::filesystem::exists(path, ec) && std::filesystem::is_regular_file(path, ec);
        }

        void RemoveIfExists(const std::string& path)
        {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }

        std::string OfflineDbDir()
        {
            return inst::config::appDir + "/offline_db";
        }

        std::string LocalManifestPath()
        {
            return OfflineDbDir() + "/manifest.json";
        }

        std::string LocalManifestAliasPath()
        {
            return OfflineDbDir() + "/offline_db_manifest.json";
        }

        std::string NormalizeSha256(const std::string& value)
        {
            std::string out;
            out.reserve(value.size());
            for (unsigned char c : value) {
                if (std::isxdigit(c))
                    out.push_back(static_cast<char>(std::tolower(c)));
            }
            return out;
        }

        bool IsValidSha256Hex(const std::string& value)
        {
            if (value.size() != 64)
                return false;
            for (unsigned char c : value) {
                if (!std::isxdigit(c))
                    return false;
            }
            return true;
        }

        bool TryGetU64(const nlohmann::json& value, std::uint64_t& out)
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
                const std::string text = Trim(value.get<std::string>());
                if (text.empty())
                    return false;
                char* end = nullptr;
                const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
                if (end == text.c_str() || (end != nullptr && *end != '\0'))
                    return false;
                out = static_cast<std::uint64_t>(parsed);
                return true;
            }
            return false;
        }

        std::string UrlOrigin(const std::string& url)
        {
            const std::size_t schemePos = url.find("://");
            if (schemePos == std::string::npos)
                return "";
            const std::size_t hostStart = schemePos + 3;
            const std::size_t hostEnd = url.find('/', hostStart);
            if (hostEnd == std::string::npos)
                return url;
            return url.substr(0, hostEnd);
        }

        std::string UrlBase(const std::string& url)
        {
            std::string base = url;
            const std::size_t hashPos = base.find('#');
            if (hashPos != std::string::npos)
                base = base.substr(0, hashPos);
            const std::size_t queryPos = base.find('?');
            if (queryPos != std::string::npos)
                base = base.substr(0, queryPos);
            const std::size_t slashPos = base.find_last_of('/');
            if (slashPos == std::string::npos)
                return base + "/";
            return base.substr(0, slashPos + 1);
        }

        std::string ResolveUrl(const std::string& manifestUrl, const std::string& rawUrl)
        {
            const std::string url = StripWhitespace(Trim(rawUrl));
            if (url.empty())
                return "";
            if (url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0)
                return url;
            if (url.rfind("//", 0) == 0) {
                const std::size_t schemePos = manifestUrl.find("://");
                const std::string scheme = (schemePos == std::string::npos) ? "https" : manifestUrl.substr(0, schemePos);
                return scheme + ":" + url;
            }
            if (!url.empty() && url.front() == '/') {
                const std::string origin = UrlOrigin(manifestUrl);
                if (!origin.empty())
                    return origin + url;
            }
            return UrlBase(manifestUrl) + url;
        }

        bool ParseManifestFile(const nlohmann::json& entry, const std::string& manifestUrl, const char* name, ManifestFile& out, std::string& error)
        {
            if (!entry.is_object()) {
                error = std::string("Invalid manifest entry for ") + name + ".";
                return false;
            }

            std::string rawUrl;
            if (entry.contains("url") && entry["url"].is_string())
                rawUrl = entry["url"].get<std::string>();
            else if (entry.contains("path") && entry["path"].is_string())
                rawUrl = entry["path"].get<std::string>();
            if (rawUrl.empty()) {
                error = std::string("Missing URL for ") + name + ".";
                return false;
            }

            std::uint64_t size = 0;
            if (!entry.contains("size") || !TryGetU64(entry["size"], size) || size == 0) {
                error = std::string("Missing or invalid size for ") + name + ".";
                return false;
            }

            std::string sha;
            if (entry.contains("sha256") && entry["sha256"].is_string())
                sha = entry["sha256"].get<std::string>();
            else if (entry.contains("sha_256") && entry["sha_256"].is_string())
                sha = entry["sha_256"].get<std::string>();
            sha = NormalizeSha256(sha);
            if (!IsValidSha256Hex(sha)) {
                error = std::string("Missing or invalid sha256 for ") + name + ".";
                return false;
            }

            out.url = ResolveUrl(manifestUrl, rawUrl);
            out.size = size;
            out.sha256 = sha;
            return true;
        }

        bool ParseManifestJson(const nlohmann::json& root, const std::string& manifestUrl, ManifestData& out, std::string& error)
        {
            if (!root.is_object()) {
                error = "Manifest root is not an object.";
                return false;
            }

            std::string version;
            if (root.contains("db_version") && root["db_version"].is_string())
                version = Trim(root["db_version"].get<std::string>());
            else if (root.contains("version") && root["version"].is_string())
                version = Trim(root["version"].get<std::string>());
            if (version.empty()) {
                error = "Manifest is missing db_version.";
                return false;
            }

            if (!root.contains("files") || !root["files"].is_object()) {
                error = "Manifest is missing files object.";
                return false;
            }

            const nlohmann::json& files = root["files"];
            const nlohmann::json* titlesNode = nullptr;
            const nlohmann::json* iconsNode = nullptr;

            if (files.contains("titles.pack"))
                titlesNode = &files["titles.pack"];
            else if (files.contains("titles_pack"))
                titlesNode = &files["titles_pack"];
            else if (files.contains("titles"))
                titlesNode = &files["titles"];

            if (files.contains("icons.pack"))
                iconsNode = &files["icons.pack"];
            else if (files.contains("icons_pack"))
                iconsNode = &files["icons_pack"];
            else if (files.contains("icons"))
                iconsNode = &files["icons"];

            if (titlesNode == nullptr || iconsNode == nullptr) {
                error = "Manifest must include titles.pack and icons.pack entries.";
                return false;
            }

            ManifestFile titles;
            ManifestFile icons;
            if (!ParseManifestFile(*titlesNode, manifestUrl, "titles.pack", titles, error))
                return false;
            if (!ParseManifestFile(*iconsNode, manifestUrl, "icons.pack", icons, error))
                return false;

            out.version = version;
            out.titlesPack = std::move(titles);
            out.iconsPack = std::move(icons);
            return true;
        }

        bool FetchManifest(const std::string& manifestUrl, ManifestData& outManifest, std::string& outManifestText, std::string& error)
        {
            OfflineDbTrace("FetchManifest start url='%s'", manifestUrl.c_str());
            outManifestText = inst::curl::downloadToBuffer(manifestUrl, 0, 0, 10000);
            if (outManifestText.empty()) {
                error = "Failed to download manifest.";
                OfflineDbTrace("FetchManifest fail: empty response");
                return false;
            }

            nlohmann::json root;
            try {
                root = nlohmann::json::parse(outManifestText);
            } catch (...) {
                error = "Failed to parse manifest JSON.";
                OfflineDbTrace("FetchManifest fail: JSON parse error");
                return false;
            }

            const bool ok = ParseManifestJson(root, manifestUrl, outManifest, error);
            if (!ok) {
                OfflineDbTrace("FetchManifest fail: %s", error.c_str());
                return false;
            }
            OfflineDbTrace("FetchManifest ok version='%s'", outManifest.version.c_str());
            return true;
        }

        bool ReadLocalManifestVersion(const std::string& path, std::string& outVersion)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;

            nlohmann::json root;
            try {
                in >> root;
            } catch (...) {
                return false;
            }

            if (root.is_object()) {
                if (root.contains("db_version") && root["db_version"].is_string()) {
                    outVersion = Trim(root["db_version"].get<std::string>());
                    return !outVersion.empty();
                }
                if (root.contains("version") && root["version"].is_string()) {
                    outVersion = Trim(root["version"].get<std::string>());
                    return !outVersion.empty();
                }
            }
            return false;
        }

        bool ComputeFileSha256(const std::string& path, std::string& outHexHash, std::uint64_t& outSize,
            const std::function<void(std::uint64_t read, std::uint64_t total)>& progress = {})
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;

            std::uint64_t totalSize = 0;
            std::error_code sizeEc;
            totalSize = std::filesystem::file_size(path, sizeEc);
            if (sizeEc)
                totalSize = 0;

            Sha256Context ctx;
            sha256ContextCreate(&ctx);
            std::vector<char> buffer(256 * 1024);
            if (buffer.empty())
                return false;
            outSize = 0;

            while (true) {
                in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const std::streamsize got = in.gcount();
                if (got > 0) {
                    sha256ContextUpdate(&ctx, buffer.data(), static_cast<std::size_t>(got));
                    outSize += static_cast<std::uint64_t>(got);
                    if (progress)
                        progress(outSize, totalSize);
                }
                if (!in)
                    break;
            }

            if (!in.eof())
                return false;

            std::array<std::uint8_t, SHA256_HASH_SIZE> hash{};
            sha256ContextGetHash(&ctx, hash.data());
            std::ostringstream hex;
            hex.fill('0');
            hex << std::hex;
            for (std::uint8_t b : hash)
                hex << std::setw(2) << static_cast<int>(b);
            outHexHash = ToLower(hex.str());
            return true;
        }

        bool DownloadAndVerify(const ManifestFile& file, const std::string& tempPath, std::string& error,
            const ProgressCallback& progress, const std::string& stageLabel,
            double percentStart, double percentEnd)
        {
            const double clampedStart = std::max(0.0, std::min(100.0, percentStart));
            const double clampedEnd = std::max(clampedStart, std::min(100.0, percentEnd));
            const double span = clampedEnd - clampedStart;
            const double downloadEnd = clampedStart + (span * 0.82);
            const double verifyStart = downloadEnd;
            const std::uint64_t tickFreq = armGetSystemTickFreq();
            u64 lastReportTick = 0;
            double lastReportedPercent = -1.0;
            std::uint64_t lastNow = 0;
            std::uint64_t lastTotal = 0;
            bool speedInitialized = false;
            u64 speedTickStart = 0;
            std::uint64_t speedBytesStart = 0;
            double smoothedSpeedMbPerSec = 0.0;

            auto reportDownloadProgress = [&](std::uint64_t now, std::uint64_t total, bool force) {
                lastNow = now;
                lastTotal = total;

                std::uint64_t effectiveTotal = total;
                if (effectiveTotal == 0)
                    effectiveTotal = file.size;
                if (effectiveTotal == 0)
                    effectiveTotal = 1;

                const std::uint64_t boundedNow = std::min(now, effectiveTotal);
                double ratio = static_cast<double>(boundedNow) / static_cast<double>(effectiveTotal);
                if (ratio < 0.0)
                    ratio = 0.0;
                if (ratio > 1.0)
                    ratio = 1.0;

                const double mappedPercent = clampedStart + ((downloadEnd - clampedStart) * ratio);
                const u64 nowTick = armGetSystemTick();
                const bool enoughTime = (lastReportTick == 0) || (tickFreq > 0 && (nowTick - lastReportTick) >= (tickFreq / 5));
                const bool enoughProgress = (lastReportedPercent < 0.0) || (mappedPercent >= lastReportedPercent + 0.5);

                if (!force && !enoughTime && !enoughProgress)
                    return;

                if (tickFreq > 0) {
                    if (!speedInitialized) {
                        speedInitialized = true;
                        speedTickStart = nowTick;
                        speedBytesStart = boundedNow;
                    } else if (nowTick > speedTickStart && boundedNow >= speedBytesStart) {
                        const u64 elapsedTicks = nowTick - speedTickStart;
                        const std::uint64_t elapsedBytes = boundedNow - speedBytesStart;
                        const bool shouldRefreshSpeed = force || (elapsedTicks >= (tickFreq / 4));
                        if (shouldRefreshSpeed && elapsedTicks > 0) {
                            const double elapsedSeconds = static_cast<double>(elapsedTicks) / static_cast<double>(tickFreq);
                            if (elapsedSeconds > 0.0) {
                                const double currentSpeedMbPerSec = (static_cast<double>(elapsedBytes) / (1024.0 * 1024.0)) / elapsedSeconds;
                                if (smoothedSpeedMbPerSec <= 0.0) {
                                    smoothedSpeedMbPerSec = currentSpeedMbPerSec;
                                } else {
                                    // Slight smoothing to reduce UI jitter between callbacks.
                                    smoothedSpeedMbPerSec = (smoothedSpeedMbPerSec * 0.65) + (currentSpeedMbPerSec * 0.35);
                                }
                            }
                            speedTickStart = nowTick;
                            speedBytesStart = boundedNow;
                        }
                    }
                }

                const int shownPercent = static_cast<int>(mappedPercent + 0.5);
                const std::uint64_t nowMb = boundedNow / (1024ULL * 1024ULL);
                const std::uint64_t totalMb = effectiveTotal / (1024ULL * 1024ULL);
                if (progress) {
                    std::ostringstream status;
                    status << stageLabel << " (" << shownPercent << "%, " << nowMb << "/" << totalMb << " MB";
                    if (smoothedSpeedMbPerSec > 0.0) {
                        status << ", " << std::fixed << std::setprecision(2) << smoothedSpeedMbPerSec << " MB/s";
                    }
                    status << ")";
                    progress(status.str(), mappedPercent);
                }
                lastReportTick = nowTick;
                lastReportedPercent = mappedPercent;
            };

            reportDownloadProgress(0, file.size, true);

            OfflineDbTrace("DownloadAndVerify start url='%s' temp='%s' expected_size=%llu expected_sha=%s",
                file.url.c_str(),
                tempPath.c_str(),
                static_cast<unsigned long long>(file.size),
                file.sha256.c_str());
            LOG_DEBUG("Offline DB download start: %s\n", file.url.c_str());
            try {
                RemoveIfExists(tempPath);
                auto singleStreamDownload = [&]() -> bool {
                    RemoveIfExists(tempPath);
                    if (!inst::curl::downloadFileWithProgress(file.url, tempPath.c_str(), 0,
                            [&](std::uint64_t downloaded, std::uint64_t total) {
                                reportDownloadProgress(downloaded, total, false);
                            })) {
                        RemoveIfExists(tempPath);
                        error = "Download failed: " + file.url;
                        OfflineDbTrace("DownloadAndVerify fail: download error url='%s'", file.url.c_str());
                        LOG_DEBUG("Offline DB download failed: %s\n", file.url.c_str());
                        return false;
                    }
                    return true;
                };
                const bool useParallelDownload = file.size >= kParallelDownloadMinSize;
                if (useParallelDownload) {
                    struct PartState {
                        std::uint64_t start = 0;
                        std::uint64_t endInclusive = 0;
                        std::atomic<std::uint64_t> downloaded{0};
                        std::atomic<bool> done{false};
                        bool success = false;
                    };

                    const std::size_t partCount = std::min<std::size_t>(kParallelDownloadParts,
                        std::max<std::size_t>(2, static_cast<std::size_t>((file.size + kParallelDownloadMinSize - 1) / kParallelDownloadMinSize)));
                    const std::uint64_t partSize = (file.size + partCount - 1) / partCount;
                    std::vector<PartState> parts(partCount);
                    std::vector<std::thread> workers;
                    std::mutex failureMutex;
                    bool failed = false;

                    OfflineDbTrace("DownloadAndVerify using parallel download parts=%llu size=%llu",
                        static_cast<unsigned long long>(partCount),
                        static_cast<unsigned long long>(file.size));

                    {
                        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
                        if (!out) {
                            error = "Failed to prepare download file.";
                            OfflineDbTrace("DownloadAndVerify fail: failed to create temp output '%s'", tempPath.c_str());
                            return false;
                        }
                    }
                    std::error_code resizeEc;
                    std::filesystem::resize_file(tempPath, file.size, resizeEc);
                    if (resizeEc) {
                        RemoveIfExists(tempPath);
                        error = "Failed to prepare download file.";
                        OfflineDbTrace("DownloadAndVerify fail: resize_file failed temp='%s' ec=%d", tempPath.c_str(), resizeEc.value());
                        return false;
                    }

                    for (std::size_t i = 0; i < partCount; i++) {
                        const std::uint64_t start = static_cast<std::uint64_t>(i) * partSize;
                        const std::uint64_t endInclusive = std::min<std::uint64_t>(file.size - 1, start + partSize - 1);
                        parts[i].start = start;
                        parts[i].endInclusive = endInclusive;

                        workers.emplace_back([&, i]() {
                            const bool ok = inst::curl::downloadFileRangeToOffsetWithProgress(file.url, tempPath.c_str(),
                                parts[i].start, parts[i].start, parts[i].endInclusive, 0,
                                [&](std::uint64_t downloaded, std::uint64_t /*total*/) {
                                    parts[i].downloaded.store(downloaded, std::memory_order_relaxed);
                                });
                            parts[i].success = ok;
                            if (!ok) {
                                std::lock_guard<std::mutex> guard(failureMutex);
                                failed = true;
                            } else {
                                parts[i].downloaded.store((parts[i].endInclusive - parts[i].start) + 1, std::memory_order_relaxed);
                            }
                            parts[i].done.store(true, std::memory_order_release);
                        });
                    }

                    while (true) {
                        std::uint64_t totalDownloaded = 0;
                        bool allDone = true;
                        for (const auto& part : parts) {
                            totalDownloaded += part.downloaded.load(std::memory_order_relaxed);
                            if (!part.done.load(std::memory_order_acquire))
                                allDone = false;
                        }

                        reportDownloadProgress(totalDownloaded, file.size, allDone);
                        if (allDone)
                            break;

                        svcSleepThread(100'000'000ULL);
                    }

                    for (auto& worker : workers) {
                        if (worker.joinable())
                            worker.join();
                    }

                    if (failed) {
                        RemoveIfExists(tempPath);
                        OfflineDbTrace("DownloadAndVerify parallel path failed, falling back to single-stream url='%s'", file.url.c_str());
                        LOG_DEBUG("Offline DB parallel download failed, retrying single-stream: %s\n", file.url.c_str());
                        if (!singleStreamDownload())
                            return false;
                    }
                } else if (!singleStreamDownload()) {
                    return false;
                }

                reportDownloadProgress(lastTotal ? lastTotal : file.size, lastTotal ? lastTotal : file.size, true);
                if (progress)
                    progress(stageLabel + " (verifying...)", verifyStart);

                OfflineDbTrace("DownloadAndVerify download complete, starting hash temp='%s'", tempPath.c_str());
                std::uint64_t actualSize = 0;
                std::string actualSha;
                u64 lastVerifyReportTick = 0;
                double lastVerifyPercent = -1.0;
                if (!ComputeFileSha256(tempPath, actualSha, actualSize,
                        [&](std::uint64_t read, std::uint64_t total) {
                            if (!progress)
                                return;

                            std::uint64_t effectiveTotal = total == 0 ? file.size : total;
                            if (effectiveTotal == 0)
                                effectiveTotal = 1;
                            const std::uint64_t boundedRead = std::min(read, effectiveTotal);
                            double ratio = static_cast<double>(boundedRead) / static_cast<double>(effectiveTotal);
                            if (ratio < 0.0)
                                ratio = 0.0;
                            if (ratio > 1.0)
                                ratio = 1.0;

                            const double mappedPercent = verifyStart + ((clampedEnd - verifyStart) * ratio);
                            const u64 nowTick = armGetSystemTick();
                            const bool enoughTime = (lastVerifyReportTick == 0) || (tickFreq > 0 && (nowTick - lastVerifyReportTick) >= (tickFreq / 5));
                            const bool enoughProgress = (lastVerifyPercent < 0.0) || (mappedPercent >= lastVerifyPercent + 0.5);
                            if (!enoughTime && !enoughProgress)
                                return;

                            std::ostringstream status;
                            status << stageLabel << " (verifying " << static_cast<int>(mappedPercent + 0.5) << "%)";
                            progress(status.str(), mappedPercent);
                            lastVerifyReportTick = nowTick;
                            lastVerifyPercent = mappedPercent;
                        })) {
                    RemoveIfExists(tempPath);
                    error = "Failed to verify downloaded file hash.";
                    OfflineDbTrace("DownloadAndVerify fail: failed to hash temp='%s'", tempPath.c_str());
                    LOG_DEBUG("Offline DB hash verify failed to read temp file: %s\n", tempPath.c_str());
                    return false;
                }

                if (progress)
                    progress(stageLabel + " (verified)", clampedEnd);

                if (actualSize != file.size) {
                    RemoveIfExists(tempPath);
                    error = "Downloaded file size mismatch.";
                    OfflineDbTrace("DownloadAndVerify fail: size mismatch expected=%llu actual=%llu",
                        static_cast<unsigned long long>(file.size),
                        static_cast<unsigned long long>(actualSize));
                    LOG_DEBUG("Offline DB size mismatch expected=%llu actual=%llu\n",
                        static_cast<unsigned long long>(file.size),
                        static_cast<unsigned long long>(actualSize));
                    return false;
                }

                if (actualSha != file.sha256) {
                    RemoveIfExists(tempPath);
                    error = "Downloaded file sha256 mismatch.";
                    OfflineDbTrace("DownloadAndVerify fail: sha mismatch expected=%s actual=%s",
                        file.sha256.c_str(),
                        actualSha.c_str());
                    LOG_DEBUG("Offline DB sha256 mismatch expected=%s actual=%s\n",
                        file.sha256.c_str(), actualSha.c_str());
                    return false;
                }

                OfflineDbTrace("DownloadAndVerify ok url='%s' actual_size=%llu",
                    file.url.c_str(),
                    static_cast<unsigned long long>(actualSize));
                LOG_DEBUG("Offline DB download verified: %s (%llu bytes)\n",
                    file.url.c_str(), static_cast<unsigned long long>(actualSize));
                return true;
            } catch (const std::exception& e) {
                RemoveIfExists(tempPath);
                error = "Download verification threw exception.";
                OfflineDbTrace("DownloadAndVerify exception: %s", e.what());
                return false;
            } catch (...) {
                RemoveIfExists(tempPath);
                error = "Download verification threw unknown exception.";
                OfflineDbTrace("DownloadAndVerify exception: unknown");
                return false;
            }
        }

        bool RenameFile(const std::string& from, const std::string& to, std::string& error)
        {
            std::error_code ec;
            std::filesystem::rename(from, to, ec);
            if (ec) {
                error = "Failed to rename " + from + " -> " + to + ".";
                return false;
            }
            return true;
        }

        bool CommitReplace(ReplaceState& state, std::string& error)
        {
            RemoveIfExists(state.backupPath);
            if (FileExists(state.target)) {
                if (!RenameFile(state.target, state.backupPath, error))
                    return false;
                state.movedOriginal = true;
            }

            if (!RenameFile(state.tempPath, state.target, error)) {
                if (state.movedOriginal) {
                    std::string rollbackError;
                    RenameFile(state.backupPath, state.target, rollbackError);
                }
                return false;
            }

            state.installedNew = true;
            return true;
        }

        void RollbackReplace(ReplaceState& state)
        {
            if (state.installedNew)
                RemoveIfExists(state.target);
            if (state.movedOriginal && FileExists(state.backupPath)) {
                std::error_code ec;
                std::filesystem::rename(state.backupPath, state.target, ec);
            }
            RemoveIfExists(state.tempPath);
        }

        void CleanupReplace(const ReplaceState& state)
        {
            RemoveIfExists(state.backupPath);
            RemoveIfExists(state.tempPath);
        }

        void ReportProgress(const ProgressCallback& cb, const std::string& stage, double percent)
        {
            if (cb)
                cb(stage, percent);
        }
    }

    std::string GetInstalledVersion()
    {
        std::string version;
        if (ReadLocalManifestVersion(LocalManifestPath(), version))
            return version;
        if (ReadLocalManifestVersion(LocalManifestAliasPath(), version))
            return version;
        return "";
    }

    bool HasInstalledPacks()
    {
        const std::string base = OfflineDbDir();
        return FileExists(base + "/titles.pack") && FileExists(base + "/icons.pack");
    }

    CheckResult CheckForUpdate(const std::string& manifestUrl)
    {
        CheckResult result;
        const std::string trimmedUrl = StripWhitespace(Trim(manifestUrl));
        OfflineDbTrace("CheckForUpdate start manifest_url='%s'", trimmedUrl.c_str());
        if (trimmedUrl.empty()) {
            result.error = "Offline DB manifest URL is empty.";
            OfflineDbTrace("CheckForUpdate fail: empty URL");
            return result;
        }

        ManifestData manifest;
        std::string manifestText;
        if (!FetchManifest(trimmedUrl, manifest, manifestText, result.error)) {
            OfflineDbTrace("CheckForUpdate fail: %s", result.error.c_str());
            return result;
        }

        result.localVersion = GetInstalledVersion();
        result.remoteVersion = manifest.version;
        result.updateAvailable = !HasInstalledPacks()
            || result.localVersion.empty()
            || result.localVersion != result.remoteVersion;
        result.success = true;
        OfflineDbTrace("CheckForUpdate ok local='%s' remote='%s' update=%d",
            result.localVersion.c_str(),
            result.remoteVersion.c_str(),
            result.updateAvailable ? 1 : 0);
        return result;
    }

    ApplyResult ApplyUpdate(const std::string& manifestUrl, bool force, ProgressCallback progress)
    {
        ResetOfflineDbTrace();
        ApplyResult result;
        const std::string trimmedUrl = StripWhitespace(Trim(manifestUrl));
        OfflineDbTrace("ApplyUpdate start manifest_url='%s' force=%d", trimmedUrl.c_str(), force ? 1 : 0);
        if (trimmedUrl.empty()) {
            result.error = "Offline DB manifest URL is empty.";
            OfflineDbTrace("ApplyUpdate fail: empty URL");
            return result;
        }

        ReportProgress(progress, "Downloading offline DB manifest", 5.0);
        OfflineDbTrace("ApplyUpdate progress: download manifest");

        ManifestData manifest;
        std::string manifestText;
        if (!FetchManifest(trimmedUrl, manifest, manifestText, result.error)) {
            OfflineDbTrace("ApplyUpdate fail: %s", result.error.c_str());
            return result;
        }

        const std::string localVersion = GetInstalledVersion();
        const bool needsUpdate = force
            || !HasInstalledPacks()
            || localVersion.empty()
            || localVersion != manifest.version;
        OfflineDbTrace("ApplyUpdate version local='%s' remote='%s' needs_update=%d",
            localVersion.c_str(),
            manifest.version.c_str(),
            needsUpdate ? 1 : 0);

        if (!needsUpdate) {
            result.success = true;
            result.updated = false;
            result.version = manifest.version;
            OfflineDbTrace("ApplyUpdate skipped: already up to date");
            return result;
        }

        const std::string dbDir = OfflineDbDir();
        std::error_code ec;
        std::filesystem::create_directories(dbDir, ec);
        if (ec) {
            result.error = "Failed to create offline_db directory.";
            OfflineDbTrace("ApplyUpdate fail: create_directories '%s' failed ec=%d", dbDir.c_str(), ec.value());
            return result;
        }

        const std::string titlesTemp = dbDir + "/titles.pack.download";
        const std::string iconsTemp = dbDir + "/icons.pack.download";
        const std::string manifestTemp = dbDir + "/manifest.json.download";
        OfflineDbTrace("ApplyUpdate temp files titles='%s' icons='%s' manifest='%s'",
            titlesTemp.c_str(), iconsTemp.c_str(), manifestTemp.c_str());

        OfflineDbTrace("ApplyUpdate progress: downloading titles.pack");
        if (!DownloadAndVerify(manifest.titlesPack, titlesTemp, result.error, progress,
                "Downloading titles.pack", 20.0, 45.0)) {
            OfflineDbTrace("ApplyUpdate fail: titles.pack %s", result.error.c_str());
            return result;
        }

        OfflineDbTrace("ApplyUpdate progress: downloading icons.pack");
        if (!DownloadAndVerify(manifest.iconsPack, iconsTemp, result.error, progress,
                "Downloading icons.pack", 45.0, 70.0)) {
            RemoveIfExists(titlesTemp);
            OfflineDbTrace("ApplyUpdate fail: icons.pack %s", result.error.c_str());
            return result;
        }

        ReportProgress(progress, "Writing manifest", 70.0);
        OfflineDbTrace("ApplyUpdate progress: writing local manifest");
        {
            std::ofstream out(manifestTemp, std::ios::binary | std::ios::trunc);
            if (!out) {
                result.error = "Failed to write local manifest.";
                OfflineDbTrace("ApplyUpdate fail: failed to open manifest temp '%s' for writing", manifestTemp.c_str());
                RemoveIfExists(titlesTemp);
                RemoveIfExists(iconsTemp);
                return result;
            }
            out << manifestText;
            if (manifestText.empty() || manifestText.back() != '\n')
                out << '\n';
            if (!out.good()) {
                result.error = "Failed to write local manifest.";
                OfflineDbTrace("ApplyUpdate fail: write manifest temp failed");
                RemoveIfExists(manifestTemp);
                RemoveIfExists(titlesTemp);
                RemoveIfExists(iconsTemp);
                return result;
            }
        }

        ReportProgress(progress, "Installing offline DB", 85.0);
        OfflineDbTrace("ApplyUpdate progress: installing files");

        ReplaceState titlesState{
            dbDir + "/titles.pack",
            titlesTemp,
            dbDir + "/titles.pack.bak"
        };
        ReplaceState iconsState{
            dbDir + "/icons.pack",
            iconsTemp,
            dbDir + "/icons.pack.bak"
        };
        ReplaceState manifestState{
            LocalManifestPath(),
            manifestTemp,
            LocalManifestPath() + ".bak"
        };

        if (!CommitReplace(titlesState, result.error)) {
            RollbackReplace(titlesState);
            RemoveIfExists(iconsTemp);
            RemoveIfExists(manifestTemp);
            OfflineDbTrace("ApplyUpdate fail: commit titles failed %s", result.error.c_str());
            return result;
        }

        if (!CommitReplace(iconsState, result.error)) {
            RollbackReplace(iconsState);
            RollbackReplace(titlesState);
            RemoveIfExists(manifestTemp);
            OfflineDbTrace("ApplyUpdate fail: commit icons failed %s", result.error.c_str());
            return result;
        }

        if (!CommitReplace(manifestState, result.error)) {
            RollbackReplace(manifestState);
            RollbackReplace(iconsState);
            RollbackReplace(titlesState);
            OfflineDbTrace("ApplyUpdate fail: commit manifest failed %s", result.error.c_str());
            return result;
        }

        CleanupReplace(titlesState);
        CleanupReplace(iconsState);
        CleanupReplace(manifestState);

        std::filesystem::copy_file(LocalManifestPath(), LocalManifestAliasPath(),
            std::filesystem::copy_options::overwrite_existing, ec);

        inst::offline::Invalidate();
        LOG_DEBUG("Offline DB updated to %s\n", manifest.version.c_str());
        OfflineDbTrace("ApplyUpdate success version='%s'", manifest.version.c_str());

        ReportProgress(progress, "Offline DB update complete", 100.0);
        result.success = true;
        result.updated = true;
        result.version = manifest.version;
        return result;
    }

    void ResetStartupCheckState()
    {
        std::lock_guard<std::mutex> guard(g_startupCheckMutex);
        g_startupCheckResult = {};
        g_startupCheckReady = false;
    }

    void SetStartupCheckResult(const CheckResult& result)
    {
        std::lock_guard<std::mutex> guard(g_startupCheckMutex);
        g_startupCheckResult = result;
        g_startupCheckReady = true;
    }

    bool TryGetStartupCheckResult(CheckResult& outResult)
    {
        std::lock_guard<std::mutex> guard(g_startupCheckMutex);
        if (!g_startupCheckReady)
            return false;
        outResult = g_startupCheckResult;
        return true;
    }
}
