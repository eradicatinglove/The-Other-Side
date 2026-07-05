#include "mtp_server.hpp"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <vector>

#include "switch.h"
#include <haze.hpp>

#include "mtp_install.hpp"
#include "util/config.hpp"
#include "util/error.hpp"
#include "usb_comms_awoo.h"

namespace inst::mtp {
namespace {

constexpr const char* kAlbumTracePath = "sdmc:/switch/CyberFoil/mtp_album_debug.log";
std::mutex g_album_trace_mutex;
std::atomic<u64> g_album_trace_count{0};
constexpr u64 kAlbumTraceMaxLines = 20000;

void AlbumTrace(const char* fmt, ...) {
    const u64 idx = g_album_trace_count.fetch_add(1, std::memory_order_relaxed);
    if (idx >= kAlbumTraceMaxLines) {
        return;
    }

    char msg[512] = {};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(g_album_trace_mutex);
    FILE* f = std::fopen(kAlbumTracePath, "ab");
    if (!f) {
        return;
    }
    std::fprintf(f, "%llu %s\n", static_cast<unsigned long long>(idx), msg);
    std::fclose(f);
}

bool CopyFsEntryName(char* dst, size_t dst_size, const char* src) {
    if (!dst || !src || dst_size == 0) {
        return false;
    }
    const size_t len = std::strlen(src);
    if (len >= dst_size) {
        return false;
    }
    std::memcpy(dst, src, len + 1);
    return true;
}

struct InstallSharedData {
    std::mutex mutex;
    bool enabled = false;
    bool in_progress = false;
    std::string current_file;
};

InstallSharedData g_shared;
bool g_running = false;
std::mutex g_state_mutex;
int g_storage_choice = 0;
bool g_ncm_ready = false;
bool g_awoo_suspended = false;

constexpr u16 kMtpVid = 0x057e; // Nintendo
constexpr u16 kMtpPid = 0x201d; // Switch

struct FsProxyVfs : haze::FileSystemProxyImpl {
    struct File {
        u64 index{};
        u32 mode{};
    };

    struct Dir {
        u64 pos{};
    };

    FsProxyVfs(const char* name, const char* display_name)
    : m_name(name), m_display_name(display_name) {
    }

    const char* GetName() const override {
        return m_name.c_str();
    }

    const char* GetDisplayName() const override {
        return m_display_name.c_str();
    }

    Result GetTotalSpace(const char* /*path*/, s64* out) override {
        return QuerySpace(out, nullptr);
    }

    Result GetFreeSpace(const char* /*path*/, s64* out) override {
        return QuerySpace(nullptr, out);
    }

    Result GetEntryType(const char* path, FsDirEntryType* out_entry_type) override {
        if (std::strcmp(path, "/") == 0) {
            *out_entry_type = FsDirEntryType_Dir;
            return 0;
        }

        const auto file_name = GetFileName(path);
        if (!file_name) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

        const auto it = std::find_if(m_entries.begin(), m_entries.end(),
            [file_name](const auto& e){ return !strcasecmp(file_name, e.name); });
        if (it == m_entries.end()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

        *out_entry_type = FsDirEntryType_File;
        return 0;
    }

    Result CreateFile(const char* path, s64 size, u32 option) override {
        const auto file_name = GetFileName(path);
        if (!file_name) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

        const auto it = std::find_if(m_entries.begin(), m_entries.end(),
            [file_name](const auto& e){ return !strcasecmp(file_name, e.name); });
        if (it != m_entries.end()) {
            // Reuse existing virtual entry to support repeated uploads with
            // the same filename across one MTP session.
            it->file_size = size;
            return 0;
        }

        FsDirectoryEntry entry{};
        if (!CopyFsEntryName(entry.name, sizeof(entry.name), file_name)) {
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        }
        entry.type = FsDirEntryType_File;
        entry.file_size = size;
        m_entries.emplace_back(entry);
        return 0;
    }

    Result DeleteFile(const char* path) override {
        const auto file_name = GetFileName(path);
        if (!file_name) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

        const auto it = std::find_if(m_entries.begin(), m_entries.end(),
            [file_name](const auto& e){ return !strcasecmp(file_name, e.name); });
        if (it == m_entries.end()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        m_entries.erase(it);
        return 0;
    }

    Result RenameFile(const char* old_path, const char* new_path) override {
        const auto file_name = GetFileName(old_path);
        if (!file_name) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

        const auto it = std::find_if(m_entries.begin(), m_entries.end(),
            [file_name](const auto& e){ return !strcasecmp(file_name, e.name); });
        if (it == m_entries.end()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

        const auto new_name = GetFileName(new_path);
        if (!new_name) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

        const auto new_it = std::find_if(m_entries.begin(), m_entries.end(),
            [new_name](const auto& e){ return !strcasecmp(new_name, e.name); });
        if (new_it != m_entries.end()) return KERNELRESULT(AlreadyExists);

        if (!CopyFsEntryName(it->name, sizeof(it->name), new_name)) {
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        }
        return 0;
    }

    Result OpenFile(const char* path, u32 mode, FsFile* out_file) override {
        const auto file_name = GetFileName(path);
        if (!file_name) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

        const auto it = std::find_if(m_entries.begin(), m_entries.end(),
            [file_name](const auto& e){ return !strcasecmp(file_name, e.name); });
        if (it == m_entries.end()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

        std::memset(out_file, 0, sizeof(*out_file));
        auto file = std::make_unique<File>();
        file->index = static_cast<u64>(std::distance(m_entries.begin(), it));
        file->mode = mode;
        m_open_files.emplace(out_file, std::move(file));
        return 0;
    }

    Result GetFileSize(FsFile* file, s64* out_size) override {
        const auto it = m_open_files.find(file);
        if (it == m_open_files.end()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        *out_size = m_entries[it->second->index].file_size;
        return 0;
    }

    Result SetFileSize(FsFile* file, s64 size) override {
        const auto it = m_open_files.find(file);
        if (it == m_open_files.end()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        m_entries[it->second->index].file_size = size;
        return 0;
    }

    Result ReadFile(FsFile* /*file*/, s64 /*off*/, void* /*buf*/, u64 /*read_size*/, u32 /*option*/, u64* /*out_bytes_read*/) override {
        return KERNELRESULT(NotImplemented);
    }

    Result WriteFile(FsFile* file, s64 off, const void* /*buf*/, u64 write_size, u32 /*option*/) override {
        const auto it = m_open_files.find(file);
        if (it == m_open_files.end()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        const auto new_size = static_cast<s64>(off + static_cast<s64>(write_size));
        auto& entry = m_entries[it->second->index];
        if (new_size > entry.file_size) {
            entry.file_size = new_size;
        }
        return 0;
    }

    void CloseFile(FsFile* file) override {
        m_open_files.erase(file);
    }

    Result CreateDirectory(const char* /*path*/) override { return KERNELRESULT(NotImplemented); }
    Result DeleteDirectoryRecursively(const char* /*path*/) override { return KERNELRESULT(NotImplemented); }
    Result RenameDirectory(const char* /*old_path*/, const char* /*new_path*/) override { return KERNELRESULT(NotImplemented); }

    Result OpenDirectory(const char* /*path*/, u32 /*mode*/, FsDir* out_dir) override {
        std::memset(out_dir, 0, sizeof(*out_dir));
        auto dir = std::make_unique<Dir>();
        dir->pos = 0;
        m_open_dirs.emplace(out_dir, std::move(dir));
        return 0;
    }

    Result ReadDirectory(FsDir* d, s64* out_total_entries, size_t max_entries, FsDirectoryEntry* buf) override {
        const auto it = m_open_dirs.find(d);
        if (it == m_open_dirs.end()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        const auto pos = static_cast<size_t>(it->second->pos);
        if (pos >= m_entries.size()) {
            *out_total_entries = 0;
            return 0;
        }
        max_entries = std::min<size_t>(m_entries.size() - pos, max_entries);
        for (size_t i = 0; i < max_entries; i++) {
            buf[i] = m_entries[pos + i];
        }
        it->second->pos += max_entries;
        *out_total_entries = static_cast<s64>(max_entries);
        return 0;
    }

    Result GetDirectoryEntryCount(FsDir* /*d*/, s64* out_count) override {
        *out_count = m_entries.size();
        return 0;
    }

    void CloseDirectory(FsDir* d) override {
        m_open_dirs.erase(d);
    }

protected:
    Result QuerySpace(s64* out_total, s64* out_free) const {
        const auto storage_id = (g_storage_choice == 1) ? NcmStorageId_BuiltInUser : NcmStorageId_SdCard;
        NcmContentStorage storage{};
        Result rc = ncmOpenContentStorage(&storage, storage_id);
        if (R_FAILED(rc)) return rc;
        if (out_total) {
            rc = ncmContentStorageGetTotalSpaceSize(&storage, out_total);
        }
        if (R_SUCCEEDED(rc) && out_free) {
            rc = ncmContentStorageGetFreeSpaceSize(&storage, out_free);
        }
        ncmContentStorageClose(&storage);
        return rc;
    }
    const char* GetFileName(const char* path) const {
        const auto* file_name = std::strrchr(path, '/');
        if (!file_name || file_name[1] == '\0') return nullptr;
        return file_name + 1;
    }

    std::string m_name;
    std::string m_display_name;
    std::vector<FsDirectoryEntry> m_entries;
    std::unordered_map<FsFile*, std::unique_ptr<File>> m_open_files;
    std::unordered_map<FsDir*, std::unique_ptr<Dir>> m_open_dirs;
};

struct FsInstallProxy final : FsProxyVfs {
    using FsProxyVfs::FsProxyVfs;

    bool IsValidFileType(const char* name) const {
        const auto* ext = std::strrchr(name, '.');
        if (!ext) return false;
        return !strcasecmp(ext, ".nsp") || !strcasecmp(ext, ".nsz") ||
               !strcasecmp(ext, ".xci") || !strcasecmp(ext, ".xcz");
    }

    Result CreateFile(const char* path, s64 size, u32 option) override {
        std::lock_guard<std::mutex> lock(g_shared.mutex);
        if (!g_shared.enabled) return KERNELRESULT(NotImplemented);
        if (!IsValidFileType(path)) return KERNELRESULT(NotImplemented);
        return FsProxyVfs::CreateFile(path, size, option);
    }

    Result OpenFile(const char* path, u32 mode, FsFile* out_file) override {
        std::lock_guard<std::mutex> lock(g_shared.mutex);
        if (!g_shared.enabled) return KERNELRESULT(NotImplemented);
        if (!IsValidFileType(path)) return KERNELRESULT(NotImplemented);

        Result rc = FsProxyVfs::OpenFile(path, mode, out_file);
        if (R_FAILED(rc)) return rc;

        if (mode & FsOpenMode_Write) {
            const auto it = m_open_files.find(out_file);
            if (it == m_open_files.end()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
            const auto& e = m_entries[it->second->index];

            if (!g_shared.current_file.empty()) return KERNELRESULT(NotImplemented);
            g_shared.current_file = e.name;
            g_shared.in_progress = true;
            if (!StartStreamInstall(e.name, e.file_size, g_storage_choice)) {
                g_shared.current_file.clear();
                g_shared.in_progress = false;
                return KERNELRESULT(NotImplemented);
            }
        }

        return 0;
    }

    Result WriteFile(FsFile* file, s64 off, const void* buf, u64 write_size, u32 option) override {
        {
            std::lock_guard<std::mutex> lock(g_shared.mutex);
            if (!g_shared.enabled) return KERNELRESULT(NotImplemented);
        }

        if (!WriteStreamInstall(buf, write_size, off)) {
            return KERNELRESULT(NotImplemented);
        }
        return FsProxyVfs::WriteFile(file, off, buf, write_size, option);
    }

    void CloseFile(FsFile* file) override {
        bool should_finalize = false;
        {
            std::lock_guard<std::mutex> lock(g_shared.mutex);
            const auto it = m_open_files.find(file);
            if (it != m_open_files.end() && (it->second->mode & FsOpenMode_Write)) {
                should_finalize = true;
                // Clear shared state before finalize to avoid lock-order inversion
                // with stream mutex operations inside CloseStreamInstall().
                g_shared.current_file.clear();
                g_shared.in_progress = false;
            }
        }

        FsProxyVfs::CloseFile(file);

        if (should_finalize) {
            CloseStreamInstall();
        }
    }

    bool MultiThreadTransfer(s64 /*size*/, bool read) override {
        (void)read;
        // Prefer throughput for install stream; write ordering is preserved by libhaze.
        return true;
    }
};

struct FsAlbumProxy final : haze::FileSystemProxyImpl {
    FsAlbumProxy() = default;
    ~FsAlbumProxy() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_fs_open) {
            fsFsClose(&m_fs);
            m_fs_open = false;
        }
        if (m_sd_fs_open) {
            fsFsClose(&m_sd_fs);
            m_sd_fs_open = false;
        }
    }

    const char* GetName() const override {
        return "album";
    }

    const char* GetDisplayName() const override {
        return "Album (Screenshots & Videos)";
    }

    u16 GetAccessCapability() const override {
        return static_cast<u16>(haze::PtpAccessCapability_ReadOnly);
    }

    Result GetTotalSpace(const char* /*path*/, s64* out) override {
        return QuerySdSpace(out, nullptr);
    }

    Result GetFreeSpace(const char* /*path*/, s64* out) override {
        return QuerySdSpace(nullptr, out);
    }

    Result GetEntryType(const char* path, FsDirEntryType* out_entry_type) override {
        if (!out_entry_type) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        std::lock_guard<std::mutex> lock(m_mutex);
        const Result rc = EnsureFsOpenLocked();
        if (R_FAILED(rc)) return rc;
        const auto fixed = FixPath(path);
        if (fixed.empty()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        Result out_rc = 0;
        for (int attempt = 0; attempt < 10; ++attempt) {
            out_rc = fsFsGetEntryType(&m_fs, fixed.c_str(), out_entry_type);
            if (R_SUCCEEDED(out_rc)) {
                break;
            }
            // The album backend can return occasional transient lookup failures under
            // host-side parallel metadata probing; retry a couple of times.
            svcSleepThread(10'000'000);
        }
        if (R_FAILED(out_rc)) {
            FsDir dir{};
            const Result dir_rc = fsFsOpenDirectory(&m_fs, fixed.c_str(), FsDirOpenMode_ReadDirs, &dir);
            if (R_SUCCEEDED(dir_rc)) {
                fsDirClose(&dir);
                *out_entry_type = FsDirEntryType_Dir;
                AlbumTrace("GetEntryType fallback-dir path='%s' fixed='%s' rc=0x%08x",
                    path ? path : "(null)", fixed.c_str(), out_rc);
                return 0;
            }

            FsFile file{};
            const Result file_rc = fsFsOpenFile(&m_fs, fixed.c_str(), FsOpenMode_Read, &file);
            if (R_SUCCEEDED(file_rc)) {
                fsFileClose(&file);
                *out_entry_type = FsDirEntryType_File;
                AlbumTrace("GetEntryType fallback-file path='%s' fixed='%s' rc=0x%08x",
                    path ? path : "(null)", fixed.c_str(), out_rc);
                return 0;
            }

            const auto slash = fixed.find_last_of('/');
            const auto dot = fixed.find_last_of('.');
            const bool looks_like_file = (dot != std::string::npos) &&
                                         (slash == std::string::npos || dot > slash + 1);
            *out_entry_type = looks_like_file ? FsDirEntryType_File : FsDirEntryType_Dir;
            AlbumTrace("GetEntryType fallback-heuristic path='%s' fixed='%s' rc=0x%08x inferred=%d",
                path ? path : "(null)", fixed.c_str(), out_rc, static_cast<int>(*out_entry_type));
            return 0;
        }
        AlbumTrace("GetEntryType path='%s' fixed='%s' rc=0x%08x type=%d",
            path ? path : "(null)", fixed.c_str(), out_rc, R_SUCCEEDED(out_rc) ? static_cast<int>(*out_entry_type) : -1);
        return out_rc;
    }

    Result CreateFile(const char* /*path*/, s64 /*size*/, u32 /*option*/) override { return static_cast<Result>(haze::ResultOperationNotSupported::Value); }
    Result DeleteFile(const char* /*path*/) override { return static_cast<Result>(haze::ResultOperationNotSupported::Value); }
    Result RenameFile(const char* /*old_path*/, const char* /*new_path*/) override { return static_cast<Result>(haze::ResultOperationNotSupported::Value); }
    Result SetFileSize(FsFile* /*file*/, s64 /*size*/) override { return static_cast<Result>(haze::ResultOperationNotSupported::Value); }
    Result WriteFile(FsFile* /*file*/, s64 /*off*/, const void* /*buf*/, u64 /*write_size*/, u32 /*option*/) override { return static_cast<Result>(haze::ResultOperationNotSupported::Value); }
    Result CreateDirectory(const char* /*path*/) override { return static_cast<Result>(haze::ResultOperationNotSupported::Value); }
    Result DeleteDirectoryRecursively(const char* /*path*/) override { return static_cast<Result>(haze::ResultOperationNotSupported::Value); }
    Result RenameDirectory(const char* /*old_path*/, const char* /*new_path*/) override { return static_cast<Result>(haze::ResultOperationNotSupported::Value); }

    Result OpenFile(const char* path, u32 mode, FsFile* out_file) override {
        if (!out_file) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        if ((mode & FsOpenMode_Write) || (mode & FsOpenMode_Append)) return static_cast<Result>(haze::ResultOperationNotSupported::Value);
        std::memset(out_file, 0, sizeof(*out_file));
        std::lock_guard<std::mutex> lock(m_mutex);
        const Result rc = EnsureFsOpenLocked();
        if (R_FAILED(rc)) return rc;
        const auto fixed = FixPath(path);
        if (fixed.empty()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        Result out_rc = 0;
        for (int attempt = 0; attempt < 8; ++attempt) {
            out_rc = fsFsOpenFile(&m_fs, fixed.c_str(), FsOpenMode_Read, out_file);
            if (R_SUCCEEDED(out_rc)) {
                break;
            }

            // Try to recover from transient album backend faults by reopening once.
            if (out_rc == 0x0000d401 && attempt == 3) {
                fsFsClose(&m_fs);
                m_fs_open = false;
                const Result reopen_rc = EnsureFsOpenLocked();
                AlbumTrace("OpenFile reopen attempt rc=0x%08x", reopen_rc);
                if (R_FAILED(reopen_rc)) {
                    break;
                }
            }
            svcSleepThread(10'000'000);
        }
        if (R_FAILED(out_rc)) {
            Result sd_rc = 0;
            const std::string sd_path = ToSdAlbumPath(fixed);
            if (!sd_path.empty()) {
                sd_rc = OpenFileFromSdLocked(sd_path.c_str(), out_file);
                if (R_SUCCEEDED(sd_rc)) {
                    AlbumTrace("OpenFile fallback-sd path='%s' fixed='%s' sd='%s' rc=0x%08x",
                        path ? path : "(null)", fixed.c_str(), sd_path.c_str(), sd_rc);
                    return sd_rc;
                }
            } else {
                sd_rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
            }
            AlbumTrace("OpenFile fallback-sd-failed path='%s' fixed='%s' sd='%s' rc=0x%08x",
                path ? path : "(null)", fixed.c_str(), sd_path.empty() ? "(invalid)" : sd_path.c_str(), sd_rc);
        }
        AlbumTrace("OpenFile path='%s' fixed='%s' mode=0x%x rc=0x%08x",
            path ? path : "(null)", fixed.c_str(), mode, out_rc);
        return out_rc;
    }

    Result GetFileSize(FsFile* file, s64* out_size) override {
        if (!file || !out_size) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        Result rc = 0;
        for (int attempt = 0; attempt < 8; ++attempt) {
            rc = fsFileGetSize(file, out_size);
            if (R_SUCCEEDED(rc)) {
                break;
            }
            svcSleepThread(5'000'000);
        }
        AlbumTrace("GetFileSize rc=0x%08x size=%lld",
            rc, static_cast<long long>(R_SUCCEEDED(rc) ? *out_size : -1));
        return rc;
    }

    Result ReadFile(FsFile* file, s64 off, void* buf, u64 read_size, u32 /*option*/, u64* out_bytes_read) override {
        if (!file || !buf || !out_bytes_read) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        const Result rc = fsFileRead(file, off, buf, read_size, FsReadOption_None, out_bytes_read);
        AlbumTrace("ReadFile off=%lld req=%llu rc=0x%08x got=%llu",
            static_cast<long long>(off),
            static_cast<unsigned long long>(read_size),
            rc,
            static_cast<unsigned long long>(R_SUCCEEDED(rc) ? *out_bytes_read : 0));
        return rc;
    }

    void CloseFile(FsFile* file) override {
        if (file) {
            fsFileClose(file);
            AlbumTrace("CloseFile");
        }
    }

    Result OpenDirectory(const char* path, u32 /*mode*/, FsDir* out_dir) override {
        if (!out_dir) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        std::lock_guard<std::mutex> lock(m_mutex);
        const Result rc = EnsureFsOpenLocked();
        if (R_FAILED(rc)) return rc;
        const auto fixed = FixPath(path);
        if (fixed.empty()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        const Result out_rc = fsFsOpenDirectory(&m_fs, fixed.c_str(),
            FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, out_dir);
        AlbumTrace("OpenDirectory path='%s' fixed='%s' rc=0x%08x",
            path ? path : "(null)", fixed.c_str(), out_rc);
        return out_rc;
    }

    Result ReadDirectory(FsDir* d, s64* out_total_entries, size_t max_entries, FsDirectoryEntry* buf) override {
        if (!d || !out_total_entries || !buf) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        const Result rc = fsDirRead(d, out_total_entries, max_entries, buf);
        AlbumTrace("ReadDirectory max=%llu rc=0x%08x read=%lld",
            static_cast<unsigned long long>(max_entries), rc,
            static_cast<long long>(R_SUCCEEDED(rc) ? *out_total_entries : -1));
        return rc;
    }

    Result GetDirectoryEntryCount(FsDir* d, s64* out_count) override {
        if (!d || !out_count) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        const Result rc = fsDirGetEntryCount(d, out_count);
        AlbumTrace("GetDirectoryEntryCount rc=0x%08x count=%lld",
            rc, static_cast<long long>(R_SUCCEEDED(rc) ? *out_count : -1));
        return rc;
    }

    void CloseDirectory(FsDir* d) override {
        if (d) {
            fsDirClose(d);
            AlbumTrace("CloseDirectory");
        }
    }

    bool MultiThreadTransfer(s64 /*size*/, bool read) override {
        /* Album reads are more reliable on this backend with conservative transfer mode. */
        const bool allow = !read;
        AlbumTrace("MultiThreadTransfer read=%d allow=%d", read ? 1 : 0, allow ? 1 : 0);
        return allow;
    }

private:
    Result EnsureFsOpenLocked() {
        if (!m_fs_open) {
            const Result rc = fsOpenImageDirectoryFileSystem(&m_fs, FsImageDirectoryId_Sd);
            if (R_FAILED(rc)) return rc;
            m_fs_open = true;
        }
        return 0;
    }

    Result EnsureSdFsOpenLocked() {
        if (!m_sd_fs_open) {
            const Result rc = fsOpenSdCardFileSystem(&m_sd_fs);
            if (R_FAILED(rc)) return rc;
            m_sd_fs_open = true;
        }
        return 0;
    }

    Result OpenFileFromSdLocked(const char* sd_path, FsFile* out_file) {
        if (!sd_path || !out_file) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        Result rc = EnsureSdFsOpenLocked();
        if (R_FAILED(rc)) {
            return rc;
        }
        for (int attempt = 0; attempt < 4; ++attempt) {
            rc = fsFsOpenFile(&m_sd_fs, sd_path, FsOpenMode_Read, out_file);
            if (R_SUCCEEDED(rc)) {
                return rc;
            }
            svcSleepThread(5'000'000);
        }
        return rc;
    }

    static std::string ToSdAlbumPath(const std::string& fixed) {
        if (fixed.empty() || fixed.find("..") != std::string::npos) {
            return {};
        }
        if (fixed == "/") {
            return "/Nintendo/Album";
        }
        return "/Nintendo/Album" + fixed;
    }

    static Result QuerySdSpace(s64* out_total, s64* out_free) {
        NcmContentStorage storage{};
        Result rc = ncmOpenContentStorage(&storage, NcmStorageId_SdCard);
        if (R_FAILED(rc)) return rc;
        if (out_total) rc = ncmContentStorageGetTotalSpaceSize(&storage, out_total);
        if (R_SUCCEEDED(rc) && out_free) rc = ncmContentStorageGetFreeSpaceSize(&storage, out_free);
        ncmContentStorageClose(&storage);
        return rc;
    }

    static std::string FixPath(const char* path) {
        if (!path || path[0] == '\0') return "/";
        std::string p(path);
        while (!p.empty() && p.front() == '/') p.erase(p.begin());
        if (p.empty()) return "/";
        if (p == "album") return "/";
        if (p.rfind("album/", 0) == 0) p.erase(0, 6);
        if (p.empty()) return "/";
        if (p.find("..") != std::string::npos) return {};
        return "/" + p;
    }

    FsFileSystem m_fs{};
    bool m_fs_open = false;
    FsFileSystem m_sd_fs{};
    bool m_sd_fs_open = false;
    std::mutex m_mutex;
};

// Full read/write access to the SD card root, rooted at sdmc:/ directly.
// Mirrors FsAlbumProxy's pattern of forwarding straight to the real fs*
// calls, but without the album-specific read-only restriction or the
// /Nintendo/Album fallback path rewriting.
struct FsSdProxy final : haze::FileSystemProxyImpl {
    FsSdProxy() = default;
    ~FsSdProxy() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_fs_open) {
            fsFsClose(&m_fs);
            m_fs_open = false;
        }
    }

    const char* GetName() const override {
        return "sdcard";
    }

    const char* GetDisplayName() const override {
        return "SD Card";
    }

    u16 GetAccessCapability() const override {
        return static_cast<u16>(haze::PtpAccessCapability_ReadWrite);
    }

    Result GetTotalSpace(const char* /*path*/, s64* out) override {
        return QuerySdSpace(out, nullptr);
    }

    Result GetFreeSpace(const char* /*path*/, s64* out) override {
        return QuerySdSpace(nullptr, out);
    }

    Result GetEntryType(const char* path, FsDirEntryType* out_entry_type) override {
        if (!out_entry_type) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        std::lock_guard<std::mutex> lock(m_mutex);
        const Result rc = EnsureFsOpenLocked();
        if (R_FAILED(rc)) return rc;
        const auto fixed = FixPath(path);
        if (fixed.empty()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return fsFsGetEntryType(&m_fs, fixed.c_str(), out_entry_type);
    }

    Result CreateFile(const char* path, s64 size, u32 option) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Result rc = EnsureFsOpenLocked();
        if (R_FAILED(rc)) return rc;
        const auto fixed = FixPath(path);
        if (fixed.empty()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return fsFsCreateFile(&m_fs, fixed.c_str(), size, option);
    }

    Result DeleteFile(const char* path) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Result rc = EnsureFsOpenLocked();
        if (R_FAILED(rc)) return rc;
        const auto fixed = FixPath(path);
        if (fixed.empty()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return fsFsDeleteFile(&m_fs, fixed.c_str());
    }

    Result RenameFile(const char* old_path, const char* new_path) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Result rc = EnsureFsOpenLocked();
        if (R_FAILED(rc)) return rc;
        const auto fixed_old = FixPath(old_path);
        const auto fixed_new = FixPath(new_path);
        if (fixed_old.empty() || fixed_new.empty()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return fsFsRenameFile(&m_fs, fixed_old.c_str(), fixed_new.c_str());
    }

    Result CreateDirectory(const char* path) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Result rc = EnsureFsOpenLocked();
        if (R_FAILED(rc)) return rc;
        const auto fixed = FixPath(path);
        if (fixed.empty()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return fsFsCreateDirectory(&m_fs, fixed.c_str());
    }

    Result DeleteDirectoryRecursively(const char* path) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Result rc = EnsureFsOpenLocked();
        if (R_FAILED(rc)) return rc;
        const auto fixed = FixPath(path);
        if (fixed.empty()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return fsFsDeleteDirectoryRecursively(&m_fs, fixed.c_str());
    }

    Result RenameDirectory(const char* old_path, const char* new_path) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Result rc = EnsureFsOpenLocked();
        if (R_FAILED(rc)) return rc;
        const auto fixed_old = FixPath(old_path);
        const auto fixed_new = FixPath(new_path);
        if (fixed_old.empty() || fixed_new.empty()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return fsFsRenameDirectory(&m_fs, fixed_old.c_str(), fixed_new.c_str());
    }

    Result OpenFile(const char* path, u32 mode, FsFile* out_file) override {
        if (!out_file) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        std::memset(out_file, 0, sizeof(*out_file));
        std::lock_guard<std::mutex> lock(m_mutex);
        const Result rc = EnsureFsOpenLocked();
        if (R_FAILED(rc)) return rc;
        const auto fixed = FixPath(path);
        if (fixed.empty()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return fsFsOpenFile(&m_fs, fixed.c_str(), mode, out_file);
    }

    Result GetFileSize(FsFile* file, s64* out_size) override {
        if (!file || !out_size) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return fsFileGetSize(file, out_size);
    }

    Result SetFileSize(FsFile* file, s64 size) override {
        if (!file) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return fsFileSetSize(file, size);
    }

    Result ReadFile(FsFile* file, s64 off, void* buf, u64 read_size, u32 option, u64* out_bytes_read) override {
        if (!file || !buf || !out_bytes_read) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return fsFileRead(file, off, buf, read_size, option, out_bytes_read);
    }

    Result WriteFile(FsFile* file, s64 off, const void* buf, u64 write_size, u32 option) override {
        if (!file || !buf) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return fsFileWrite(file, off, buf, write_size, option);
    }

    void CloseFile(FsFile* file) override {
        if (file) fsFileClose(file);
    }

    Result OpenDirectory(const char* path, u32 mode, FsDir* out_dir) override {
        if (!out_dir) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        std::lock_guard<std::mutex> lock(m_mutex);
        const Result rc = EnsureFsOpenLocked();
        if (R_FAILED(rc)) return rc;
        const auto fixed = FixPath(path);
        if (fixed.empty()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return fsFsOpenDirectory(&m_fs, fixed.c_str(),
            FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, out_dir);
    }

    Result ReadDirectory(FsDir* d, s64* out_total_entries, size_t max_entries, FsDirectoryEntry* buf) override {
        if (!d || !out_total_entries || !buf) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return fsDirRead(d, out_total_entries, max_entries, buf);
    }

    Result GetDirectoryEntryCount(FsDir* d, s64* out_count) override {
        if (!d || !out_count) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return fsDirGetEntryCount(d, out_count);
    }

    void CloseDirectory(FsDir* d) override {
        if (d) fsDirClose(d);
    }

private:
    Result EnsureFsOpenLocked() {
        if (!m_fs_open) {
            const Result rc = fsOpenSdCardFileSystem(&m_fs);
            if (R_FAILED(rc)) return rc;
            m_fs_open = true;
        }
        return 0;
    }

    static Result QuerySdSpace(s64* out_total, s64* out_free) {
        NcmContentStorage storage{};
        Result rc = ncmOpenContentStorage(&storage, NcmStorageId_SdCard);
        if (R_FAILED(rc)) return rc;
        if (out_total) rc = ncmContentStorageGetTotalSpaceSize(&storage, out_total);
        if (R_SUCCEEDED(rc) && out_free) rc = ncmContentStorageGetFreeSpaceSize(&storage, out_free);
        ncmContentStorageClose(&storage);
        return rc;
    }

    // Reject ".." traversal but otherwise pass the path through as-is,
    // since this proxy is rooted at sdmc:/ itself (no subfolder prefix
    // to strip, unlike the album proxy).
    static std::string FixPath(const char* path) {
        if (!path || path[0] == '\0') return "/";
        std::string p(path);
        while (!p.empty() && p.front() == '/') p.erase(p.begin());
        if (p.empty()) return "/";
        if (p == "sdcard") return "/";
        if (p.rfind("sdcard/", 0) == 0) p.erase(0, 7);
        if (p.empty()) return "/";
        if (p.find("..") != std::string::npos) return {};
        return "/" + p;
    }

    FsFileSystem m_fs{};
    bool m_fs_open = false;
    std::mutex m_mutex;
};

haze::FsEntries g_entries;

} // namespace

bool StartInstallServer(int storage_choice)
{
    if (IsInstallServerRunning()) {
        StopInstallServer();
    }

    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_running) return true;

    g_storage_choice = storage_choice;
    std::remove(kAlbumTracePath);
    g_album_trace_count.store(0, std::memory_order_relaxed);
    if (!g_awoo_suspended) {
        awoo_usbCommsExit();
        g_awoo_suspended = true;
    }
    if (!g_ncm_ready) {
        const Result rc = ncmInitialize();
        if (R_SUCCEEDED(rc)) {
            g_ncm_ready = true;
        } else {
            if (g_awoo_suspended) {
                const Result resume_rc = awoo_usbCommsInitialize();
                if (R_SUCCEEDED(resume_rc) || resume_rc == MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized)) {
                    g_awoo_suspended = false;
                }
            }
            return false;
        }
    }
    g_entries.clear();
    g_entries.emplace_back(std::make_shared<FsInstallProxy>("install", "Install (NSP, XCI, NSZ, XCZ)"));
    g_entries.emplace_back(std::make_shared<FsSdProxy>());
    if (inst::config::mtpExposeAlbum) {
        g_entries.emplace_back(std::make_shared<FsAlbumProxy>());
    }

    if (!haze::Initialize(nullptr, 0x2C, 2, g_entries, kMtpVid, kMtpPid)) {
        if (g_awoo_suspended) {
            const Result rc = awoo_usbCommsInitialize();
            if (R_SUCCEEDED(rc) || rc == MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized)) {
                g_awoo_suspended = false;
            }
        }
        return false;
    }


    {
        std::lock_guard<std::mutex> shared_lock(g_shared.mutex);
        g_shared.current_file.clear();
        g_shared.in_progress = false;
        g_shared.enabled = true;
    }
    g_running = true;
    return true;
}

void StopInstallServer()
{
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (!g_running) return;
    inst::mtp::CancelStreamInstall();
    haze::Exit();
    g_entries.clear();
    {
        std::lock_guard<std::mutex> shared_lock(g_shared.mutex);
        g_shared.enabled = false;
        g_shared.in_progress = false;
        g_shared.current_file.clear();
    }
    if (g_ncm_ready) {
        ncmExit();
        g_ncm_ready = false;
    }
    if (g_awoo_suspended) {
        const Result rc = awoo_usbCommsInitialize();
        if (R_SUCCEEDED(rc) || rc == MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized)) {
            g_awoo_suspended = false;
        }
    }
    g_running = false;
}

bool IsInstallServerRunning()
{
    std::lock_guard<std::mutex> lock(g_state_mutex);
    return g_running;
}

} // namespace inst::mtp
