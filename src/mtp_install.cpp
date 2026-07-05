#include "mtp_install.hpp"

#include "mtp_install.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "switch.h"
#include <cstring>

#include "install/install_nsp.hpp"
#include "install/install_xci.hpp"
#include "install/install.hpp"
#include "install/nca.hpp"
#include "install/pfs0.hpp"
#include "install/hfs0.hpp"
#include "data/byte_buffer.hpp"
#include "nx/nca_writer.h"
#include "nx/ncm.hpp"
#include "util/config.hpp"
#include "util/error.hpp"
#include "util/file_util.hpp"
#include "util/title_util.hpp"
#include "util/lang.hpp"
#include "util/util.hpp"
#include "ui/MainApplication.hpp"
#include "ui/instPage.hpp"

namespace inst::ui {
    extern MainApplication* mainApp;
}

namespace inst::mtp {
namespace {

std::atomic<bool> g_stream_active{false};
std::atomic<bool> g_stream_complete{false};
std::atomic<std::uint64_t> g_stream_total{0};
std::atomic<std::uint64_t> g_stream_received{0};
std::atomic<std::uint64_t> g_stream_title_id{0};
std::mutex g_stream_mutex;
std::string g_stream_name;
std::mutex g_stream_trace_mutex;
std::atomic<u64> g_stream_trace_count{0};
std::atomic<u64> g_stream_write_calls{0};
std::atomic<bool> g_stream_trace_enabled{false};
constexpr u64 kStreamTraceMaxLines = 60000;
constexpr const char* kStreamTracePath = "sdmc:/switch/CyberFoil/mtp_install_debug.log";
constexpr const char* kStreamTraceEnablePath = "sdmc:/switch/CyberFoil/mtp_install_debug.enable";

bool IsStreamTraceEnabled() {
    return g_stream_trace_enabled.load(std::memory_order_relaxed);
}

u64 TicksToMs(u64 ticks) {
    const u64 freq = armGetSystemTickFreq();
    if (freq == 0) {
        return 0;
    }
    return (ticks * 1000ULL) / freq;
}

void StreamTrace(const char* fmt, ...) {
    if (!IsStreamTraceEnabled()) {
        return;
    }
    const u64 idx = g_stream_trace_count.fetch_add(1, std::memory_order_relaxed);
    if (idx >= kStreamTraceMaxLines) {
        return;
    }

    char msg[512] = {};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(g_stream_trace_mutex);
    FILE* f = std::fopen(kStreamTracePath, "ab");
    if (!f) {
        return;
    }
    std::fprintf(f, "%llu %s\n", static_cast<unsigned long long>(idx), msg);
    std::fclose(f);
}

void ResetStreamTrace() {
    if (!IsStreamTraceEnabled()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_stream_trace_mutex);
    std::remove(kStreamTracePath);
    g_stream_trace_count.store(0, std::memory_order_relaxed);
    g_stream_write_calls.store(0, std::memory_order_relaxed);
}

class StreamInstaller {
public:
    StreamInstaller() = default;
    virtual ~StreamInstaller() = default;
    virtual bool Feed(const void* buf, size_t size, std::uint64_t offset) = 0;
    virtual bool Finalize() = 0;
};

class MtpNspStream final : public StreamInstaller {
public:
    explicit MtpNspStream(std::uint64_t total_size, NcmStorageId dest_storage);

    bool Feed(const void* buf, size_t size, std::uint64_t offset) override;
    bool Finalize() override;

private:
    struct EntryState {
        std::string name;
        NcmContentId nca_id{};
        std::uint64_t data_offset = 0;
        std::uint64_t size = 0;
        std::uint64_t written = 0;
        bool started = false;
        bool complete = false;
        bool is_nca = false;
        bool is_cnmt = false;
        std::shared_ptr<nx::ncm::ContentStorage> storage;
        std::unique_ptr<NcaWriter> nca_writer;
        std::vector<std::uint8_t> ticket_buf;
        std::vector<std::uint8_t> cert_buf;
    };

    bool ParseHeaderIfReady();
    bool EnsureEntryStarted(EntryState& entry);
    bool WriteEntryData(EntryState& entry, const std::uint8_t* data, size_t size, std::uint64_t rel_offset);
    bool CommitCnmt(EntryState& entry);

    NcmStorageId m_dest_storage = NcmStorageId_SdCard;
    std::uint64_t m_total_size = 0;
    std::uint64_t m_received = 0;
    std::vector<std::uint8_t> m_header_bytes;
    std::vector<EntryState> m_entries;
    size_t m_hint_index = 0;
    bool m_header_parsed = false;
    std::unique_ptr<class StreamInstallHelper> m_helper;
};

// XCI/XCZ streaming uses the pull-based installer below.

std::unique_ptr<StreamInstaller> g_stream;

class StreamInstallHelper final : public tin::install::Install {
public:
    StreamInstallHelper(NcmStorageId dest_storage, bool ignore_req)
        : Install(dest_storage, ignore_req) {}

    void AddContentMeta(const nx::ncm::ContentMeta& meta, const NcmContentInfo& info) {
        m_contentMeta.push_back(meta);
        m_cnmt_infos.push_back(info);
    }

    void CommitLatest() {
        if (m_contentMeta.empty()) return;
        const size_t idx = m_contentMeta.size() - 1;
        tin::data::ByteBuffer install_buf;
        m_contentMeta[idx].GetInstallContentMeta(install_buf, m_cnmt_infos[idx], m_ignoreReqFirmVersion);
        InstallContentMetaRecords(install_buf, idx);
        InstallApplicationRecord(idx);
    }

    void CommitAll() {
        for (size_t i = 0; i < m_contentMeta.size(); i++) {
            tin::data::ByteBuffer install_buf;
            m_contentMeta[i].GetInstallContentMeta(install_buf, m_cnmt_infos[i], m_ignoreReqFirmVersion);
            InstallContentMetaRecords(install_buf, i);
            InstallApplicationRecord(i);
        }
    }

private:
    std::vector<NcmContentInfo> m_cnmt_infos;

    std::vector<std::tuple<nx::ncm::ContentMeta, NcmContentInfo>> ReadCNMT() override { return {}; }
    void InstallTicketCert() override {}
    void InstallNCA(const NcmContentId& /*ncaId*/) override {}
};

bool IsXciName(const std::string& name) {
    auto pos = name.find_last_of('.');
    if (pos == std::string::npos) return false;
    auto ext = name.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".xci" || ext == ".xcz";
}

bool IsNspName(const std::string& name) {
    auto pos = name.find_last_of('.');
    if (pos == std::string::npos) return false;
    auto ext = name.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".nsp" || ext == ".nsz";
}

MtpNspStream::MtpNspStream(std::uint64_t total_size, NcmStorageId dest_storage)
    : m_dest_storage(dest_storage), m_total_size(total_size)
{
    m_helper = std::make_unique<StreamInstallHelper>(dest_storage, inst::config::ignoreReqVers);
}


bool MtpNspStream::ParseHeaderIfReady()
{
    if (m_header_parsed) return true;
    if (m_header_bytes.size() < sizeof(tin::install::PFS0BaseHeader)) return false;

    const auto* base = reinterpret_cast<const tin::install::PFS0BaseHeader*>(m_header_bytes.data());
    if (base->magic != 0x30534650) {
        StreamTrace("NSP ParseHeader invalid magic=0x%08x", base->magic);
        THROW_FORMAT("Invalid PFS0 magic");
    }

    const size_t header_size = sizeof(tin::install::PFS0BaseHeader) +
        base->numFiles * sizeof(tin::install::PFS0FileEntry) + base->stringTableSize;

    if (m_header_bytes.size() < header_size) return false;

    m_header_bytes.resize(header_size);
    m_header_parsed = true;

    m_entries.clear();
    m_hint_index = 0;
    for (u32 i = 0; i < base->numFiles; i++) {
        const auto* entry = reinterpret_cast<const tin::install::PFS0FileEntry*>(
            m_header_bytes.data() + sizeof(tin::install::PFS0BaseHeader) + i * sizeof(tin::install::PFS0FileEntry));
        const char* name = reinterpret_cast<const char*>(
            m_header_bytes.data() + sizeof(tin::install::PFS0BaseHeader) +
            base->numFiles * sizeof(tin::install::PFS0FileEntry) + entry->stringTableOffset);

        EntryState st;
        st.name = name;
        st.data_offset = header_size + entry->dataOffset;
        st.size = entry->fileSize;
        st.is_nca = st.name.find(".nca") != std::string::npos || st.name.find(".ncz") != std::string::npos;
        st.is_cnmt = st.name.find(".cnmt.nca") != std::string::npos || st.name.find(".cnmt.ncz") != std::string::npos;
        if (st.is_nca && st.name.size() >= 32) {
            st.nca_id = tin::util::GetNcaIdFromString(st.name.substr(0, 32));
        }
        m_entries.emplace_back(std::move(st));
    }
    std::sort(m_entries.begin(), m_entries.end(), [](const EntryState& a, const EntryState& b) {
        return a.data_offset < b.data_offset;
    });
    StreamTrace("NSP ParseHeader parsed files=%u header=%zu total=%llu",
        static_cast<unsigned>(base->numFiles),
        header_size,
        static_cast<unsigned long long>(m_total_size));

    return true;
}

bool MtpNspStream::EnsureEntryStarted(EntryState& entry)
{
    if (entry.started) return true;
    if (!entry.is_nca) {
        entry.started = true;
        return true;
    }

    entry.storage = std::make_shared<nx::ncm::ContentStorage>(m_dest_storage);
    try {
        entry.storage->DeletePlaceholder(*(NcmPlaceHolderId*)&entry.nca_id);
    } catch (...) {}
    entry.nca_writer = std::make_unique<NcaWriter>(entry.nca_id, entry.storage);
    entry.started = true;
    StreamTrace("NSP EntryStart name='%s' size=%llu cnmt=%d",
        entry.name.c_str(),
        static_cast<unsigned long long>(entry.size),
        entry.is_cnmt ? 1 : 0);
    return true;
}

bool MtpNspStream::CommitCnmt(EntryState& entry)
{
    if (!entry.is_cnmt || !entry.storage) return false;

    try {
        std::string cnmt_path = entry.storage->GetPath(entry.nca_id);
        nx::ncm::ContentMeta meta = tin::util::GetContentMetaFromNCA(cnmt_path);
        {
            const auto key = meta.GetContentMetaKey();
            const auto base_id = tin::util::GetBaseTitleId(key.id, static_cast<NcmContentMetaType>(key.type));
            g_stream_title_id.store(base_id, std::memory_order_relaxed);
        }
        NcmContentInfo cnmt_info{};
        cnmt_info.content_id = entry.nca_id;
        ncmU64ToContentInfoSize(entry.size & 0xFFFFFFFFFFFF, &cnmt_info);
        cnmt_info.content_type = NcmContentType_Meta;
        m_helper->AddContentMeta(meta, cnmt_info);
        m_helper->CommitLatest();
        StreamTrace("NSP CommitCnmt ok name='%s' size=%llu",
            entry.name.c_str(),
            static_cast<unsigned long long>(entry.size));
        return true;
    } catch (...) {
        StreamTrace("NSP CommitCnmt fail name='%s'", entry.name.c_str());
        return false;
    }
}

bool MtpNspStream::WriteEntryData(EntryState& entry, const std::uint8_t* data, size_t size, std::uint64_t rel_offset)
{
    if (rel_offset != entry.written) {
        // Host retries/overlaps can happen near transfer end; accept already-written prefix.
        if (rel_offset < entry.written) {
            const auto overlap = static_cast<size_t>(std::min<std::uint64_t>(entry.written - rel_offset, size));
            data += overlap;
            size -= overlap;
            rel_offset += overlap;
            if (size == 0) {
                return true;
            }
        } else {
            StreamTrace("NSP WriteEntryData gap name='%s' rel=%llu written=%llu size=%zu",
                entry.name.c_str(),
                static_cast<unsigned long long>(rel_offset),
                static_cast<unsigned long long>(entry.written),
                size);
            return false;
        }
    }

    if (entry.name.find(".tik") != std::string::npos) {
        entry.ticket_buf.insert(entry.ticket_buf.end(), data, data + size);
        entry.written += size;
        if (entry.written >= entry.size) {
            entry.complete = true;
        }
        return true;
    }
    if (entry.name.find(".cert") != std::string::npos) {
        entry.cert_buf.insert(entry.cert_buf.end(), data, data + size);
        entry.written += size;
        if (entry.written >= entry.size) {
            entry.complete = true;
        }
        return true;
    }

    if (!entry.is_nca) {
        // Non-NCA metadata payloads (eg xml) are not needed by installer logic.
        entry.written += size;
        if (entry.written >= entry.size) {
            entry.complete = true;
        }
        return true;
    }

    if (!entry.nca_writer) return false;
    entry.nca_writer->write(data, size);
    entry.written += size;
    if (entry.written >= entry.size) {
        entry.nca_writer->close();
        try {
            entry.storage->Register(*(NcmPlaceHolderId*)&entry.nca_id, entry.nca_id);
            entry.storage->DeletePlaceholder(*(NcmPlaceHolderId*)&entry.nca_id);
        } catch (...) {}
        entry.complete = true;
        if (entry.is_cnmt) {
            CommitCnmt(entry);
        }
        StreamTrace("NSP EntryComplete name='%s' size=%llu",
            entry.name.c_str(),
            static_cast<unsigned long long>(entry.size));
    }
    return true;
}

bool MtpNspStream::Feed(const void* buf, size_t size, std::uint64_t offset)
{
    try {
    const auto* data = static_cast<const std::uint8_t*>(buf);
    const auto chunk_start = offset;
    const auto chunk_end = offset + size;
    if (offset == m_received) {
        m_received += size;
    }

    if (m_total_size) {
        const auto current = g_stream_received.load(std::memory_order_relaxed);
        if (m_received > current) {
            g_stream_received.store(m_received, std::memory_order_relaxed);
        }
    }
    if (!m_header_parsed && offset <= 0x20000) {
        const auto start = offset;
        const auto end = std::min<std::uint64_t>(offset + size, 0x20000);
        const auto rel = start;
        const auto len = static_cast<size_t>(end - start);
        if (m_header_bytes.size() < rel + len) {
            m_header_bytes.resize(rel + len);
        }
        std::memcpy(m_header_bytes.data() + rel, data, len);
    }

    if (!ParseHeaderIfReady()) {
        return true;
    }

    if (m_entries.empty()) {
        return true;
    }

    if (m_hint_index >= m_entries.size()) {
        m_hint_index = 0;
    }

    while (m_hint_index > 0 && chunk_start < m_entries[m_hint_index].data_offset) {
        --m_hint_index;
    }
    while (m_hint_index < m_entries.size()) {
        const auto end = m_entries[m_hint_index].data_offset + m_entries[m_hint_index].size;
        if (chunk_start < end) break;
        ++m_hint_index;
    }

    for (size_t i = m_hint_index; i < m_entries.size(); ++i) {
        auto& entry = m_entries[i];
        const auto entry_start = entry.data_offset;
        const auto entry_end = entry.data_offset + entry.size;

        if (chunk_end <= entry_start) break;
        if (chunk_start >= entry_end) continue;

        const auto write_start = std::max<std::uint64_t>(chunk_start, entry_start);
        const auto write_end = std::min<std::uint64_t>(chunk_end, entry_end);
        const auto rel = write_start - chunk_start;
        const auto write_size = static_cast<size_t>(write_end - write_start);

        if (!EnsureEntryStarted(entry)) {
            StreamTrace("NSP EnsureEntryStarted fail name='%s'", entry.name.c_str());
            return false;
        }
        const auto entry_rel = write_start - entry_start;
        if (!WriteEntryData(entry, data + rel, write_size, entry_rel)) {
            StreamTrace("NSP WriteEntryData fail name='%s' entry_rel=%llu write_size=%zu",
                entry.name.c_str(),
                static_cast<unsigned long long>(entry_rel),
                write_size);
            return false;
        }
    }

    return true;
    } catch (...) {
        StreamTrace("NSP Feed exception off=%llu size=%zu",
            static_cast<unsigned long long>(offset), size);
        return false;
    }
}

bool MtpNspStream::Finalize()
{
    if (!m_helper) return true;
    StreamTrace("NSP Finalize begin entries=%zu", m_entries.size());
    bool ok = true;

    std::unordered_map<std::string, std::vector<std::uint8_t>> tickets_by_base;
    std::unordered_map<std::string, std::vector<std::uint8_t>> certs_by_base;
    for (const auto& entry : m_entries) {
        const auto tik_pos = entry.name.rfind(".tik");
        if (tik_pos != std::string::npos && tik_pos + 4 == entry.name.size()) {
            tickets_by_base[entry.name.substr(0, tik_pos)] = entry.ticket_buf;
        }
        const auto cert_pos = entry.name.rfind(".cert");
        if (cert_pos != std::string::npos && cert_pos + 5 == entry.name.size()) {
            certs_by_base[entry.name.substr(0, cert_pos)] = entry.cert_buf;
        }
    }

    std::unordered_set<std::string> base_names;
    base_names.reserve(tickets_by_base.size() + certs_by_base.size());
    for (const auto& entry : tickets_by_base) {
        base_names.insert(entry.first);
    }
    for (const auto& entry : certs_by_base) {
        base_names.insert(entry.first);
    }

    for (const auto& base : base_names) {
        const auto tik_it = tickets_by_base.find(base);
        const auto cert_it = certs_by_base.find(base);
        if (tik_it == tickets_by_base.end() || cert_it == certs_by_base.end()) {
            continue;
        }
        if (!tik_it->second.empty() && !cert_it->second.empty()) {
            const Result rc = esImportTicket(
                tik_it->second.data(), tik_it->second.size(),
                cert_it->second.data(), cert_it->second.size());
            if (R_FAILED(rc)) {
                LOG_DEBUG("MTP finalize: ticket import failed (0x%08x)\n", rc);
                StreamTrace("NSP Finalize ticket import fail base='%s' rc=0x%08x", base.c_str(), rc);
                ok = false;
            }
        }
    }

    try {
        m_helper->CommitAll();
    } catch (const std::exception& e) {
        LOG_DEBUG("MTP finalize: CommitAll failed: %s\n", e.what());
        StreamTrace("NSP Finalize CommitAll exception='%s'", e.what());
        ok = false;
    } catch (...) {
        LOG_DEBUG("MTP finalize: CommitAll failed with unknown exception\n");
        StreamTrace("NSP Finalize CommitAll unknown exception");
        ok = false;
    }
    StreamTrace("NSP Finalize end ok=%d", ok ? 1 : 0);
    return ok;
}

class MtpStreamBuffer {
public:
    explicit MtpStreamBuffer(size_t max_size) : m_max_size(max_size) {}

    bool Push(const void* buf, size_t size) {
        const auto* data = static_cast<const std::uint8_t*>(buf);
        while (size > 0) {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_can_write.wait(lock, [&]() { return !m_active || m_buffer.size() < m_max_size; });
            if (!m_active) return false;

            const size_t writable = m_max_size - m_buffer.size();
            const size_t chunk = std::min<size_t>(size, writable);
            const size_t offset = m_buffer.size();
            m_buffer.resize(offset + chunk);
            std::memcpy(m_buffer.data() + offset, data, chunk);
            data += chunk;
            size -= chunk;
            lock.unlock();
            m_can_read.notify_one();
        }
        return true;
    }

    bool ReadChunk(void* buf, size_t size, u64* out_read) {
        auto* out = static_cast<std::uint8_t*>(buf);
        *out_read = 0;
        std::unique_lock<std::mutex> lock(m_mutex);
        while (m_active && m_buffer.empty()) {
            m_can_read.wait(lock);
        }
        if (!m_active && m_buffer.empty()) {
            return false;
        }

        const size_t chunk = std::min<size_t>(size, m_buffer.size());
        std::memcpy(out, m_buffer.data(), chunk);
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + chunk);
        *out_read = chunk;
        lock.unlock();
        m_can_write.notify_one();
        return true;
    }

    void Disable() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_active = false;
        m_can_read.notify_all();
        m_can_write.notify_all();
    }

    size_t GetBufferedSize() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_buffer.size();
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_can_read;
    std::condition_variable m_can_write;
    std::vector<std::uint8_t> m_buffer;
    size_t m_max_size = 0;
    bool m_active = true;
};

class MtpStreamSource {
public:
    explicit MtpStreamSource(MtpStreamBuffer& buffer) : m_buffer(buffer) {}

    Result Read(void* buf, s64 off, s64 size, u64* bytes_read) {
        if (off < m_offset) {
            StreamTrace("XCI SourceRead bad off=%lld cur=%lld size=%lld",
                static_cast<long long>(off),
                static_cast<long long>(m_offset),
                static_cast<long long>(size));
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        }

        auto* out = static_cast<std::uint8_t*>(buf);
        *bytes_read = 0;
        std::vector<std::uint8_t> temp(0x80000);

        while (size > 0) {
            if (off > m_offset) {
                const auto skip = static_cast<s64>(off - m_offset);
                const auto chunk = static_cast<size_t>(std::min<s64>(skip, static_cast<s64>(temp.size())));
                u64 read = 0;
                if (!m_buffer.ReadChunk(temp.data(), chunk, &read)) {
                    StreamTrace("XCI SourceRead skip failed off=%lld chunk=%zu", static_cast<long long>(off), chunk);
                    return KERNELRESULT(NotImplemented);
                }
                m_offset += static_cast<s64>(read);
                continue;
            }

            const auto chunk = static_cast<size_t>(std::min<s64>(size, static_cast<s64>(temp.size())));
            u64 read = 0;
            if (!m_buffer.ReadChunk(out, chunk, &read)) {
                StreamTrace("XCI SourceRead data failed off=%lld chunk=%zu", static_cast<long long>(off), chunk);
                return KERNELRESULT(NotImplemented);
            }
            *bytes_read += read;
            out += read;
            m_offset += static_cast<s64>(read);
            off += static_cast<s64>(read);
            size -= static_cast<s64>(read);
        }

        return 0;
    }

private:
    MtpStreamBuffer& m_buffer;
    s64 m_offset = 0;
};

struct StreamHfs0Header {
    u32 magic;
    u32 total_files;
    u32 string_table_size;
    u32 padding;
};

struct StreamHfs0FileTableEntry {
    u64 data_offset;
    u64 data_size;
    u32 name_offset;
    u32 hash_size;
    u64 padding;
    u8 hash[0x20];
};

struct StreamHfs0 {
    StreamHfs0Header header{};
    std::vector<StreamHfs0FileTableEntry> file_table{};
    std::vector<std::string> string_table{};
    s64 data_offset{};
};

struct StreamCollectionEntry {
    std::string name;
    u64 offset{};
    u64 size{};
};

static bool ReadHfs0Partition(MtpStreamSource& source, s64 off, StreamHfs0& out) {
    u64 bytes_read = 0;
    if (R_FAILED(source.Read(&out.header, off, sizeof(out.header), &bytes_read))) return false;
    if (out.header.magic != 0x30534648) return false;
    off += bytes_read;

    out.file_table.resize(out.header.total_files);
    const auto file_table_size = static_cast<s64>(out.file_table.size() * sizeof(StreamHfs0FileTableEntry));
    if (R_FAILED(source.Read(out.file_table.data(), off, file_table_size, &bytes_read))) return false;
    off += bytes_read;

    std::vector<char> string_table(out.header.string_table_size);
    if (R_FAILED(source.Read(string_table.data(), off, string_table.size(), &bytes_read))) return false;
    off += bytes_read;

    out.string_table.clear();
    out.string_table.reserve(out.header.total_files);
    for (u32 i = 0; i < out.header.total_files; i++) {
        out.string_table.emplace_back(string_table.data() + out.file_table[i].name_offset);
    }

    out.data_offset = off;
    return true;
}

static bool GetXciCollections(MtpStreamSource& source, std::vector<StreamCollectionEntry>& out) {
    StreamHfs0 root{};
    s64 root_offset = 0xF000;
    if (!ReadHfs0Partition(source, root_offset, root)) {
        root_offset = 0x10000;
        if (!ReadHfs0Partition(source, root_offset, root)) {
            return false;
        }
    }

    for (u32 i = 0; i < root.header.total_files; i++) {
        if (root.string_table[i] != "secure") continue;

        StreamHfs0 secure{};
        const auto secure_offset = root.data_offset + static_cast<s64>(root.file_table[i].data_offset);
        if (!ReadHfs0Partition(source, secure_offset, secure)) return false;

        out.clear();
        out.reserve(secure.header.total_files);
        for (u32 j = 0; j < secure.header.total_files; j++) {
            StreamCollectionEntry entry;
            entry.name = secure.string_table[j];
            entry.offset = static_cast<u64>(secure.data_offset + static_cast<s64>(secure.file_table[j].data_offset));
            entry.size = secure.file_table[j].data_size;
            out.emplace_back(std::move(entry));
        }
        return true;
    }

    return false;
}

class MtpXciStreamPull final : public StreamInstaller {
public:
    explicit MtpXciStreamPull(std::uint64_t total_size, NcmStorageId dest_storage)
        : m_dest_storage(dest_storage), m_total_size(total_size), m_buffer(8 * 1024 * 1024) {
        m_helper = std::make_unique<StreamInstallHelper>(dest_storage, inst::config::ignoreReqVers);
        StreamTrace("XCI ctor total=%llu storage=%u",
            static_cast<unsigned long long>(total_size),
            static_cast<unsigned>(dest_storage));
        m_thread = std::thread([this]() {
            MtpStreamSource source(m_buffer);
            const bool ok = InstallFromSource(source);
            m_ok.store(ok, std::memory_order_relaxed);
            m_done.store(true, std::memory_order_relaxed);
            StreamTrace("XCI worker done ok=%d received=%llu buffered=%zu",
                ok ? 1 : 0,
                static_cast<unsigned long long>(m_received),
                m_buffer.GetBufferedSize());
            m_buffer.Disable();
        });
    }

    ~MtpXciStreamPull() override {
        m_buffer.Disable();
        if (m_thread.joinable()) {
            m_thread.join();
        }
        StreamTrace("XCI dtor");
    }

    bool Feed(const void* buf, size_t size, std::uint64_t /*offset*/) override {
        if (size == 0) return true;
        const u64 t0 = armGetSystemTick();
        m_received += size;
        if (m_total_size) {
            const auto current = g_stream_received.load(std::memory_order_relaxed);
            if (m_received > current) {
                g_stream_received.store(m_received, std::memory_order_relaxed);
            }
        }
        const bool ok = m_buffer.Push(buf, size);
        const u64 dt_ms = TicksToMs(armGetSystemTick() - t0);
        if (!ok || dt_ms >= 250 || (m_received % (64ULL * 1024ULL * 1024ULL) < size)) {
            StreamTrace("XCI Feed size=%zu ok=%d dt_ms=%llu received=%llu buffered=%zu",
                size,
                ok ? 1 : 0,
                static_cast<unsigned long long>(dt_ms),
                static_cast<unsigned long long>(m_received),
                m_buffer.GetBufferedSize());
        }
        return ok;
    }

    bool Finalize() override {
        StreamTrace("XCI Finalize begin received=%llu buffered=%zu",
            static_cast<unsigned long long>(m_received),
            m_buffer.GetBufferedSize());
        m_buffer.Disable();
        if (m_thread.joinable()) {
            m_thread.join();
        }
        const bool ok = m_ok.load(std::memory_order_relaxed);
        StreamTrace("XCI Finalize end ok=%d done=%d",
            ok ? 1 : 0,
            m_done.load(std::memory_order_relaxed) ? 1 : 0);
        return ok;
    }

private:
    struct EntryState {
        std::string name;
        NcmContentId nca_id{};
        std::uint64_t size = 0;
        std::uint64_t written = 0;
        bool started = false;
        bool complete = false;
        bool is_nca = false;
        bool is_cnmt = false;
        std::shared_ptr<nx::ncm::ContentStorage> storage;
        std::unique_ptr<NcaWriter> nca_writer;
        std::vector<std::uint8_t> ticket_buf;
        std::vector<std::uint8_t> cert_buf;
    };

    bool EnsureEntryStarted(EntryState& entry) {
        if (entry.started) return true;
        if (!entry.is_nca) {
            entry.started = true;
            return true;
        }

        entry.storage = std::make_shared<nx::ncm::ContentStorage>(m_dest_storage);
        try {
            entry.storage->DeletePlaceholder(*(NcmPlaceHolderId*)&entry.nca_id);
        } catch (...) {}
        entry.nca_writer = std::make_unique<NcaWriter>(entry.nca_id, entry.storage);
        entry.started = true;
        return true;
    }

    bool CommitCnmt(EntryState& entry) {
        if (!entry.is_cnmt || !entry.storage) return false;

        try {
            std::string cnmt_path = entry.storage->GetPath(entry.nca_id);
            nx::ncm::ContentMeta meta = tin::util::GetContentMetaFromNCA(cnmt_path);
            {
                const auto key = meta.GetContentMetaKey();
                const auto base_id = tin::util::GetBaseTitleId(key.id, static_cast<NcmContentMetaType>(key.type));
                g_stream_title_id.store(base_id, std::memory_order_relaxed);
            }
            NcmContentInfo cnmt_info{};
            cnmt_info.content_id = entry.nca_id;
            ncmU64ToContentInfoSize(entry.size & 0xFFFFFFFFFFFF, &cnmt_info);
            cnmt_info.content_type = NcmContentType_Meta;
            m_helper->AddContentMeta(meta, cnmt_info);
            m_helper->CommitLatest();
            return true;
        } catch (...) {
            return false;
        }
    }

    bool WriteEntryData(EntryState& entry, const std::uint8_t* data, size_t size) {
        if (entry.name.find(".tik") != std::string::npos) {
            entry.ticket_buf.insert(entry.ticket_buf.end(), data, data + size);
            entry.written += size;
            if (entry.written >= entry.size) {
                entry.complete = true;
            }
            return true;
        }
        if (entry.name.find(".cert") != std::string::npos) {
            entry.cert_buf.insert(entry.cert_buf.end(), data, data + size);
            entry.written += size;
            if (entry.written >= entry.size) {
                entry.complete = true;
            }
            return true;
        }

        if (!entry.is_nca || !entry.nca_writer) return false;
        entry.nca_writer->write(data, size);
        entry.written += size;
        if (entry.written >= entry.size) {
            entry.nca_writer->close();
            try {
                entry.storage->Register(*(NcmPlaceHolderId*)&entry.nca_id, entry.nca_id);
                entry.storage->DeletePlaceholder(*(NcmPlaceHolderId*)&entry.nca_id);
            } catch (...) {}
            entry.complete = true;
            if (entry.is_cnmt) {
                CommitCnmt(entry);
            }
        }
        return true;
    }

    bool InstallFromSource(MtpStreamSource& source) {
        std::vector<StreamCollectionEntry> collections;
        if (!GetXciCollections(source, collections)) {
            StreamTrace("XCI InstallFromSource GetXciCollections failed");
            return false;
        }
        StreamTrace("XCI InstallFromSource collections=%zu", collections.size());

        std::sort(collections.begin(), collections.end(), [](const auto& a, const auto& b) {
            return a.offset < b.offset;
        });

        std::unordered_map<std::string, EntryState> entries;
        entries.reserve(collections.size());
        std::vector<std::uint8_t> buf(0x400000);

        for (const auto& collection : collections) {
            EntryState entry;
            entry.name = collection.name;
            entry.size = collection.size;
            entry.is_nca = entry.name.find(".nca") != std::string::npos || entry.name.find(".ncz") != std::string::npos;
            entry.is_cnmt = entry.name.find(".cnmt.nca") != std::string::npos || entry.name.find(".cnmt.ncz") != std::string::npos;
            if (entry.is_nca && entry.name.size() >= 32) {
                entry.nca_id = tin::util::GetNcaIdFromString(entry.name.substr(0, 32));
            }

            if (!EnsureEntryStarted(entry)) {
                StreamTrace("XCI EnsureEntryStarted fail name='%s'", entry.name.c_str());
                return false;
            }

            u64 remaining = collection.size;
            u64 offset = collection.offset;
            StreamTrace("XCI Collection start name='%s' off=%llu size=%llu",
                entry.name.c_str(),
                static_cast<unsigned long long>(collection.offset),
                static_cast<unsigned long long>(collection.size));
            while (remaining > 0) {
                const auto chunk = static_cast<size_t>(std::min<u64>(remaining, buf.size()));
                u64 bytes_read = 0;
                if (R_FAILED(source.Read(buf.data(), static_cast<s64>(offset), static_cast<s64>(chunk), &bytes_read))) {
                    StreamTrace("XCI Source.Read fail name='%s' off=%llu chunk=%zu",
                        entry.name.c_str(),
                        static_cast<unsigned long long>(offset),
                        chunk);
                    return false;
                }
                if (bytes_read == 0) {
                    StreamTrace("XCI Source.Read eof name='%s' off=%llu", entry.name.c_str(), static_cast<unsigned long long>(offset));
                    return false;
                }
                if (!WriteEntryData(entry, buf.data(), static_cast<size_t>(bytes_read))) {
                    StreamTrace("XCI WriteEntryData fail name='%s' bytes=%llu",
                        entry.name.c_str(),
                        static_cast<unsigned long long>(bytes_read));
                    return false;
                }
                offset += bytes_read;
                remaining -= bytes_read;
            }

            StreamTrace("XCI Collection done name='%s' size=%llu",
                entry.name.c_str(),
                static_cast<unsigned long long>(entry.size));
            entries.emplace(entry.name, std::move(entry));
        }

        for (auto& [name, entry] : entries) {
            if (entry.name.find(".tik") != std::string::npos) {
                const auto base = entry.name.substr(0, entry.name.size() - 4);
                auto it = entries.find(base + ".cert");
                if (it != entries.end() && !entry.ticket_buf.empty() && !it->second.cert_buf.empty()) {
                    const Result rc = esImportTicket(entry.ticket_buf.data(), entry.ticket_buf.size(), it->second.cert_buf.data(), it->second.cert_buf.size());
                    if (R_FAILED(rc)) {
                        LOG_DEBUG("MTP XCI finalize: ticket import failed (0x%08x)\n", rc);
                        StreamTrace("XCI ticket import fail base='%s' rc=0x%08x", base.c_str(), rc);
                        return false;
                    }
                }
            }
        }

        try {
            m_helper->CommitAll();
            StreamTrace("XCI CommitAll ok");
            return true;
        } catch (const std::exception& e) {
            LOG_DEBUG("MTP XCI finalize: CommitAll failed: %s\n", e.what());
            StreamTrace("XCI CommitAll exception='%s'", e.what());
            return false;
        } catch (...) {
            LOG_DEBUG("MTP XCI finalize: CommitAll failed with unknown exception\n");
            StreamTrace("XCI CommitAll unknown exception");
            return false;
        }
    }

    NcmStorageId m_dest_storage = NcmStorageId_SdCard;
    std::uint64_t m_total_size = 0;
    std::uint64_t m_received = 0;
    MtpStreamBuffer m_buffer;
    std::thread m_thread;
    std::atomic<bool> m_done{false};
    std::atomic<bool> m_ok{true};
    std::unique_ptr<StreamInstallHelper> m_helper;
};

} // namespace

bool StartStreamInstall(const std::string& name, std::uint64_t size, int storage_choice)
{
    g_stream_trace_enabled.store(std::filesystem::exists(kStreamTraceEnablePath), std::memory_order_relaxed);
    ResetStreamTrace();
    StreamTrace("Start name='%s' size=%llu storage_choice=%d",
        name.c_str(),
        static_cast<unsigned long long>(size),
        storage_choice);
    {
        std::lock_guard<std::mutex> lock(g_stream_mutex);
        g_stream.reset();
    }

    NcmStorageId storage = (storage_choice == 1) ? NcmStorageId_BuiltInUser : NcmStorageId_SdCard;
    {
        std::lock_guard<std::mutex> lock(g_stream_mutex);
        g_stream_name = name;
        if (IsNspName(name)) {
            g_stream = std::make_unique<MtpNspStream>(size, storage);
            StreamTrace("Start stream_type=NSP");
        } else if (IsXciName(name)) {
            g_stream = std::make_unique<MtpXciStreamPull>(size, storage);
            StreamTrace("Start stream_type=XCI");
        } else {
            g_stream_name.clear();
            g_stream_total.store(0, std::memory_order_relaxed);
            g_stream_received.store(0, std::memory_order_relaxed);
            g_stream_active.store(false, std::memory_order_relaxed);
            g_stream_complete.store(false, std::memory_order_relaxed);
            g_stream_title_id.store(0, std::memory_order_relaxed);
            StreamTrace("Start unsupported extension name='%s'", name.c_str());
            return false;
        }
    }

    g_stream_total.store(size, std::memory_order_relaxed);
    g_stream_received.store(0, std::memory_order_relaxed);
    g_stream_active.store(true, std::memory_order_relaxed);
    g_stream_complete.store(false, std::memory_order_relaxed);
    g_stream_title_id.store(0, std::memory_order_relaxed);

    inst::util::initInstallServices();
    StreamTrace("Start initInstallServices done");
    return true;
}

bool WriteStreamInstall(const void* buf, size_t size, std::uint64_t offset)
{
    const u64 call_idx = g_stream_write_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    const u64 t0 = armGetSystemTick();
    bool ok = false;
    bool has_stream = false;
    std::lock_guard<std::mutex> lock(g_stream_mutex);
    if (!g_stream) {
        const u64 dt_ms = TicksToMs(armGetSystemTick() - t0);
        StreamTrace("Write call=%llu no-stream off=%llu size=%zu dt_ms=%llu",
            static_cast<unsigned long long>(call_idx),
            static_cast<unsigned long long>(offset),
            size,
            static_cast<unsigned long long>(dt_ms));
        return false;
    }
    has_stream = true;
    ok = g_stream->Feed(buf, size, offset);
    (void)has_stream;
    const u64 dt_ms = TicksToMs(armGetSystemTick() - t0);
    if (!ok || dt_ms >= 200 || call_idx <= 16 || (call_idx % 256ULL) == 0ULL) {
        StreamTrace("Write call=%llu off=%llu size=%zu ok=%d dt_ms=%llu recv=%llu total=%llu",
            static_cast<unsigned long long>(call_idx),
            static_cast<unsigned long long>(offset),
            size,
            ok ? 1 : 0,
            static_cast<unsigned long long>(dt_ms),
            static_cast<unsigned long long>(g_stream_received.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_stream_total.load(std::memory_order_relaxed)));
    }
    return ok;
}

void CloseStreamInstall()
{
    StreamTrace("Close begin");
    std::unique_ptr<StreamInstaller> stream;
    {
        std::lock_guard<std::mutex> lock(g_stream_mutex);
        if (!g_stream) return;
        stream = std::move(g_stream);
    }

    bool ok = false;
    const u64 t0 = armGetSystemTick();
    try {
        ok = stream->Finalize();
    } catch (const std::exception& e) {
        LOG_DEBUG("MTP close: finalize threw: %s\n", e.what());
        StreamTrace("Close finalize exception='%s'", e.what());
        ok = false;
    } catch (...) {
        LOG_DEBUG("MTP close: finalize threw unknown exception\n");
        StreamTrace("Close finalize unknown exception");
        ok = false;
    }
    const u64 dt_ms = TicksToMs(armGetSystemTick() - t0);

    {
        std::lock_guard<std::mutex> lock(g_stream_mutex);
        g_stream_name.clear();
    }
    g_stream_active.store(false, std::memory_order_relaxed);
    g_stream_complete.store(ok, std::memory_order_relaxed);
    inst::util::deinitInstallServices();
    StreamTrace("Close end ok=%d dt_ms=%llu", ok ? 1 : 0, static_cast<unsigned long long>(dt_ms));
}

void CancelStreamInstall()
{
    StreamTrace("Cancel begin");
    {
        std::lock_guard<std::mutex> lock(g_stream_mutex);
        g_stream.reset();
        g_stream_name.clear();
    }

    g_stream_active.store(false, std::memory_order_relaxed);
    g_stream_complete.store(false, std::memory_order_relaxed);
    g_stream_total.store(0, std::memory_order_relaxed);
    g_stream_received.store(0, std::memory_order_relaxed);
    g_stream_title_id.store(0, std::memory_order_relaxed);
    inst::util::deinitInstallServices();
    StreamTrace("Cancel end");
}

bool IsStreamInstallActive()
{
    return g_stream_active.load(std::memory_order_relaxed);
}

bool ConsumeStreamInstallComplete()
{
    if (!g_stream_complete.load(std::memory_order_relaxed)) {
        return false;
    }
    g_stream_complete.store(false, std::memory_order_relaxed);
    return true;
}

void GetStreamInstallProgress(std::uint64_t* out_received, std::uint64_t* out_total)
{
    if (out_received) {
        *out_received = g_stream_received.load(std::memory_order_relaxed);
    }
    if (out_total) {
        *out_total = g_stream_total.load(std::memory_order_relaxed);
    }
}

std::string GetStreamInstallName()
{
    std::lock_guard<std::mutex> lock(g_stream_mutex);
    return g_stream_name;
}

bool GetStreamInstallTitleId(std::uint64_t* out_title_id)
{
    const auto value = g_stream_title_id.load(std::memory_order_relaxed);
    if (value == 0) {
        return false;
    }
    if (out_title_id) {
        *out_title_id = value;
    }
    return true;
}

} // namespace inst::mtp
