#include "install/http_stream_nsp.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <curl/curl.h>
#include <switch.h>

#include "install/install.hpp"
#include "install/pfs0.hpp"
#include "data/byte_buffer.hpp"
#include "nx/nca_writer.h"
#include "nx/ncm.hpp"
#include "nx/ipc/es.h"
#include "util/error.hpp"
#include "util/file_util.hpp"
#include "util/title_util.hpp"
#include "ui/instPage.hpp"

// Basically the same push-based demux approach as MtpNspStream in
// mtp_install.cpp, just fed by a single curl download instead of MTP
// writes. Own file so it doesn't touch MTP/SD/USB/the old HTTP path.
namespace tin::install::nsp
{
    namespace
    {
        // same trick as the MTP path - subclass Install, stub out what we
        // drive manually, expose the commit helpers
        class HttpStreamInstallHelper final : public tin::install::Install
        {
            public:
                HttpStreamInstallHelper(NcmStorageId destStorage, bool ignoreReq)
                    : Install(destStorage, ignoreReq) {}

                void AddContentMeta(const nx::ncm::ContentMeta& meta, const NcmContentInfo& info)
                {
                    m_contentMeta.push_back(meta);
                    m_cnmtInfos.push_back(info);
                }

                void CommitLatest()
                {
                    if (m_contentMeta.empty()) return;
                    const size_t idx = m_contentMeta.size() - 1;
                    tin::data::ByteBuffer installBuf;
                    m_contentMeta[idx].GetInstallContentMeta(installBuf, m_cnmtInfos[idx], m_ignoreReqFirmVersion);
                    InstallContentMetaRecords(installBuf, idx);
                    InstallApplicationRecord(idx);
                }

            private:
                std::vector<NcmContentInfo> m_cnmtInfos;

                std::vector<std::tuple<nx::ncm::ContentMeta, NcmContentInfo>> ReadCNMT() override { return {}; }
                void InstallTicketCert() override {}
                void InstallNCA(const NcmContentId&) override {}
        };

        struct NspEntryState
        {
            std::string name;
            NcmContentId ncaId{};
            std::uint64_t dataOffset = 0;
            std::uint64_t size = 0;
            std::uint64_t written = 0;
            bool started = false;
            bool complete = false;
            bool isNca = false;
            bool isCnmt = false;
            std::shared_ptr<nx::ncm::ContentStorage> storage;
            std::unique_ptr<NcaWriter> ncaWriter;
            std::vector<std::uint8_t> ticketBuf;
            std::vector<std::uint8_t> certBuf;
        };

        // takes whatever chunk sizes curl's write callback hands us (offset
        // always non-decreasing) and demuxes into the right NCA placeholder
        class HttpNspDemuxer
        {
            public:
                HttpNspDemuxer(NcmStorageId destStorage, bool ignoreReqFirmVersion)
                    : m_destStorage(destStorage)
                {
                    m_helper = std::make_unique<HttpStreamInstallHelper>(destStorage, ignoreReqFirmVersion);
                }

                // false = unrecoverable
                bool Feed(const std::uint8_t* data, size_t size, std::uint64_t offset)
                {
                    try
                    {
                        const std::uint64_t chunkStart = offset;
                        const std::uint64_t chunkEnd = offset + size;

                        if (!m_headerParsed && offset <= kMaxHeaderScan)
                        {
                            const std::uint64_t end = std::min<std::uint64_t>(offset + size, kMaxHeaderScan);
                            const size_t len = static_cast<size_t>(end - offset);
                            if (m_headerBytes.size() < offset + len)
                                m_headerBytes.resize(offset + len);
                            std::memcpy(m_headerBytes.data() + offset, data, len);
                        }

                        if (!ParseHeaderIfReady())
                            return true; // still buffering the header

                        if (m_entries.empty())
                            return true;

                        if (m_hintIndex >= m_entries.size())
                            m_hintIndex = 0;
                        while (m_hintIndex > 0 && chunkStart < m_entries[m_hintIndex].dataOffset)
                            --m_hintIndex;
                        while (m_hintIndex < m_entries.size())
                        {
                            const std::uint64_t end = m_entries[m_hintIndex].dataOffset + m_entries[m_hintIndex].size;
                            if (chunkStart < end) break;
                            ++m_hintIndex;
                        }

                        for (size_t i = m_hintIndex; i < m_entries.size(); ++i)
                        {
                            NspEntryState& entry = m_entries[i];
                            const std::uint64_t entryStart = entry.dataOffset;
                            const std::uint64_t entryEnd = entry.dataOffset + entry.size;

                            if (chunkEnd <= entryStart) break;
                            if (chunkStart >= entryEnd) continue;

                            const std::uint64_t writeStart = std::max<std::uint64_t>(chunkStart, entryStart);
                            const std::uint64_t writeEnd = std::min<std::uint64_t>(chunkEnd, entryEnd);
                            const std::uint64_t rel = writeStart - chunkStart;
                            const size_t writeSize = static_cast<size_t>(writeEnd - writeStart);
                            const std::uint64_t entryRel = writeStart - entryStart;

                            if (!EnsureEntryStarted(entry))
                                return false;
                            if (!WriteEntryData(entry, data + rel, writeSize, entryRel))
                                return false;
                        }

                        return true;
                    }
                    catch (...)
                    {
                        return false;
                    }
                }

                // ticket import - CommitLatest already fires when the cnmt NCA
                // finishes, this is just cleanup
                void Finalize()
                {
                    std::unordered_map<std::string, std::vector<std::uint8_t>> ticketsByBase;
                    std::unordered_map<std::string, std::vector<std::uint8_t>> certsByBase;
                    for (const auto& entry : m_entries)
                    {
                        const auto tikPos = entry.name.rfind(".tik");
                        if (tikPos != std::string::npos && tikPos + 4 == entry.name.size())
                            ticketsByBase[entry.name.substr(0, tikPos)] = entry.ticketBuf;
                        const auto certPos = entry.name.rfind(".cert");
                        if (certPos != std::string::npos && certPos + 5 == entry.name.size())
                            certsByBase[entry.name.substr(0, certPos)] = entry.certBuf;
                    }

                    std::unordered_set<std::string> baseNames;
                    for (const auto& e : ticketsByBase) baseNames.insert(e.first);
                    for (const auto& e : certsByBase) baseNames.insert(e.first);

                    for (const auto& base : baseNames)
                    {
                        const auto tikIt = ticketsByBase.find(base);
                        const auto certIt = certsByBase.find(base);
                        if (tikIt == ticketsByBase.end() || certIt == certsByBase.end()) continue;
                        if (tikIt->second.empty() || certIt->second.empty()) continue;
                        try
                        {
                            ASSERT_OK(esImportTicket(
                                tikIt->second.data(), tikIt->second.size(),
                                certIt->second.data(), certIt->second.size()),
                                "Failed to import ticket");
                        }
                        catch (...) { /* non-fatal: some titles ship without tickets */ }
                    }
                }

                bool HasParsedAnyEntries() const { return !m_entries.empty(); }

                // 0 until the header's parsed - after that it's the real total
                // size (header + every entry), needed for multi-part progress
                // since one part's Content-Length is only a fraction of it
                std::uint64_t GetTotalExpectedSize() const
                {
                    if (!m_headerParsed) return 0;
                    std::uint64_t total = m_headerBytes.size();
                    for (const auto& e : m_entries) total += e.size;
                    return total;
                }

                bool AllNcaEntriesComplete() const
                {
                    for (const auto& e : m_entries)
                        if (e.isNca && !e.complete) return false;
                    return true;
                }

            private:
                static constexpr std::uint64_t kMaxHeaderScan = 0x20000; // 128KB is far more than any real PFS0 header needs

                bool ParseHeaderIfReady()
                {
                    if (m_headerParsed) return true;
                    if (m_headerBytes.size() < sizeof(tin::install::PFS0BaseHeader)) return false;

                    const auto* base = reinterpret_cast<const tin::install::PFS0BaseHeader*>(m_headerBytes.data());
                    if (base->magic != 0x30534650 /* "PFS0" */)
                        THROW_FORMAT("Invalid PFS0 magic in streamed NSP\n");

                    const size_t headerSize = sizeof(tin::install::PFS0BaseHeader) +
                        static_cast<size_t>(base->numFiles) * sizeof(tin::install::PFS0FileEntry) + base->stringTableSize;

                    if (m_headerBytes.size() < headerSize)
                        return false; // need more bytes yet

                    m_headerBytes.resize(headerSize);
                    m_headerParsed = true;

                    m_entries.clear();
                    m_hintIndex = 0;
                    for (u32 i = 0; i < base->numFiles; i++)
                    {
                        const auto* entry = reinterpret_cast<const tin::install::PFS0FileEntry*>(
                            m_headerBytes.data() + sizeof(tin::install::PFS0BaseHeader) + i * sizeof(tin::install::PFS0FileEntry));
                        const char* name = reinterpret_cast<const char*>(
                            m_headerBytes.data() + sizeof(tin::install::PFS0BaseHeader) +
                            base->numFiles * sizeof(tin::install::PFS0FileEntry) + entry->stringTableOffset);

                        NspEntryState st;
                        st.name = name;
                        st.dataOffset = headerSize + entry->dataOffset;
                        st.size = entry->fileSize;
                        st.isNca = st.name.find(".nca") != std::string::npos || st.name.find(".ncz") != std::string::npos;
                        st.isCnmt = st.name.find(".cnmt.nca") != std::string::npos || st.name.find(".cnmt.ncz") != std::string::npos;
                        if (st.isNca && st.name.size() >= 32)
                            st.ncaId = tin::util::GetNcaIdFromString(st.name.substr(0, 32));
                        m_entries.emplace_back(std::move(st));
                    }
                    std::sort(m_entries.begin(), m_entries.end(),
                        [](const NspEntryState& a, const NspEntryState& b) { return a.dataOffset < b.dataOffset; });

                    return true;
                }

                bool EnsureEntryStarted(NspEntryState& entry)
                {
                    if (entry.started) return true;
                    if (!entry.isNca) { entry.started = true; return true; }

                    entry.storage = std::make_shared<nx::ncm::ContentStorage>(m_destStorage);
                    try { entry.storage->DeletePlaceholder(*(NcmPlaceHolderId*)&entry.ncaId); } catch (...) {}
                    entry.ncaWriter = std::make_unique<NcaWriter>(entry.ncaId, entry.storage);
                    entry.started = true;
                    return true;
                }

                bool WriteEntryData(NspEntryState& entry, const std::uint8_t* data, size_t size, std::uint64_t relOffset)
                {
                    if (relOffset != entry.written)
                    {
                        if (relOffset < entry.written)
                        {
                            const size_t overlap = static_cast<size_t>(std::min<std::uint64_t>(entry.written - relOffset, size));
                            data += overlap;
                            size -= overlap;
                            relOffset += overlap;
                            if (size == 0) return true;
                        }
                        else
                        {
                            return false; // gap - shouldn't happen on a sequential stream
                        }
                    }

                    if (entry.name.find(".tik") != std::string::npos)
                    {
                        entry.ticketBuf.insert(entry.ticketBuf.end(), data, data + size);
                        entry.written += size;
                        if (entry.written >= entry.size) entry.complete = true;
                        return true;
                    }
                    if (entry.name.find(".cert") != std::string::npos)
                    {
                        entry.certBuf.insert(entry.certBuf.end(), data, data + size);
                        entry.written += size;
                        if (entry.written >= entry.size) entry.complete = true;
                        return true;
                    }
                    if (!entry.isNca)
                    {
                        entry.written += size;
                        if (entry.written >= entry.size) entry.complete = true;
                        return true;
                    }

                    if (!entry.ncaWriter) return false;
                    entry.ncaWriter->write(data, size);
                    entry.written += size;
                    if (entry.written >= entry.size)
                    {
                        entry.ncaWriter->close();
                        try
                        {
                            entry.storage->Register(*(NcmPlaceHolderId*)&entry.ncaId, entry.ncaId);
                            entry.storage->DeletePlaceholder(*(NcmPlaceHolderId*)&entry.ncaId);
                        }
                        catch (...) {}
                        entry.complete = true;
                        if (entry.isCnmt)
                            CommitCnmt(entry);
                        // free these up now - big NSPs with lots of NCAs can chew
                        // through the kernel handle table otherwise
                        entry.ncaWriter = nullptr;
                        entry.storage = nullptr;
                    }
                    return true;
                }

                void CommitCnmt(NspEntryState& entry)
                {
                    if (!entry.isCnmt || !entry.storage) return;
                    try
                    {
                        std::string cnmtPath = entry.storage->GetPath(entry.ncaId);
                        nx::ncm::ContentMeta meta = tin::util::GetContentMetaFromNCA(cnmtPath);
                        NcmContentInfo cnmtInfo{};
                        cnmtInfo.content_id = entry.ncaId;
                        ncmU64ToContentInfoSize(entry.size & 0xFFFFFFFFFFFF, &cnmtInfo);
                        cnmtInfo.content_type = NcmContentType_Meta;
                        m_helper->AddContentMeta(meta, cnmtInfo);
                        m_helper->CommitLatest();
                    }
                    catch (...)
                    {
                        THROW_FORMAT("Failed to read/commit CNMT from streamed NSP\n");
                    }
                }

                NcmStorageId m_destStorage;
                std::vector<std::uint8_t> m_headerBytes;
                bool m_headerParsed = false;
                std::vector<NspEntryState> m_entries;
                size_t m_hintIndex = 0;
                std::unique_ptr<HttpStreamInstallHelper> m_helper;
        };

        struct CurlStreamState
        {
            HttpNspDemuxer* demuxer = nullptr;
            std::uint64_t runningOffset = 0;
            bool feedFailed = false;
            u64 lastDataTick = 0;
            u64 lastReportTick = 0;
            u64 startTick = 0;
            std::uint64_t totalBytes = 0;
            std::uint64_t bytesSinceLastReport = 0;
            double emaSpeedMBps = 0.0;
        };

        size_t CurlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
        {
            auto* state = static_cast<CurlStreamState*>(userdata);
            const size_t bytes = size * nmemb;

            if (inst::ui::instPage::isInstallCancelRequested())
                return 0; // aborts the transfer

            state->lastDataTick = armGetSystemTick();

            if (!state->demuxer->Feed(reinterpret_cast<const std::uint8_t*>(ptr), bytes, state->runningOffset))
            {
                state->feedFailed = true;
                return 0; // aborts the transfer
            }
            state->runningOffset += bytes;
            // curl's write callback fires way more than twice a second with small
            // chunks, so accumulate between reports or the speed calc below just
            // measures one tiny chunk and rounds down to 0.0 every time
            state->bytesSinceLastReport += bytes;

            const u64 now = armGetSystemTick();
            const u64 freq = armGetSystemTickFreq();
            if (state->lastReportTick == 0) state->lastReportTick = now;
            if (now - state->lastReportTick >= freq / 2)
            {
                const double elapsedSec = (double)(now - state->lastReportTick) / (double)freq;
                const double mb = (double)state->bytesSinceLastReport / (1024.0 * 1024.0);
                // rough smoothing, it's just for the UI
                const double instSpeed = elapsedSec > 0.0 ? (mb / elapsedSec) : 0.0;
                state->emaSpeedMBps = state->emaSpeedMBps <= 0.0
                    ? instSpeed
                    : (state->emaSpeedMBps * 0.7) + (instSpeed * 0.3);
                state->lastReportTick = now;
                state->bytesSinceLastReport = 0;

                const double sizeMB = (double)state->runningOffset / (1024.0 * 1024.0);
                const double totalMB = (double)state->totalBytes / (1024.0 * 1024.0);
                int pct = state->totalBytes > 0
                    ? (int)(((double)state->runningOffset / (double)state->totalBytes) * 100.0)
                    : 0;
                if (pct > 100) pct = 100;

                char detail[160];
                std::snprintf(detail, sizeof(detail), "Downloading & Installing %.1f / %.1f MB (%d%%) \xE2\x80\xA2 %.1f MB/s",
                    sizeMB, totalMB, pct, state->emaSpeedMBps);
                inst::ui::instPage::setProgressDetailText(detail);
                inst::ui::instPage::setInstBarPerc((double)pct);
            }

            return bytes;
        }

        // unlike the write callback, this fires on a timer even with no data
        // flowing - it's what actually catches a connection that went quiet
        int CurlProgressCallback(void* userdata, curl_off_t dltotal, curl_off_t, curl_off_t, curl_off_t)
        {
            auto* state = static_cast<CurlStreamState*>(userdata);
            const std::uint64_t knownTotal = state->demuxer ? state->demuxer->GetTotalExpectedSize() : 0;
            if (knownTotal > 0)
                state->totalBytes = knownTotal;
            else if (dltotal > 0)
                state->totalBytes = static_cast<std::uint64_t>(dltotal);

            if (inst::ui::instPage::isInstallCancelRequested())
                return 1;

            static constexpr u64 kIdleTimeoutSeconds = 20;
            const u64 now = armGetSystemTick();
            const u64 freq = armGetSystemTickFreq();
            if (state->lastDataTick == 0)
                state->lastDataTick = state->startTick;
            if ((now - state->lastDataTick) >= freq * kIdleTimeoutSeconds)
                return 1; // abort - CURLE_ABORTED_BY_CALLBACK

            return 0;
        }
    } // namespace

    void InstallNspHttpStreamSequential(
        const std::string& url,
        const std::string& basicAuthUser,
        const std::string& basicAuthPass,
        NcmStorageId destStorageId,
        bool ignoreReqFirmVersion)
    {
        HttpNspDemuxer demuxer(destStorageId, ignoreReqFirmVersion);

        std::string requestUrl = url;
        if (!basicAuthUser.empty())
        {
            const size_t schemeEnd = requestUrl.find("://");
            if (schemeEnd != std::string::npos)
                requestUrl = requestUrl.substr(0, schemeEnd + 3) + basicAuthUser + ":" + basicAuthPass + "@" + requestUrl.substr(schemeEnd + 3);
        }

        CURL* curl = curl_easy_init();
        if (!curl)
            THROW_FORMAT("Failed to initialize curl for streamed NSP install\n");

        CurlStreamState state;
        state.demuxer = &demuxer;
        state.startTick = armGetSystemTick();
        state.lastDataTick = state.startTick;

        curl_easy_setopt(curl, CURLOPT_URL, requestUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "");
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 45L);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &CurlWriteCallback);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &CurlProgressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);

        inst::ui::instPage::setInstInfoText("Downloading and installing...");
        inst::ui::instPage::setInstBarPerc(0);

        const CURLcode rc = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_easy_cleanup(curl);

        if (inst::ui::instPage::isInstallCancelRequested())
            THROW_FORMAT("Installation canceled.\n");

        if (state.feedFailed)
            THROW_FORMAT("Failed while writing streamed NSP data to NCM storage\n");

        if (rc != CURLE_OK)
            THROW_FORMAT("Streamed NSP download failed: %s (http=%ld)\n", curl_easy_strerror(rc), httpCode);

        if (!demuxer.HasParsedAnyEntries())
            THROW_FORMAT("Streamed NSP had no valid PFS0 entries\n");

        if (!demuxer.AllNcaEntriesComplete())
            THROW_FORMAT("Streamed NSP download ended before all content finished writing\n");

        demuxer.Finalize();

        inst::ui::instPage::setInstBarPerc(100);
        inst::ui::instPage::setProgressDetailText("Downloading & Installing 100%");
    }

    void InstallNspHttpStreamJbodParts(
        const std::vector<std::string>& partUrls,
        const std::string& basicAuthUser,
        const std::string& basicAuthPass,
        NcmStorageId destStorageId,
        bool ignoreReqFirmVersion)
    {
        if (partUrls.empty())
            THROW_FORMAT("No parts to install\n");

        HttpNspDemuxer demuxer(destStorageId, ignoreReqFirmVersion);

        CurlStreamState state;
        state.demuxer = &demuxer;
        state.startTick = armGetSystemTick();
        state.lastDataTick = state.startTick;

        inst::ui::instPage::setInstInfoText("Downloading and installing...");
        inst::ui::instPage::setInstBarPerc(0);

        // each part is its own plain full download, no Range header - offset
        // never resets between parts so the demuxer still sees one file
        for (size_t partIdx = 0; partIdx < partUrls.size(); partIdx++)
        {
            std::string requestUrl = partUrls[partIdx];
            if (!basicAuthUser.empty())
            {
                const size_t schemeEnd = requestUrl.find("://");
                if (schemeEnd != std::string::npos)
                    requestUrl = requestUrl.substr(0, schemeEnd + 3) + basicAuthUser + ":" + basicAuthPass + "@" + requestUrl.substr(schemeEnd + 3);
            }

            CURL* curl = curl_easy_init();
            if (!curl)
                THROW_FORMAT("Failed to initialize curl for streamed NSP part %zu\n", partIdx);

            // fresh idle clock for each part's own connection
            state.lastDataTick = armGetSystemTick();

            curl_easy_setopt(curl, CURLOPT_URL, requestUrl.c_str());
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "");
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
            curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
            curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 45L);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &CurlWriteCallback);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &CurlProgressCallback);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);

            const CURLcode rc = curl_easy_perform(curl);
            long httpCode = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
            curl_easy_cleanup(curl);

            if (inst::ui::instPage::isInstallCancelRequested())
                THROW_FORMAT("Installation canceled.\n");

            if (state.feedFailed)
                THROW_FORMAT("Failed while writing streamed NSP data to NCM storage\n");

            if (rc != CURLE_OK)
                THROW_FORMAT("Streamed NSP part %zu download failed: %s (http=%ld)\n",
                    partIdx, curl_easy_strerror(rc), httpCode);
        }

        if (!demuxer.HasParsedAnyEntries())
            THROW_FORMAT("Streamed NSP had no valid PFS0 entries\n");

        if (!demuxer.AllNcaEntriesComplete())
            THROW_FORMAT("Streamed NSP download ended before all content finished writing\n");

        demuxer.Finalize();

        inst::ui::instPage::setInstBarPerc(100);
        inst::ui::instPage::setProgressDetailText("Downloading & Installing 100%");
    }
}
