#include <curl/curl.h>
#include <zstd.h>
#include <webp/decode.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <borealis/extern/stb_image/stb_image_write.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <switch.h>
#include <dirent.h>
#include <unordered_set>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <threads.h>

#include <borealis.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <nlohmann/json.hpp>


#include "install/install_nsp.hpp"
#include "install/install_xci.hpp"
#include "install/sdmc_nsp.hpp"
#include "util/offline_title_db.hpp"
#include "util/network_util.hpp"
#include "util/util.hpp"
#include "install/sdmc_xci.hpp"
#include "install/http_nsp.hpp"
#include "install/http_xci.hpp"
#include "nx/ipc/es.h"
#include "nx/ipc/ns_ext.h"
#include "mtp_server.hpp"
#include "pinned_status_view.hpp"
#include "ui/instPage.hpp"
#include "ui/MainApplication.hpp"
#include "util/config.hpp"
#include "util/hauth.hpp"
#include "util/uid.hpp"

namespace i18n = brls::i18n;
using namespace i18n::literals;


#define LOCATIONS_PATH  "sdmc:/switch/TheOtherSide/locations.conf"
#define ICON_CACHE_DIR  "sdmc:/switch/TheOtherSide/icon_cache"
#define LOCATIONS_DIR   "sdmc:/switch/TheOtherSide"
#define TINCLONE_UA    "Tinfoil/20.00 (Nintendo Switch; en-US)"
#define TINFOIL_CDN    "https://img.tinfoil.io/"


// icon cache helpers
static std::string GetIconShardDir(const std::string& tidUpper) {
    uint32_t hash = 2166136261u;
    for (unsigned char c : tidUpper) {
        hash ^= c;
        hash *= 16777619u;
    }
    char shard[3];
    snprintf(shard, sizeof(shard), "%02X", (unsigned)(hash % 256));
    std::string dir = std::string(ICON_CACHE_DIR) + "/" + shard;
    mkdir(dir.c_str(), 0777);
    return dir;
}


// shop list data
std::vector<std::string> g_titleNames;
std::vector<std::string> g_titleIds;
std::vector<std::string> g_titleUrls;


std::vector<std::string> g_titleIconUrls;


std::mutex g_titleDataMutex;
std::string g_fetchStatus = "Loading...";
bool g_fetchDone   = false;
bool g_fetchCancel = false;
std::atomic<bool> g_installInProgress{false};


std::atomic<int> g_activeBgThreads{0};
struct BgThreadGuard {
    BgThreadGuard()  { g_activeBgThreads.fetch_add(1, std::memory_order_relaxed); }
    ~BgThreadGuard() { g_activeBgThreads.fetch_sub(1, std::memory_order_relaxed); }
};


std::atomic<bool> g_deferredQuitRequested{false};


static std::unordered_map<std::string, std::string> g_iconCacheSet;
static bool g_iconCacheSetLoaded = false;


static std::mutex g_iconCacheSetMutex;
static void loadIconCacheSet() {
    if (g_iconCacheSetLoaded) return;
    g_iconCacheSetLoaded = true;

    auto scanDir = [](const std::string& path) {
        DIR* d = opendir(path.c_str());
        if (!d) return;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            std::string name(e->d_name);
            if (name.size() > 4 && (name.substr(name.size()-4) == ".jpg" || name.substr(name.size()-4) == ".png")) {
                std::string ext = name.substr(name.size()-4);
                std::string tid = name.substr(0, name.size()-4);
                for (auto& c : tid) c = toupper(c);
                g_iconCacheSet.emplace(tid, ext);
            } else if (name.size() > 5 && name.substr(name.size()-5) == ".webp") {
                std::string tid = name.substr(0, name.size()-5);
                for (auto& c : tid) c = toupper(c);
                g_iconCacheSet.emplace(tid, ".webp");
            }
        }
        closedir(d);
    };


    DIR* top = opendir(ICON_CACHE_DIR);
    if (!top) return;
    std::vector<std::string> subdirs;
    struct dirent* e;
    while ((e = readdir(top)) != nullptr) {
        std::string name(e->d_name);
        if (name == "." || name == "..") continue;
        bool isDir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN) {


            struct stat st;
            isDir = stat((std::string(ICON_CACHE_DIR) + "/" + name).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        }
        if (isDir)
            subdirs.push_back(std::string(ICON_CACHE_DIR) + "/" + name);
    }
    closedir(top);

    scanDir(ICON_CACHE_DIR);
    for (auto& sub : subdirs) scanDir(sub);
}


static int migrateIconCacheToShards(const std::function<void(int,int)>& progressCb) {
    DIR* d = opendir(ICON_CACHE_DIR);
    if (!d) return 0;
    std::vector<std::string> filesToMove;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string name(e->d_name);
        if (name == "." || name == "..") continue;
        if (e->d_type == DT_DIR) continue;
        if (e->d_type == DT_UNKNOWN) {
            struct stat st;
            if (stat((std::string(ICON_CACHE_DIR) + "/" + name).c_str(), &st) == 0 && S_ISDIR(st.st_mode))
                continue;
        }
        if ((name.size() > 4 && (name.substr(name.size()-4) == ".jpg" || name.substr(name.size()-4) == ".png")) ||
            (name.size() > 5 && name.substr(name.size()-5) == ".webp")) {
            filesToMove.push_back(name);
        }
    }
    closedir(d);

    int moved = 0;
    int total = (int)filesToMove.size();
    for (int i = 0; i < total; i++) {
        const std::string& name = filesToMove[i];
        std::string tid, ext;
        if (name.size() > 5 && name.substr(name.size()-5) == ".webp") {
            tid = name.substr(0, name.size()-5);
            ext = ".webp";
        } else {
            tid = name.substr(0, name.size()-4);
            ext = name.substr(name.size()-4);
        }
        for (auto& c : tid) c = toupper(c);
        std::string oldPath = std::string(ICON_CACHE_DIR) + "/" + name;
        std::string newPath = GetIconShardDir(tid) + "/" + tid + ext;
        if (rename(oldPath.c_str(), newPath.c_str()) == 0) moved++;
        if (progressCb && (i % 200 == 0 || i == total - 1))
            progressCb(i + 1, total);
    }
    return moved;
}


std::string g_pendingInstallPath;
std::string g_pendingInstallName;


std::string g_shopUser;
std::string g_shopPass;


size_t write_to_string(void* ptr, size_t size, size_t nmemb, std::string* stream) {
    stream->append(static_cast<const char*>(ptr), size * nmemb);
    return size * nmemb;
}
size_t write_to_file(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    return fwrite(ptr, size, nmemb, stream);
}

// network stuff
std::string httpGet(const std::string& url,
                    const std::string& user = "",
                    const std::string& pass = "") {
    CURL* curl = curl_easy_init();
    if (!curl) return "";
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, TINCLONE_UA);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &response);


    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
        +[](void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
            return g_fetchCancel ? 1 : 0;
        });
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, nullptr);


    struct curl_slist* headers = nullptr;
    std::string hauthVal = "HAUTH: " + inst::util::ComputeHauthFromUrl(url);
    std::string uauthVal = "UAUTH: " + inst::util::ComputeUauthFromUrl(url, user, pass);
    std::string uidVal   = "UID: "   + inst::util::ComputeUidFromMmcCid();
    headers = curl_slist_append(headers, "Theme: 0000000000000000000000000000000000000000000000000000000000000000");
    headers = curl_slist_append(headers, "Version: 20.00");
    headers = curl_slist_append(headers, "Revision: 0");
    headers = curl_slist_append(headers, "Language: en-US");
    headers = curl_slist_append(headers, hauthVal.c_str());
    headers = curl_slist_append(headers, uauthVal.c_str());
    headers = curl_slist_append(headers, uidVal.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    if (!user.empty()) {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        std::string auth = user + ":" + pass;
        curl_easy_setopt(curl, CURLOPT_USERPWD, auth.c_str());
    }
    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    if (headers) curl_slist_free_all(headers);

    mkdir("sdmc:/switch/TheOtherSide", 0777);
    FILE* dbg = fopen("sdmc:/switch/TheOtherSide/debug.txt", "a");
    if (dbg) {
        fprintf(dbg, "URL: %s\n", url.c_str());
        fprintf(dbg, "CURLcode: %d (%s)\n", res, curl_easy_strerror(res));
        fprintf(dbg, "HTTP code: %ld\n", httpCode);
        fprintf(dbg, "Response size: %zu\n", response.size());
        if (!response.empty())
            fprintf(dbg, "Response (first 500):\n%.500s\n", response.c_str());
        fprintf(dbg, "---\n");
        fclose(dbg);
    }
    curl_easy_cleanup(curl);
    return response;
}


std::string decodeTinfoilResponse(const std::string& raw) {
    static const char kMagic[] = "TINFOIL";
    static const size_t kMagicLen = 7;
    static const size_t kZstdPayloadOffset = 272;

    if (raw.size() < kMagicLen || raw.compare(0, kMagicLen, kMagic) != 0)
        return raw;

    if (raw.size() <= kZstdPayloadOffset)
        return raw;

    const char* payload = raw.data() + kZstdPayloadOffset;
    size_t payloadLen = raw.size() - kZstdPayloadOffset;

    auto logErr = [](const char* msg) {
        FILE* dbg = fopen("sdmc:/switch/TheOtherSide/debug.txt", "a");
        if (dbg) { fprintf(dbg, "%s\n", msg); fclose(dbg); }
    };


    ZSTD_DStream* dstream = ZSTD_createDStream();
    if (!dstream) { logErr("zstd: failed to create DStream"); return ""; }
    ZSTD_initDStream(dstream);

    ZSTD_inBuffer in = { payload, payloadLen, 0 };
    std::string out;
    out.resize(std::max<size_t>(payloadLen * 4, 1 << 20));
    size_t outFilled = 0;

    size_t ret = 0;
    bool frameComplete = false;
    do {
        if (outFilled == out.size())
            out.resize(out.size() * 2);

        ZSTD_outBuffer outBuf = { &out[0] + outFilled, out.size() - outFilled, 0 };
        ret = ZSTD_decompressStream(dstream, &outBuf, &in);
        outFilled += outBuf.pos;

        if (ZSTD_isError(ret)) {
            char msg[160];
            snprintf(msg, sizeof(msg), "zstd stream decompress failed: %s", ZSTD_getErrorName(ret));
            logErr(msg);
            ZSTD_freeDStream(dstream);
            return "";
        }
        if (ret == 0) { frameComplete = true; break; }
    } while (in.pos < in.size);

    ZSTD_freeDStream(dstream);

    if (!frameComplete) {
        logErr("zstd: input exhausted before frame completed (truncated/incomplete stream)");
        return "";
    }

    out.resize(outFilled);

    if (out.empty()) {
        logErr("zstd: decompressed output was empty");
        return "";
    }

    char msg[96];
    snprintf(msg, sizeof(msg), "zstd: decoded %zu bytes from %zu byte payload", out.size(), payloadLen);
    logErr(msg);
    return out;
}


#define ICON_INDEX_PATH  "sdmc:/switch/TheOtherSide/icon_index.txt"
#define ICON_CACHE_DIR   "sdmc:/switch/TheOtherSide/icon_cache"
#define TITLEDB_URL      "https://github.com/blawar/titledb/raw/refs/heads/master/US.en.json"


static std::unordered_map<std::string, std::string> g_iconIndex;
static bool g_iconIndexLoaded = false;
static std::mutex g_iconIndexMutex;


static std::atomic<bool> g_iconIndexBuildInProgress{false};


static std::atomic<int> g_activeIconFetches{0};
constexpr int kMaxConcurrentIconFetches = 6;


static std::atomic<uint64_t> g_hasIconTicksAccum{0};


static std::atomic<uint64_t> g_statTicksAccum{0};
static std::atomic<uint64_t> g_readTicksAccum{0};


static std::atomic<bool> g_iconCacheMigrationInProgress{false};

static void loadIconIndexIfNeeded() {
    std::lock_guard<std::mutex> lock(g_iconIndexMutex);
    if (g_iconIndexLoaded) return;
    g_iconIndexLoaded = true;
    FILE* f = fopen(ICON_INDEX_PATH, "rb");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back()=='\n'||s.back()=='\r')) s.pop_back();
        size_t sep = s.find('|');
        if (sep == std::string::npos) continue;
        g_iconIndex[s.substr(0, sep)] = s.substr(sep + 1);
    }
    fclose(f);
    FILE* dbg = fopen("sdmc:/switch/TheOtherSide/debug.txt", "a");
    if (dbg) { fprintf(dbg, "icon index loaded: %zu entries\n", g_iconIndex.size()); fclose(dbg); }
}


bool buildIconIndex() {
    FILE* dbg = fopen("sdmc:/switch/TheOtherSide/debug.txt", "a");
    if (dbg) { fprintf(dbg, "icon index: starting download\n"); fclose(dbg); }


    std::string tmpPath = "sdmc:/switch/TheOtherSide/titledb_tmp.json";
    FILE* tmpFile = fopen(tmpPath.c_str(), "wb");
    if (!tmpFile) return false;

    CURL* curl = curl_easy_init();
    if (!curl) { fclose(tmpFile); return false; }
    curl_easy_setopt(curl, CURLOPT_URL, TITLEDB_URL);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, TINCLONE_UA);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, tmpFile);


    static uint64_t s_idx_last = 0; s_idx_last = 0;
    auto idxXferFunc = +[](void*, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) -> int {
        if (dltotal <= 0 || dlnow <= 0) return 0;
        uint64_t now = (uint64_t)dlnow;
        if (now - s_idx_last >= 1024*1024) {
            s_idx_last = now;
            double pct = 100.0*dlnow/dltotal;
            char msg[48]; snprintf(msg, sizeof(msg), "Building icon index: %.0f%%", pct);
            std::lock_guard<std::mutex> lock(g_pendingIconsMutex);
            g_pendingNotifications.push_back(msg);
        }
        return 0;
    };
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, idxXferFunc);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, nullptr);
    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);
    fclose(tmpFile);

    if (res != CURLE_OK || httpCode != 200) {
        remove(tmpPath.c_str());
        FILE* dbg2 = fopen("sdmc:/switch/TheOtherSide/debug.txt", "a");
        if (dbg2) { fprintf(dbg2, "icon index: download failed rc=%d http=%ld\n", res, httpCode); fclose(dbg2); }
        return false;
    }


    FILE* outIdx = fopen(ICON_INDEX_PATH, "wb");
    if (!outIdx) { remove(tmpPath.c_str()); return false; }


    FILE* in = fopen(tmpPath.c_str(), "r");
    if (!in) { fclose(outIdx); remove(tmpPath.c_str()); return false; }

    size_t written = 0;
    char line[2048];
    std::string curId, curIcon;
    while (fgets(line, sizeof(line), in)) {
        std::string s(line);

        size_t idPos = s.find("\"id\":");
        if (idPos != std::string::npos) {
            size_t q1 = s.find('"', idPos + 5);
            size_t q2 = s.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos)
                curId = s.substr(q1 + 1, q2 - q1 - 1);
        }

        size_t iconPos = s.find("\"iconUrl\":");
        if (iconPos != std::string::npos) {
            size_t q1 = s.find('"', iconPos + 10);
            size_t q2 = s.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos)
                curIcon = s.substr(q1 + 1, q2 - q1 - 1);
        }

        if (!curId.empty() && !curIcon.empty()) {

            for (auto& c : curId) c = toupper(c);
            fprintf(outIdx, "%s|%s\n", curId.c_str(), curIcon.c_str());
            written++;
            curId.clear();
            curIcon.clear();
        }
    }
    fclose(in);
    fclose(outIdx);
    remove(tmpPath.c_str());


    {
        std::lock_guard<std::mutex> lock(g_iconIndexMutex);
        g_iconIndex.clear();
        g_iconIndexLoaded = false;
    }
    loadIconIndexIfNeeded();

    FILE* dbg3 = fopen("sdmc:/switch/TheOtherSide/debug.txt", "a");
    if (dbg3) { fprintf(dbg3, "icon index: wrote %zu entries\n", written); fclose(dbg3); }
    return written > 0;
}


std::string fetchShopIconForTitle(const std::string& titleId) {
    if (titleId.empty()) return "";


    std::string tid = titleId;
    for (auto& c : tid) c = toupper(c);


    mkdir(ICON_CACHE_DIR, 0777);
    std::string cachePath = GetIconShardDir(tid) + "/" + tid + ".jpg";
    FILE* cf = fopen(cachePath.c_str(), "rb");
    if (!cf) return "";
    fseek(cf, 0, SEEK_END);
    long sz = ftell(cf);
    if (sz < 100) { fclose(cf); return ""; }
    fseek(cf, 0, SEEK_SET);
    std::string data(sz, '\0');
    size_t read = fread(&data[0], 1, sz, cf);
    fclose(cf);
    return (read == (size_t)sz) ? data : "";
}


struct PendingIcon { std::string titleId; std::string iconBytes; uint64_t generation; };
static std::vector<PendingIcon> g_pendingIcons;
std::vector<std::string> g_pendingNotifications;
std::mutex g_pendingIconsMutex;


static std::map<std::string, std::vector<brls::ListItem*>> g_visibleShopItems;
static std::atomic<uint64_t> g_shopScreenGeneration{0};


void frame_showFileBrowser(const std::string& path);
void frame_showShop(const std::string& category, const std::string& typeFilter = "all");

static bool g_iconApplyTaskStopped = false;

class IconApplyTask : public brls::RepeatingTask {
public:
    IconApplyTask() : brls::RepeatingTask(0) {}
    ~IconApplyTask() {}
    void run(retro_time_t currentTime) override {
        brls::RepeatingTask::run(currentTime);
        if (g_iconApplyTaskStopped) return;

        std::vector<std::string> notifications;
        std::string installPath, installName;
        {
            std::lock_guard<std::mutex> lock(g_pendingIconsMutex);
            notifications.swap(g_pendingNotifications);
            if (!g_pendingInstallPath.empty()) {
                installPath = g_pendingInstallPath;
                installName = g_pendingInstallName;
                g_pendingInstallPath.clear();
                g_pendingInstallName.clear();
            }
        }
        for (auto& msg : notifications) {
            if (msg.substr(0, 7) == "UPDATE:") {

                size_t c1 = msg.find(':', 7);
                std::string tag = msg.substr(7, c1 - 7);
                std::string ver = msg.substr(c1 + 1);
                brls::Dialog* dlg = new brls::Dialog("Update available!\n\n" + tag + " is available.\nYou have v" + inst::config::appVersion + ".\n\nUpdate now?");
                dlg->addButton("Update", [dlg, tag](brls::View*) {
                    dlg->close([tag]() {

                        brls::Application::notify("Downloading update...");
                        thrd_t t;
                        thrd_create(&t, [](void* p) -> int {
                            BgThreadGuard bgGuard;
                            std::string* ptag = static_cast<std::string*>(p);
                            std::string dlUrl = "https://github.com/eradicatinglove/The-Other-Side/releases/download/" + *ptag + "/TheOtherSide.nro";
                            delete ptag;

                            CURL* curl = curl_easy_init();
                            if (!curl) {
                                std::lock_guard<std::mutex> lock(g_pendingIconsMutex);
                                g_pendingNotifications.push_back("Update download failed");
                                return 0;
                            }
                            std::string tmpNro = "sdmc:/switch/TheOtherSide/TheOtherSide_update.nro";
                            FILE* f = fopen(tmpNro.c_str(), "wb");
                            if (!f) { curl_easy_cleanup(curl); return 0; }

                            static uint64_t s_upd_last = 0; s_upd_last = 0;
                            auto xferFunc = +[](void*, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) -> int {
                                if (dltotal <= 0 || dlnow <= 0) return 0;
                                uint64_t now = (uint64_t)dlnow;
                                if (now - s_upd_last >= 1024*1024) {
                                    s_upd_last = now;
                                    double pct = 100.0*dlnow/dltotal;
                                    char msg2[48]; snprintf(msg2, sizeof(msg2), "Update: %.0f%%", pct);
                                    std::lock_guard<std::mutex> lock(g_pendingIconsMutex);
                                    g_pendingNotifications.push_back(msg2);
                                }
                                return 0;
                            };

                            curl_easy_setopt(curl, CURLOPT_URL, dlUrl.c_str());
                            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                            curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
                            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
                            curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
                            curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 512L * 1024L);
                            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
                            curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
                            curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 45L);
                            { std::string ua = "TheOtherSide/" + inst::config::appVersion; curl_easy_setopt(curl, CURLOPT_USERAGENT, ua.c_str()); }
                            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_file);
                            curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
                            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
                            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferFunc);
                            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, nullptr);
                            CURLcode res = curl_easy_perform(curl);
                            curl_easy_cleanup(curl);
                            fclose(f);

                            if (res == CURLE_OK) {
                                FILE* dbgUpd = fopen("sdmc:/switch/TheOtherSide/debug.txt", "a");
                                if (dbgUpd) { fprintf(dbgUpd, "update downloaded\n"); fclose(dbgUpd); }


                                struct stat dlSt;
                                bool dlLooksSane = stat(tmpNro.c_str(), &dlSt) == 0 && dlSt.st_size > 100000;

                                if (dlLooksSane) {


                                    std::lock_guard<std::mutex> lock(g_pendingIconsMutex);
                                    g_pendingNotifications.push_back("Update downloaded! Please restart the app to finish updating.");
                                } else {
                                    remove(tmpNro.c_str());
                                    std::lock_guard<std::mutex> lock(g_pendingIconsMutex);
                                    g_pendingNotifications.push_back("Update download looked incomplete — please try again");
                                }
                            } else {
                                FILE* dbgUpd = fopen("sdmc:/switch/TheOtherSide/debug.txt", "a");
                                if (dbgUpd) { fprintf(dbgUpd, "update download failed: rc=%d\n", res); fclose(dbgUpd); }
                                remove(tmpNro.c_str());
                                std::lock_guard<std::mutex> lock(g_pendingIconsMutex);
                                g_pendingNotifications.push_back("Update download failed");
                            }
                            return 0;
                        }, new std::string(tag));
                        thrd_detach(t);
                    });
                });
                dlg->addButton("Not now", [dlg](brls::View*) { dlg->close([](){}); });
                dlg->setCancelable(false);
                dlg->open();
            } else {
                brls::Application::notify(msg);
            }
        }

        if (!installPath.empty()) {
            brls::Dialog* instDlg = new brls::Dialog("Download complete!\n\nInstall " + installName + " now?");
            std::string fp = installPath;
            instDlg->addButton("Install", [instDlg, fp](brls::View*) {
                instDlg->close([fp]() {
                    frame_showFileBrowser("sdmc:/switch/TheOtherSide/temp/");
                });
            });
            instDlg->addButton("Not now", [instDlg](brls::View*) {
                instDlg->close([](){
                    brls::Application::notify("File saved in Browse SD Card > TheOtherSide > temp");
                });
            });
            instDlg->setCancelable(false);
            instDlg->open();
        }

        std::vector<PendingIcon> batch;
        {
            std::lock_guard<std::mutex> lock(g_pendingIconsMutex);
            if (g_pendingIcons.empty()) return;


            constexpr size_t kMaxIconsAppliedPerFrame = 4;
            if (g_pendingIcons.size() <= kMaxIconsAppliedPerFrame) {
                batch.swap(g_pendingIcons);
            } else {
                batch.assign(g_pendingIcons.end() - kMaxIconsAppliedPerFrame, g_pendingIcons.end());
                g_pendingIcons.resize(g_pendingIcons.size() - kMaxIconsAppliedPerFrame);
            }
        }
        for (auto& p : batch) {
            if (p.generation != g_shopScreenGeneration.load()) continue;
            auto it = g_visibleShopItems.find(p.titleId);
            if (it != g_visibleShopItems.end() && !p.iconBytes.empty()) {
                for (brls::ListItem* listItem : it->second)
                    listItem->setThumbnail((unsigned char*)p.iconBytes.data(), p.iconBytes.size());
            }
        }
    }
};

static IconApplyTask* g_iconApplyTask = nullptr;
static void ensureIconApplyTaskRunning() {
    if (!g_iconApplyTask) {
        g_iconApplyTask = new IconApplyTask();
        g_iconApplyTask->start();
    }
}


enum class ShopFormat : int { TinfoilLegacy = 0, CyberFoil = 1 };

struct Location {
    std::string protocol, url, port, username, password, path;
    ShopFormat format = ShopFormat::TinfoilLegacy;
};


static std::string sanitizeHostInput(std::string input) {

    size_t schemePos = input.find("://");
    if (schemePos != std::string::npos)
        input = input.substr(schemePos + 3);

    size_t pathPos = input.find_first_of("/?#");
    if (pathPos != std::string::npos)
        input = input.substr(0, pathPos);

    size_t start = input.find_first_not_of(" \t\r\n");
    size_t end   = input.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return input.substr(start, end - start + 1);
}

void saveLocations(const std::vector<Location>& locs) {

    mkdir(LOCATIONS_DIR, 0777);
    FILE* f = fopen(LOCATIONS_PATH, "w");
    if (!f) return;
    FILE* dbg = fopen("sdmc:/switch/TheOtherSide/debug.txt", "a");
    for (auto& loc : locs) {
        fprintf(f, "%s|%s|%s|%s|%s|%s|%d\n",
            loc.protocol.c_str(), loc.url.c_str(), loc.port.c_str(),
            loc.username.c_str(), loc.password.c_str(), loc.path.c_str(),
            (int)loc.format);
        if (dbg)
            fprintf(dbg, "saveLocations: proto=[%s] url=[%s] port=[%s] user=[%s] path=[%s] format=[%d]\n",
                loc.protocol.c_str(), loc.url.c_str(), loc.port.c_str(),
                loc.username.c_str(), loc.path.c_str(), (int)loc.format);
    }
    if (dbg) fclose(dbg);
    fclose(f);
}

std::vector<Location> loadLocations() {
    std::vector<Location> locs;


    FILE* test = fopen(LOCATIONS_PATH, "r");
    if (!test) {
        mkdir(LOCATIONS_DIR, 0777);
        FILE* f = fopen(LOCATIONS_PATH, "w");
        if (f) fclose(f);
        test = fopen(LOCATIONS_PATH, "r");
        if (!test) return locs;
    }
    FILE* f = test;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        int len = strlen(line);
        while (len > 0 && (line[len-1]=='\n'||line[len-1]=='\r')) line[--len]=0;
        if (!len || line[0]=='#') continue;
        Location loc;
        std::vector<std::string> parts;


        std::string lineStr(line);
        size_t pos = 0;
        while (true) {
            size_t next = lineStr.find('|', pos);
            if (next == std::string::npos) {
                parts.push_back(lineStr.substr(pos));
                break;
            }
            parts.push_back(lineStr.substr(pos, next - pos));
            pos = next + 1;
        }
        if (parts.size() > 0) loc.protocol = parts[0];
        if (parts.size() > 1) loc.url      = parts[1];
        if (parts.size() > 2) loc.port     = parts[2];
        if (parts.size() > 3) loc.username = parts[3];
        if (parts.size() > 4) loc.password = parts[4];
        if (parts.size() > 5) loc.path     = parts[5];
        if (parts.size() > 6 && !parts[6].empty()) {

            loc.format = (parts[6] == "1") ? ShopFormat::CyberFoil : ShopFormat::TinfoilLegacy;
        } else {


            loc.format = (loc.url.find("ghostland.at") != std::string::npos)
                ? ShopFormat::CyberFoil : ShopFormat::TinfoilLegacy;
        }
        if (!loc.url.empty()) locs.push_back(loc);
    }
    fclose(f);
    return locs;
}


// grabs the shop listings on startup
void doFetch() {
    auto locs = loadLocations();
    if (locs.empty()) {
        g_fetchStatus = "No shops configured. Go to Options to add one.";
        g_fetchDone   = true;
        return;
    }

    {
        NifmInternetConnectionType connType;
        u32 wifiStrength;
        NifmInternetConnectionStatus connStatus;
        Result rc = nifmGetInternetConnectionStatus(&connType, &wifiStrength, &connStatus);
        FILE* dbg = fopen("sdmc:/switch/TheOtherSide/debug.txt", "a");
        if (dbg) {
            fprintf(dbg, "nifm rc: 0x%x connType:%d wifiStr:%u connStatus:%d\n",
                rc, (int)connType, wifiStrength, (int)connStatus);
            fclose(dbg);
        }
        if (R_FAILED(rc) || connStatus != NifmInternetConnectionStatus_Connected) {
            g_fetchStatus = "No internet connection";
            g_fetchDone = true;
            return;
        }
    }

    for (auto& loc : locs) {
        if (g_fetchCancel) break;
        g_fetchStatus = "Fetching " + loc.url + "...";

        std::string json;


        std::string proto = loc.protocol.empty() ? "https" : loc.protocol;
        for (auto& c : proto) c = tolower(c);
        std::string rootUrl = proto + "://" + loc.url;
        if (!loc.port.empty()) rootUrl += ":" + loc.port;
        if (!loc.path.empty()) {
            if (loc.path[0] != '/') rootUrl += "/";
            rootUrl += loc.path;
        }
        if (!rootUrl.empty() && rootUrl.back() == '/') rootUrl.pop_back();


        bool isGhostland = loc.url.find("ghostland.at") != std::string::npos;
        if (isGhostland || loc.format == ShopFormat::CyberFoil) {
            std::string cfUrl;
            std::string hauthHeader;
            if (isGhostland) {
                cfUrl = proto + "://nx.ghostland.at/api/shop/sections";
                hauthHeader = "HAUTH: D0634E67FCF4DBD14DA344ACDC45E4BE";
            } else {


                cfUrl = loc.path.empty()
                    ? (proto + "://" + loc.url + (loc.port.empty() ? "" : (":" + loc.port)) + "/api/shop/sections")
                    : rootUrl;


                hauthHeader = "HAUTH: " + inst::util::ComputeHauthFromUrl(cfUrl);
            }
            std::string uid = inst::util::ComputeUidFromMmcCid();

            struct curl_slist* gh = nullptr;
            gh = curl_slist_append(gh, "Theme: 0000000000000000000000000000000000000000000000000000000000000000");
            gh = curl_slist_append(gh, "Version: 1.4");
            gh = curl_slist_append(gh, "Revision: 5");
            gh = curl_slist_append(gh, "Language: en");
            gh = curl_slist_append(gh, hauthHeader.c_str());
            std::string ua = "UAUTH: " + inst::util::ComputeUauthFromUrl(cfUrl, loc.username, loc.password);
            gh = curl_slist_append(gh, ua.c_str());
            std::string ui = "UID: " + uid;
            gh = curl_slist_append(gh, ui.c_str());

            CURL* curl = curl_easy_init();
            if (curl) {
                curl_easy_setopt(curl, CURLOPT_URL, cfUrl.c_str());
                curl_easy_setopt(curl, CURLOPT_USERAGENT, "cyberfoil");
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
                curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
                curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
                curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, gh);
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &json);


                curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
                curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                    +[](void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
                        return g_fetchCancel ? 1 : 0;
                    });
                curl_easy_setopt(curl, CURLOPT_XFERINFODATA, nullptr);
                CURLcode res = curl_easy_perform(curl);
                long httpCode = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
                curl_easy_cleanup(curl);
                FILE* dbg = fopen("sdmc:/switch/TheOtherSide/debug.txt", "a");
                if (dbg) {
                    fprintf(dbg, "%s: rc=%d http=%ld size=%zu\n",
                            isGhostland ? "Ghost eShop" : ("CyberFoil shop [" + loc.url + "]").c_str(),
                            res, httpCode, json.size());
                    fclose(dbg);
                }
            }
            curl_slist_free_all(gh);


            if (!json.empty() && json.find("\"sections\"") != std::string::npos) {
                size_t pos = 0;
                while ((pos = json.find("\"title_id\"", pos)) != std::string::npos) {
                    size_t objStart = json.rfind('{', pos);
                    size_t objEnd = json.find('}', pos);
                    if (objStart == std::string::npos || objEnd == std::string::npos) { pos++; continue; }
                    std::string obj = json.substr(objStart, objEnd - objStart + 1);
                    auto getF = [&](const std::string& k) -> std::string {
                        std::string needle = "\"" + k + "\"";
                        size_t p = obj.find(needle);
                        if (p == std::string::npos) return "";
                        size_t c = obj.find(':', p + needle.size());
                        if (c == std::string::npos) return "";
                        size_t v = obj.find_first_not_of(" \t\n\r", c + 1);
                        if (v == std::string::npos) return "";
                        if (obj[v] == '"') {
                            size_t e = obj.find('"', v + 1);
                            return e == std::string::npos ? "" : obj.substr(v + 1, e - v - 1);
                        }
                        size_t e = obj.find_first_of(",}", v);
                        return obj.substr(v, e - v);
                    };
                    std::string tid = getF("title_id");
                    std::string name = getF("name");
                    std::string url = getF("url");
                    std::string iconUrl = getF("icon_url");

                    if (!tid.empty() && !name.empty() && !url.empty()) {

                        if (url.size() > 5 && url.substr(0, 5) == "jbod:") {
                            size_t slash = url.find('/', 5);
                            if (slash != std::string::npos) {
                                std::string encoded = url.substr(slash + 1);
                                std::string decoded;
                                for (size_t i = 0; i < encoded.size(); i++) {
                                    if (encoded[i] == '%' && i + 2 < encoded.size()) {
                                        int val; sscanf(encoded.substr(i+1,2).c_str(), "%x", &val);
                                        decoded += (char)val; i += 2;
                                    } else decoded += encoded[i];
                                }
                                size_t hash = decoded.find('#');
                                if (hash != std::string::npos) decoded = decoded.substr(0, hash);
                                url = decoded;
                            }
                        }
                        {
                            std::lock_guard<std::mutex> lock(g_titleDataMutex);
                            g_titleNames.push_back(name);
                            g_titleIds.push_back(tid);
                            g_titleUrls.push_back(url);
                            g_titleIconUrls.push_back(iconUrl);
                        }
                    }
                    pos = objEnd + 1;
                }
                bool anyTitles;
                {
                    std::lock_guard<std::mutex> lock(g_titleDataMutex);
                    anyTitles = !g_titleNames.empty();
                    if (anyTitles) g_fetchStatus = std::to_string(g_titleNames.size()) + " titles loaded";
                }
                if (anyTitles) {
                    g_fetchDone = true;
                    return;
                }
            }
            json.clear();
            continue;
        }

        json = httpGet(rootUrl, loc.username, loc.password);


        if (!json.empty() && json.compare(0, 7, "TINFOIL") == 0) {
            std::string decoded = decodeTinfoilResponse(json);
            FILE* dbg = fopen("sdmc:/switch/TheOtherSide/debug.txt", "a");
            if (dbg) {
                fprintf(dbg, "Detected TINFOIL container, decoded size: %zu\n", decoded.size());
                fclose(dbg);
            }
            json = decoded;
        }


        if (!json.empty() && json.find("\"files\"") != std::string::npos) {

            size_t filesPos = json.find("\"files\"");
            size_t arrStart = json.find('[', filesPos);
            size_t arrEnd   = json.rfind(']');
            if (arrStart != std::string::npos && arrEnd != std::string::npos) {
                std::string arr = json.substr(arrStart, arrEnd - arrStart + 1);
                size_t pos = 0;
                while ((pos = arr.find("{", pos)) != std::string::npos) {
                    size_t end = arr.find("}", pos);
                    if (end == std::string::npos) break;
                    std::string obj = arr.substr(pos, end - pos + 1);
                    pos = end + 1;
                    auto getF = [&](const std::string& key) {
                        std::string needle = "\"" + key + "\"";
                        size_t p = obj.find(needle);
                        if (p == std::string::npos) return std::string();
                        p = obj.find('"', p + needle.size() + 1);
                        if (p == std::string::npos) return std::string();
                        size_t e = obj.find('"', p + 1);
                        if (e == std::string::npos) return std::string();
                        return obj.substr(p + 1, e - p - 1);
                    };
                    std::string url = getF("url");
                    if (url.empty()) continue;


                    bool isAbsolute = url.find("://") != std::string::npos
                                   || url.substr(0,7) == "gdrive:"
                                   || url.substr(0,4) == "ftp:";
                    if (!isAbsolute) {
                        std::string base = (loc.protocol.empty() ? "https" : loc.protocol);
                        for (auto& c : base) c = tolower(c);
                        base += "://" + loc.url;
                        if (!loc.port.empty()) base += ":" + loc.port;
                        if (!url.empty() && url[0] != '/') base += "/";
                        url = base + url;
                    }

                    std::string name = url;
                    size_t sl = name.rfind('/');
                    if (sl != std::string::npos) name = name.substr(sl + 1);

                    std::string decoded;
                    for (size_t i = 0; i < name.size(); i++) {
                        if (name[i] == '%' && i + 2 < name.size()) {
                            int val = 0;
                            sscanf(name.substr(i+1,2).c_str(), "%x", &val);
                            decoded += (char)val;
                            i += 2;
                        } else decoded += name[i];
                    }

                    std::string tid, cleanName = decoded;
                    size_t lb = decoded.find('[');
                    size_t rb = decoded.find(']', lb != std::string::npos ? lb : 0);
                    if (lb != std::string::npos && rb != std::string::npos && rb - lb - 1 == 16)
                        tid = decoded.substr(lb + 1, 16);
                    size_t nameEnd = lb != std::string::npos ? lb : decoded.size();
                    while (nameEnd > 0 && (decoded[nameEnd-1]==' '||decoded[nameEnd-1]=='.')) nameEnd--;
                    if (nameEnd > 0) cleanName = decoded.substr(0, nameEnd);
                    if (!url.empty()) {
                        std::lock_guard<std::mutex> lock(g_titleDataMutex);
                        g_titleNames.push_back(cleanName.empty() ? decoded : cleanName);
                        g_titleIds.push_back(tid);
                        g_titleUrls.push_back(url);
                        g_titleIconUrls.push_back("");
                    }
                }
            }
            {
                std::lock_guard<std::mutex> lock(g_titleDataMutex);
                g_fetchStatus = std::to_string(g_titleNames.size()) + " titles loaded";
            }
            g_fetchDone = true;


            g_shopUser = loc.username;
            g_shopPass = loc.password;
            tin::network::SetBasicAuth(g_shopUser, g_shopPass);
            return;
        }

        if (json.empty()) {
            g_fetchStatus = "Failed to reach: " + loc.url;
            continue;
        }


        if (json.find("\"locations\"") != std::string::npos) {


            size_t locPos = json.find("\"locations\"");
            size_t objStart = json.find('{', locPos + 10);
            if (objStart != std::string::npos) {

                size_t p = objStart + 1;
                while (p < json.size() && p < objStart + 50000) {

                    size_t ks = json.find('"', p);
                    if (ks == std::string::npos) break;
                    size_t ke = json.find('"', ks + 1);
                    if (ke == std::string::npos) break;
                    std::string host = json.substr(ks + 1, ke - ks - 1);
                    if (host.empty() || host[0] == '}') { p = ke + 1; continue; }

                    std::string path;
                    size_t pathKey = json.find("\"path\"", ke);
                    size_t nextHost = json.find('"', ke + 2);
                    if (pathKey != std::string::npos && pathKey < nextHost + 200) {
                        size_t pvs = json.find('"', pathKey + 6);
                        size_t pve = json.find('"', pvs + 1);
                        if (pvs != std::string::npos && pve != std::string::npos)
                            path = json.substr(pvs + 1, pve - pvs - 1);
                    }
                    if (!host.empty() && host.find('.') != std::string::npos) {
                        std::string fwdUrl = "https://" + host;
                        if (!path.empty()) { if (path[0]!='/') fwdUrl+="/"; fwdUrl+=path; }
                        g_fetchStatus = "Fetching " + host + "...";
                        std::string fwdJson = httpGet(fwdUrl, "", "");
                        if (!fwdJson.empty() && fwdJson.compare(0, 7, "TINFOIL") == 0)
                            fwdJson = decodeTinfoilResponse(fwdJson);
                        if (!fwdJson.empty() && fwdJson.find("\"files\"") != std::string::npos) {


                            json += fwdJson;
                        }
                    }
                    p = ke + 1;
                }
            }
        }


        size_t pos = 0;
        while ((pos = json.find("{\"url\"", pos)) != std::string::npos) {
            size_t end = json.find('}', pos);
            if (end == std::string::npos) break;
            std::string obj = json.substr(pos, end-pos+1);
            pos = end+1;


            std::string url, name;
            auto getField = [&](const std::string& key) {
                std::string needle = "\"" + key + "\"";
                size_t p = obj.find(needle);
                if (p == std::string::npos) return std::string("");
                p = obj.find('"', p + needle.size() + 1);
                if (p == std::string::npos) return std::string("");
                size_t e = obj.find('"', p+1);
                if (e == std::string::npos) return std::string("");
                return obj.substr(p+1, e-p-1);
            };
            url  = getField("url");
            name = getField("name");

            if (url.empty() && !name.empty()) {
                url = loc.url;
                if (!url.empty() && url.back()!='/') url += '/';
                url += name;
            }


            std::string tid;
            size_t lb = name.find('[');
            size_t rb = name.find(']', lb != std::string::npos ? lb : 0);
            if (lb != std::string::npos && rb != std::string::npos && rb-lb-1 == 16)
                tid = name.substr(lb+1, 16);


            std::string cleanName = name;
            size_t nameEnd = lb != std::string::npos ? lb : name.size();
            while (nameEnd > 0 && (name[nameEnd-1]==' '||name[nameEnd-1]=='.')) nameEnd--;
            if (nameEnd > 0) cleanName = name.substr(0, nameEnd);

            if (!url.empty() || !tid.empty()) {
                std::lock_guard<std::mutex> lock(g_titleDataMutex);
                g_titleNames.push_back(cleanName.empty() ? tid : cleanName);
                g_titleIds.push_back(tid);
                g_titleUrls.push_back(url);
                g_titleIconUrls.push_back("");
            }
        }
    }
    g_fetchDone   = true;
    {
        std::lock_guard<std::mutex> lock(g_titleDataMutex);
        g_fetchStatus = std::to_string(g_titleNames.size()) + " titles loaded";
    }
}


// installed games tab
void frame_showInstalled() {
    brls::AppletFrame* frame = new brls::AppletFrame(true, true);
    frame->setTitle("Installed Titles");

    brls::List* list = new brls::List();

    NsApplicationRecord* records = new NsApplicationRecord[4096]();
    s32 count = 0;
    nsListApplicationRecord(records, 4096, 0, &count);

    if (count == 0) {
        list->addView(new brls::ListItem("No installed titles found"));
    } else {
        constexpr int COLS = 4;
        NsApplicationControlData* ctrl = new NsApplicationControlData();

        brls::BoxLayout* row = nullptr;
        int col = 0;

        for (s32 i = 0; i < count; i++) {
            char name[0x201] = {};
            char tid[32]     = {};
            snprintf(tid, sizeof(tid), "%016lX", records[i].application_id);
            u64 appId = records[i].application_id;

            unsigned char* iconBuf = nullptr;
            size_t iconSize = 0;
            size_t ctrlSize = 0;

            if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage,
                    records[i].application_id, ctrl, sizeof(*ctrl), &ctrlSize))) {
                NacpLanguageEntry* lang = nullptr;
                if (R_SUCCEEDED(nacpGetLanguageEntry(&ctrl->nacp, &lang)) && lang)
                    strncpy(name, lang->name, sizeof(name)-1);
                if (ctrlSize > sizeof(ctrl->nacp)) {
                    iconSize = ctrlSize - sizeof(ctrl->nacp);
                    iconBuf  = new unsigned char[iconSize];
                    memcpy(iconBuf, ctrl->icon, iconSize);
                }
            }
            if (!name[0]) strncpy(name, tid, sizeof(name)-1);


            if (col == 0) {
                row = new brls::BoxLayout(brls::BoxLayoutOrientation::HORIZONTAL);
                row->setHeight(200);
                row->setSpacing(4);
            }


            brls::ListItem* cell = new brls::ListItem(name);
            cell->setWidth(280);
            if (iconBuf)
                cell->setThumbnail(iconBuf, iconSize);


            cell->getClickEvent()->subscribe([appId](brls::View*) {
                accountInitialize(AccountServiceType_Application);
                AccountUid uid = {};
                accountGetPreselectedUser(&uid);
                accountExit();
                Result rc = appletRequestLaunchApplication(appId, nullptr);
                if (R_FAILED(rc))
                    brls::Application::notify("Launch failed");
            });

            row->addView(cell, false);
            col++;

            if (col == COLS) {
                list->addView(row);
                row = nullptr;
                col = 0;
            }
        }

        if (row) list->addView(row);

        delete ctrl;
    }
    delete[] records;

    frame->setContentView(list);
    brls::Application::pushView(frame);
}


// browse tab
void frame_showFileBrowser(const std::string& path) {
    brls::AppletFrame* frame = new brls::AppletFrame(true, true);
    frame->setTitle("File Browser");
    frame->setIcon(BOREALIS_ASSET("icon/filebrowser.jpg"));
    brls::List* list = new brls::List();

    std::vector<std::string> dirs, files;
    DIR* d = opendir(path.c_str());
    if (d) {
        struct dirent* ent;
        while ((ent = readdir(d))) {
            if (ent->d_name[0]=='.') continue;
            std::string name = ent->d_name;
            std::string fp   = path + (path.back()=='/' ? "" : "/") + name;
            if (ent->d_type == DT_DIR) dirs.push_back(fp);
            else {
                std::string ext = name.size()>=4 ? name.substr(name.size()-4) : "";
                for (auto& c : ext) c = tolower(c);
                if (ext==".nsp"||ext==".xci"||ext==".nsz"||ext==".xcz")
                    files.push_back(fp);
            }
        }
        closedir(d);
    }

    if (path != "sdmc:/") {
        brls::ListItem* up = new brls::ListItem("[..] Go up");
        std::string parent = path;
        if (!parent.empty() && parent.back()=='/') parent.pop_back();
        size_t sl = parent.rfind('/');
        parent = (sl != std::string::npos && sl > 6) ? parent.substr(0, sl+1) : "sdmc:/";
        up->getClickEvent()->subscribe([parent](brls::View*) {
            brls::Application::popView();
        });
        list->addView(up);
    }

    for (auto& dp : dirs) {
        std::string dn = dp.substr(dp.rfind('/')+1);
        brls::ListItem* item = new brls::ListItem(dn, "folder");
        item->getClickEvent()->subscribe([dp](brls::View*) { frame_showFileBrowser(dp); });
        list->addView(item);
    }
    for (auto& fp : files) {
        std::string fn = fp.substr(fp.rfind('/')+1);
        brls::ListItem* item = new brls::ListItem(fn, fp);
        item->getClickEvent()->subscribe([fn, fp](brls::View*) {

            brls::Dialog* dlg = new brls::Dialog("Install to SD card?\n\n" + fn);
            dlg->addButton("SD Card", [dlg, fn, fp](brls::View*) {
                dlg->close([fn, fp](){
                    inst::ui::instPage::clearInstallCancel();

                    struct InstArgs { std::string fp, fn; };
                    auto* args = new InstArgs{fp, fn};
                    thrd_t t;
                    thrd_create(&t, [](void* p) -> int {
                        BgThreadGuard bgGuard;
                        auto* a = static_cast<InstArgs*>(p);

                        ncmInitialize();
                        nsextInitialize();
                        esInitialize();
                        splCryptoInitialize();
                        splInitialize();
                        try {
                            inst::ui::instPage::clearInstallCancel();
                            std::string ext = a->fn.size() >= 4 ? a->fn.substr(a->fn.size()-4) : "";
                            for (auto& c : ext) c = tolower(c);
                            inst::ui::instPage::setInstInfoText("Installing: " + a->fn);
                            if (ext == ".nsp" || ext == ".nsz") {
                                auto nsp = std::make_shared<tin::install::nsp::SDMCNSP>(a->fp);
                                tin::install::nsp::NSPInstall task(NcmStorageId_SdCard, false, nsp);
                                task.Prepare();
                                task.Begin();
                            } else if (ext == ".xci" || ext == ".xcz") {
                                auto xci = std::make_shared<tin::install::xci::SDMCXCI>(a->fp);
                                tin::install::xci::XCIInstallTask task(NcmStorageId_SdCard, false, xci);
                                task.Prepare();
                                task.Begin();
                            }
                            inst::ui::instPage::setInstInfoText("Done: " + a->fn);

                            if (a->fp.find("/TheOtherSide/temp/") != std::string::npos)
                                remove(a->fp.c_str());
                            {
                                std::lock_guard<std::mutex> lock(g_pendingIconsMutex);
                                g_pendingNotifications.push_back("Installed: " + a->fn);
                            }
                        } catch (std::exception& e) {
                            std::string err = e.what();
                            inst::ui::instPage::setInstInfoText("Failed: " + err);
                            {
                                std::lock_guard<std::mutex> lock(g_pendingIconsMutex);
                                g_pendingNotifications.push_back("Failed: " + err.substr(0, 60));
                            }
                        }
                        splExit();
                        splCryptoExit();
                        esExit();
                        nsextExit();
                        ncmExit();
                        delete a;
                        return 0;
                    }, args);
                    thrd_detach(t);
                    brls::Application::notify("Installing " + fn + "...");
                });
            });
            dlg->addButton("Cancel", [dlg](brls::View*) { dlg->close([](){}); });
            dlg->setCancelable(true);
            dlg->open();
        });
        list->addView(item);
    }
    if (dirs.empty() && files.empty())
        list->addView(new brls::ListItem("(empty)", "No NSP/XCI/NSZ files here"));

    frame->setContentView(list);
    brls::Application::pushView(frame);
}


static brls::ListItem* buildShopTitleItem(const std::string& name, const std::string& tid, const std::string& url, const std::string& iconUrl) {

    std::string tidUpper = tid;
    for (auto& c : tidUpper) c = toupper(c);
    std::string iconShardDir = tidUpper.empty() ? std::string(ICON_CACHE_DIR) : GetIconShardDir(tidUpper);
    std::string iconPathJpg = iconShardDir + "/" + tidUpper + ".jpg";
    std::string iconPathPng = iconShardDir + "/" + tidUpper + ".png";
    std::string iconPathWebp = iconShardDir + "/" + tidUpper + ".webp";
    std::string iconPath = iconPathJpg;
    bool hasIcon;
    {


        std::lock_guard<std::mutex> lock(g_iconCacheSetMutex);
        auto it = g_iconCacheSet.find(tidUpper);
        hasIcon = !tidUpper.empty() && it != g_iconCacheSet.end();
        if (hasIcon) iconPath = iconShardDir + "/" + tidUpper + it->second;
    }


    uint64_t offlineDbTidNum = 0;
    bool offlineDbPackHas = false;
    if (!hasIcon && !tidUpper.empty()) {
        offlineDbTidNum = strtoull(tidUpper.c_str(), nullptr, 16);
        uint64_t hasIconT0 = armGetSystemTick();
        offlineDbPackHas = offlineDbTidNum != 0 && inst::offline::HasIcon(offlineDbTidNum);
        g_hasIconTicksAccum.fetch_add(armGetSystemTick() - hasIconT0, std::memory_order_relaxed);
    }


    brls::ListItem* item = hasIcon
        ? new brls::ListItem(name)
        : new brls::ListItem(name, tid);

    if (hasIcon) {
        item->setHeight(140);


        uint64_t readT0 = armGetSystemTick();
        FILE* f = fopen(iconPath.c_str(), "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            if (sz > 0) {
                fseek(f, 0, SEEK_SET);
                std::string bytes;
                bytes.resize((size_t)sz);
                if (fread(&bytes[0], 1, (size_t)sz, f) == (size_t)sz) {
                    uint64_t gen = g_shopScreenGeneration.load();
                    std::lock_guard<std::mutex> lock(g_pendingIconsMutex);
                    g_visibleShopItems[tidUpper].push_back(item);
                    g_pendingIcons.push_back(PendingIcon{tidUpper, bytes, gen});
                }
            }
            fclose(f);
        }
        g_readTicksAccum.fetch_add(armGetSystemTick() - readT0, std::memory_order_relaxed);
    } else if (!tidUpper.empty()) {


        std::string effectiveUrl = iconUrl;
        std::string effectivePath = iconPathPng;
        bool useOfflineDbPack = false;
        if (effectiveUrl.empty() && offlineDbPackHas) {
            useOfflineDbPack = true;
        } else if (effectiveUrl.empty()) {
            loadIconIndexIfNeeded();
            std::string indexIconUrl;
            std::lock_guard<std::mutex> lock(g_iconIndexMutex);
            auto it = g_iconIndex.find(tidUpper);
            if (it != g_iconIndex.end()) indexIconUrl = it->second;
            effectiveUrl = indexIconUrl;
            effectivePath = iconPathJpg;
        }
        if (useOfflineDbPack &&
            g_activeIconFetches.load(std::memory_order_relaxed) < kMaxConcurrentIconFetches) {


            g_activeIconFetches.fetch_add(1, std::memory_order_relaxed);
            uint64_t gen = g_shopScreenGeneration.load();
            {
                std::lock_guard<std::mutex> lock(g_pendingIconsMutex);
                g_visibleShopItems[tidUpper].push_back(item);
            }
            struct OfflineDbFetchArgs { std::string tid, path; uint64_t tidNum; uint64_t gen; };
            auto* args = new OfflineDbFetchArgs{tidUpper, iconPathWebp, offlineDbTidNum, gen};
            thrd_t t;
            thrd_create(&t, [](void* p) -> int {
                BgThreadGuard bgGuard;
                struct FetchSlotGuard {
                    ~FetchSlotGuard() { g_activeIconFetches.fetch_sub(1, std::memory_order_relaxed); }
                } slotGuard;
                std::unique_ptr<OfflineDbFetchArgs> a(static_cast<OfflineDbFetchArgs*>(p));
                if (a->gen != g_shopScreenGeneration.load()) return 0;

                std::vector<uint8_t> imgBytes;
                if (inst::offline::TryGetIconData(a->tidNum, imgBytes) && !imgBytes.empty()) {
                    FILE* f = fopen(a->path.c_str(), "wb");
                    if (f) { fwrite(imgBytes.data(), 1, imgBytes.size(), f); fclose(f); }
                    {
                        std::lock_guard<std::mutex> lock(g_iconCacheSetMutex);
                        g_iconCacheSet[a->tid] = ".webp";
                    }
                    std::lock_guard<std::mutex> lock(g_pendingIconsMutex);
                    g_pendingIcons.push_back(PendingIcon{a->tid, std::string(imgBytes.begin(), imgBytes.end()), a->gen});
                }
                return 0;
            }, args);
            thrd_detach(t);
        } else if (!effectiveUrl.empty() &&
            g_activeIconFetches.load(std::memory_order_relaxed) < kMaxConcurrentIconFetches) {
            g_activeIconFetches.fetch_add(1, std::memory_order_relaxed);
            uint64_t gen = g_shopScreenGeneration.load();
            {
                std::lock_guard<std::mutex> lock(g_pendingIconsMutex);
                g_visibleShopItems[tidUpper].push_back(item);
            }
            struct IconFetchArgs { std::string tid, url, path; uint64_t gen; };
            auto* args = new IconFetchArgs{tidUpper, effectiveUrl, effectivePath, gen};
            thrd_t t;
            thrd_create(&t, [](void* p) -> int {
                BgThreadGuard bgGuard;
                struct FetchSlotGuard {
                    ~FetchSlotGuard() { g_activeIconFetches.fetch_sub(1, std::memory_order_relaxed); }
                } slotGuard;
                std::unique_ptr<IconFetchArgs> a(static_cast<IconFetchArgs*>(p));


                if (a->gen != g_shopScreenGeneration.load()) return 0;

                std::string bytes;
                CURL* curl = curl_easy_init();
                if (curl) {
                    curl_easy_setopt(curl, CURLOPT_URL, a->url.c_str());
                    curl_easy_setopt(curl, CURLOPT_USERAGENT, TINCLONE_UA);
                    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
                    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
                    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
                    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
                    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
                    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &bytes);


                    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
                    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                        +[](void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
                            return g_fetchCancel ? 1 : 0;
                        });
                    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, nullptr);
                    CURLcode res = curl_easy_perform(curl);
                    long httpCode = 0;
                    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
                    curl_easy_cleanup(curl);
                    if (res != CURLE_OK || httpCode != 200 || bytes.size() < 100) bytes.clear();
                }

                if (!bytes.empty()) {

                    FILE* f = fopen(a->path.c_str(), "wb");
                    if (f) { fwrite(bytes.data(), 1, bytes.size(), f); fclose(f); }
                    {
                        std::lock_guard<std::mutex> lock(g_iconCacheSetMutex);
                        size_t dot = a->path.find_last_of('.');
                        g_iconCacheSet[a->tid] = dot != std::string::npos ? a->path.substr(dot) : ".jpg";
                    }
                    std::lock_guard<std::mutex> lock(g_pendingIconsMutex);
                    g_pendingIcons.push_back(PendingIcon{a->tid, bytes, a->gen});
                }
                return 0;
            }, args);
            thrd_detach(t);
        }
    }

    item->getClickEvent()->subscribe([name, url](brls::View*) {
        std::string n = name, u = url;
        brls::Dialog* dlg = new brls::Dialog("Install from shop?\n\n" + n);
        dlg->addButton("Install", [dlg, n, u](brls::View*) {
            dlg->close([n, u](){
                FILE* dbgPre = fopen("sdmc:/switch/TheOtherSide/debug.txt", "a");
                if (dbgPre) { fprintf(dbgPre, "install button pressed: %s -> %s\n", n.c_str(), u.c_str()); fclose(dbgPre); }

                if (g_installInProgress.load()) {
                    brls::Application::notify("Install already in progress - please wait");
                    return;
                }
                g_installInProgress.store(true);
                ensureIconApplyTaskRunning();
                inst::ui::instPage::clearInstallCancel();
                struct NetArgs { std::string u, n, user, pass; };
                auto* args = new NetArgs{u, n, g_shopUser, g_shopPass};
                thrd_t t;
                int createResult = thrd_create(&t, [](void* p) -> int {
                    BgThreadGuard bgGuard;
                    auto* a = static_cast<NetArgs*>(p);
                    if (!a->user.empty())
                        tin::network::SetBasicAuth(a->user, a->pass);
                    FILE* dbg = fopen("sdmc:/switch/TheOtherSide/debug.txt", "a");
                    if (dbg) { fprintf(dbg, "install start: %s -> %s\n", a->n.c_str(), a->u.c_str()); fclose(dbg); }

                    std::string lurl = a->u;
                    for (auto& c : lurl) c = tolower(c);
                    std::string ext = ".nsp";
                    if (lurl.find(".nsz") != std::string::npos) ext = ".nsz";
                    else if (lurl.find(".xci") != std::string::npos) ext = ".xci";
                    else if (lurl.find(".xcz") != std::string::npos) ext = ".xcz";

                    const std::string tempDir = "sdmc:/switch/TheOtherSide/temp";
                    std::string safeId;
                    for (char c : a->n) { if (isalnum(c)||c==' '||c=='-') safeId+=c; if(safeId.size()>40)break; }
                    if (safeId.empty()) safeId = "game";
                    const std::string tempPath = tempDir + "/" + safeId + ext;
                    mkdir(tempDir.c_str(), 0777);

                    try {

                        {
                            std::lock_guard<std::mutex> lock(g_pendingIconsMutex);
                            g_pendingNotifications.push_back("Downloading " + a->n + "...");
                        }

                        std::string dlUrl = a->u;
                        if (!a->user.empty()) {
                            size_t se = dlUrl.find("://");
                            if (se != std::string::npos)
                                dlUrl = dlUrl.substr(0,se+3)+a->user+":"+a->pass+"@"+dlUrl.substr(se+3);
                        }

                        FILE* outFile = fopen(tempPath.c_str(), "wb");
                        if (!outFile) throw std::runtime_error("Failed to open temp file");
                        CURL* curl = curl_easy_init();
                        if (!curl) { fclose(outFile); throw std::runtime_error("curl_easy_init failed"); }

                        uint64_t totalBytes = 0;
                        struct DlCtx { FILE* f; uint64_t* total; };
                        DlCtx dlCtx{outFile, &totalBytes};

                        auto writeFunc = +[](char* ptr, size_t sz, size_t n, void* ud) -> size_t {
                            auto* ctx = static_cast<DlCtx*>(ud);
                            size_t w = fwrite(ptr, sz, n, ctx->f);
                            *ctx->total += w * sz;
                            return w * sz;
                        };
                        static int s_lastNotifyPct = -1; s_lastNotifyPct = -1;
                        auto xferFunc = +[](void*, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) -> int {
                            if (dltotal<=0||dlnow<=0) return 0;
                            int pct = (int)(100.0*dlnow/dltotal);
                            if (pct > s_lastNotifyPct) {
                                s_lastNotifyPct = pct;
                                double mbNow=dlnow/1048576.0, mbTotal=dltotal/1048576.0;
                                char msg[64]; snprintf(msg,sizeof(msg),"DL %.0f/%.0f MB (%d%%)",mbNow,mbTotal,pct);
                                std::lock_guard<std::mutex> lock(g_pendingIconsMutex);
                                g_pendingNotifications.push_back(msg);
                            }
                            return 0;
                        };

                        curl_easy_setopt(curl, CURLOPT_URL, dlUrl.c_str());
                        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, false);
                        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
                        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
                        curl_easy_setopt(curl, CURLOPT_USERAGENT, "");
                        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &dlCtx);
                        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFunc);
                        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
                        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferFunc);
                        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, nullptr);

                        CURLcode rc = curl_easy_perform(curl);
                        curl_easy_cleanup(curl);
                        fclose(outFile);

                        if (rc != CURLE_OK) {
                            remove(tempPath.c_str());
                            throw std::runtime_error(std::string("Download failed: ")+curl_easy_strerror(rc));
                        }

                        FILE* dbg2 = fopen("sdmc:/switch/TheOtherSide/debug.txt","a");
                        if (dbg2) { fprintf(dbg2,"download complete: %llu bytes\n",(unsigned long long)totalBytes); fclose(dbg2); }


                        {
                            std::lock_guard<std::mutex> lock(g_pendingIconsMutex);
                            g_pendingNotifications.push_back("Installing...");
                        }
                        ncmInitialize();
                        nsextInitialize();
                        esInitialize();
                        splCryptoInitialize();
                        splInitialize();

                        if (ext == ".xci" || ext == ".xcz") {
                            auto xci = std::make_shared<tin::install::xci::SDMCXCI>(tempPath);
                            tin::install::xci::XCIInstallTask task(NcmStorageId_SdCard, false, xci);
                            task.Prepare();
                            task.Begin();
                        } else {
                            auto nsp = std::make_shared<tin::install::nsp::SDMCNSP>(tempPath);
                            tin::install::nsp::NSPInstall task(NcmStorageId_SdCard, false, nsp);
                            task.Prepare();
                            task.Begin();
                        }

                        splExit(); splCryptoExit(); esExit(); nsextExit(); ncmExit();
                        remove(tempPath.c_str());

                        FILE* dbg3 = fopen("sdmc:/switch/TheOtherSide/debug.txt","a");
                        if (dbg3) { fprintf(dbg3,"install SUCCESS: %s\n",a->n.c_str()); fclose(dbg3); }
                        g_installInProgress.store(false);
                        std::lock_guard<std::mutex> lock(g_pendingIconsMutex);
                        g_pendingNotifications.push_back("Installed: " + a->n);

                    } catch (std::exception& e) {
                        std::string err = e.what();
                        remove(tempPath.c_str());
                        splExit(); splCryptoExit(); esExit(); nsextExit(); ncmExit();
                        FILE* dbg4 = fopen("sdmc:/switch/TheOtherSide/debug.txt","a");
                        if (dbg4) { fprintf(dbg4,"install FAILED: %s\n",err.c_str()); fclose(dbg4); }
                        g_installInProgress.store(false);
                        std::lock_guard<std::mutex> lock(g_pendingIconsMutex);
                        g_pendingNotifications.push_back("Install failed: " + err.substr(0, 60));
                    }
                    delete a;
                    return 0;
                }, args);
                if (createResult == thrd_success) {
                    thrd_detach(t);
                    brls::Application::notify("Starting download...");
                } else {
                    FILE* dbgFail = fopen("sdmc:/switch/TheOtherSide/debug.txt", "a");
                    if (dbgFail) { fprintf(dbgFail, "install thread creation FAILED, code=%d\n", createResult); fclose(dbgFail); }
                    brls::Application::notify("Install failed to start (thread error)");
                    delete args;
                }
            });
        });
        dlg->addButton("Cancel", [dlg](brls::View*) { dlg->close([](){}); });
        dlg->setCancelable(true);
        dlg->open();
    });
    return item;
}


static const size_t kShopPageSize = 60;


static std::string showKeyboard(const std::string& guide, const std::string& initial, int maxLen);

// shop stuff
void frame_showShop(const std::string& category, const std::string& typeFilter) {
    g_shopScreenGeneration.fetch_add(1);
    g_visibleShopItems.clear();

    brls::AppletFrame* frame = new brls::AppletFrame(true, true);
    frame->setTitle(category);
    frame->setIcon(BOREALIS_ASSET("icon/games.jpg"));
    brls::List* list = new brls::List();

    bool titlesEmpty;
    {
        std::lock_guard<std::mutex> lock(g_titleDataMutex);
        titlesEmpty = g_titleNames.empty();
    }
    if (!g_fetchDone) {
        list->addView(new brls::ListItem(g_fetchStatus, "Please wait..."));
    } else if (titlesEmpty) {
        list->addView(new brls::ListItem("No titles found", "Check " LOCATIONS_PATH));
    } else {

        auto searchTerm = std::make_shared<std::string>("");
        auto currentFilter = std::make_shared<std::string>(typeFilter);
        brls::ListItem* searchBtn = new brls::ListItem("Search", "Press A to search");

        auto buildTitleList = std::make_shared<std::function<void()>>();
        *buildTitleList = [list, searchBtn, searchTerm, currentFilter, buildTitleList]() {
            while (list->getViewsCount() > 1)
                list->removeView(1, true);

            std::string filter = *searchTerm;
            for (auto& c : filter) c = tolower(c);


            std::vector<size_t> matches;
            {
                std::lock_guard<std::mutex> lock(g_titleDataMutex);
                for (size_t i = 0; i < g_titleNames.size(); i++) {
                    const std::string& tid = g_titleIds[i];

                    std::string suffix = tid.size() >= 3 ? tid.substr(tid.size() - 3) : "";
                    for (auto& c : suffix) c = toupper(c);

                    bool typeMatch = true;
                    if (*currentFilter == "games")
                        typeMatch = (suffix == "000");
                    else if (*currentFilter == "updates")
                        typeMatch = (suffix != "000");

                    if (!typeMatch) continue;

                    if (filter.empty()) {
                        matches.push_back(i);
                    } else {
                        std::string nameLower = g_titleNames[i];
                        for (auto& c : nameLower) c = tolower(c);
                        if (nameLower.find(filter) != std::string::npos ||
                            tid.find(*searchTerm) != std::string::npos)
                            matches.push_back(i);
                    }
                }
            }

            if (filter.empty())
                searchBtn->setValue(std::to_string(matches.size()) + " titles");
            else
                searchBtn->setValue("\"" + *searchTerm + "\" — " + std::to_string(matches.size()) + " results");

            auto loadedCount = std::make_shared<size_t>(0);
            brls::ListItem* loadMoreBtn = new brls::ListItem("Load more...", "");
            std::function<void()> loadNextPage;

            loadNextPage = [list, loadedCount, loadMoreBtn, loadNextPage, matches]() mutable {
                size_t start = *loadedCount;
                size_t end = std::min(start + kShopPageSize, matches.size());
                bool hadButton = list->getViewsCount() > 1;

                int firstNewItem = (int)list->getViewsCount() - (hadButton ? 1 : 0);
                if (hadButton)
                    list->removeView((int)list->getViewsCount() - 1, false);
                uint64_t pageTickStart = armGetSystemTick();
                uint64_t buildTicks = 0, addViewTicks = 0;
                g_hasIconTicksAccum.store(0, std::memory_order_relaxed);
                g_statTicksAccum.store(0, std::memory_order_relaxed);
                g_readTicksAccum.store(0, std::memory_order_relaxed);
                for (size_t i = start; i < end; i++) {
                    uint64_t t0 = armGetSystemTick();
                    std::string tName, tId, tUrl, tIconUrl;
                    {
                        std::lock_guard<std::mutex> lock(g_titleDataMutex);
                        tName = g_titleNames[matches[i]];
                        tId   = g_titleIds[matches[i]];
                        tUrl  = g_titleUrls[matches[i]];
                        tIconUrl = matches[i] < g_titleIconUrls.size() ? g_titleIconUrls[matches[i]] : std::string();
                    }
                    brls::ListItem* built = buildShopTitleItem(tName, tId, tUrl, tIconUrl);
                    uint64_t t1 = armGetSystemTick();
                    list->addView(built);
                    uint64_t t2 = armGetSystemTick();
                    buildTicks += (t1 - t0);
                    addViewTicks += (t2 - t1);
                }
                uint64_t pageTickEnd = armGetSystemTick();
                {
                    double totalMs = armTicksToNs(pageTickEnd - pageTickStart) / 1000000.0;
                    double buildMs = armTicksToNs(buildTicks) / 1000000.0;
                    double addViewMs = armTicksToNs(addViewTicks) / 1000000.0;
                    double hasIconMs = armTicksToNs(g_hasIconTicksAccum.load(std::memory_order_relaxed)) / 1000000.0;
                    double statMs = armTicksToNs(g_statTicksAccum.load(std::memory_order_relaxed)) / 1000000.0;
                    double readMs = armTicksToNs(g_readTicksAccum.load(std::memory_order_relaxed)) / 1000000.0;
                    FILE* dbg = fopen("sdmc:/switch/TheOtherSide/debug.txt", "a");
                    if (dbg) {
                        fprintf(dbg, "page load: %zu titles, total=%.1fms buildShopTitleItem=%.1fms addView=%.1fms HasIcon=%.1fms stat=%.1fms fileRead=%.1fms\n",
                                end - start, totalMs, buildMs, addViewMs, hasIconMs, statMs, readMs);
                        fclose(dbg);
                    }
                }
                *loadedCount = end;
                if (end < matches.size()) {
                    size_t remaining = matches.size() - end;
                    loadMoreBtn->setLabel("Load " + std::to_string(std::min(kShopPageSize, remaining)) + " more...");
                    loadMoreBtn->setValue(std::to_string(remaining) + " remaining");
                } else {
                    loadMoreBtn->setLabel("All " + std::to_string(matches.size()) + " titles loaded");
                    loadMoreBtn->setValue("");
                }
                if (hadButton) list->addView(loadMoreBtn);

                int totalItems = (int)list->getViewsCount();
                if (totalItems > 1 && start > 0) {
                    float targetScroll = (float)firstNewItem / (float)totalItems;
                    list->setScrollY(targetScroll);

                }
            };

            list->addView(loadMoreBtn);
            loadMoreBtn->getClickEvent()->subscribe([loadNextPage](brls::View*) mutable { loadNextPage(); });
            loadNextPage();
        };

        list->addView(searchBtn);
        (*buildTitleList)();


        frame->registerAction("Jump Up", brls::Key::L, [list]() {

            brls::View* focus = brls::Application::currentFocus;
            int count = list->getViewsCount();
            for (int i = 0; i < count; i++) {
                if (list->getChild(i) == focus) {
                    int target = std::max(0, i - 10);
                    brls::Application::giveFocus(list->getChild(target));
                    break;
                }
            }
            return true;
        });
        frame->registerAction("Jump Down", brls::Key::R, [list]() {
            brls::View* focus = brls::Application::currentFocus;
            int count = list->getViewsCount();
            for (int i = 0; i < count; i++) {
                if (list->getChild(i) == focus) {
                    int target = std::min(count - 1, i + 10);
                    brls::Application::giveFocus(list->getChild(target));
                    break;
                }
            }
            return true;
        });

        searchBtn->getClickEvent()->subscribe([searchTerm, buildTitleList](brls::View*) mutable {
            std::string query = showKeyboard("Search games...", *searchTerm, 256);
            *searchTerm = query;
            (*buildTitleList)();
        });
    }
    frame->setContentView(list);
    brls::Application::pushView(frame);
}


static std::string showKeyboard(const std::string& guide, const std::string& initial, int maxLen = 256) {
    SwkbdConfig kbd;
    swkbdCreate(&kbd, 0);
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetGuideText(&kbd, guide.c_str());
    swkbdConfigSetInitialText(&kbd, initial.c_str());
    swkbdConfigSetStringLenMax(&kbd, maxLen);
    char buf[512] = {};
    Result rc = swkbdShow(&kbd, buf, sizeof(buf));
    swkbdClose(&kbd);
    if (R_SUCCEEDED(rc)) return std::string(buf);
    return initial;
}


static void showAddShopDialog(std::function<void()> onDone = nullptr) {
    Location loc;
    loc.protocol = "HTTPS";
    loc.url      = "";
    loc.path     = "";


    std::string proto = showKeyboard("Step 1 of 7 - Protocol: type http or https", "https");
    for (auto& c : proto) c = toupper(c);
    if (proto == "HTTP" || proto == "HTTPS")
        loc.protocol = proto;


    loc.url = sanitizeHostInput(showKeyboard("Step 2 of 7 - Host only, e.g: opennx.github.io", ""));
    if (loc.url.empty()) {
        brls::Application::notify("Cancelled - no host entered");
        return;
    }


    std::string rawPort = showKeyboard("Step 3 of 7 - Port (leave blank for standard 80/443), e.g: 82", "");
    std::string cleanPort;
    for (char c : rawPort) if (c >= '0' && c <= '9') cleanPort += c;
    if (!cleanPort.empty()) {
        int portNum = std::stoi(cleanPort);
        loc.port = (portNum > 0 && portNum <= 65535) ? cleanPort : "";
    }


    loc.path = showKeyboard("Step 4 of 7 - Path (leave blank for none), e.g: tinfoil.json", "");


    loc.username = showKeyboard("Step 5 of 7 - Username (leave blank if none)", "");


    if (!loc.username.empty())
        loc.password = showKeyboard("Step 5 of 7 - Password", "");


    std::string title = showKeyboard("Step 6 of 7 - Title (display name only)", loc.url);


    std::string fmt = showKeyboard("Step 7 of 7 - Format: type TINFOIL or CYBERFOIL (leave blank for TINFOIL)", "");
    for (auto& c : fmt) c = toupper(c);
    loc.format = (fmt == "CYBERFOIL") ? ShopFormat::CyberFoil : ShopFormat::TinfoilLegacy;


    std::string summary = "Protocol: " + loc.protocol + "\n"
        + "Host: " + loc.url + "\n"
        + "Port: " + (loc.port.empty() ? "(default)" : loc.port) + "\n"
        + "Path: " + (loc.path.empty() ? "(none)" : loc.path) + "\n"
        + "Username: " + (loc.username.empty() ? "(none)" : loc.username) + "\n"
        + "Format: " + (loc.format == ShopFormat::CyberFoil ? "CyberFoil" : "Tinfoil/Legacy") + "\n"
        + "Title: " + title;

    brls::Dialog* confirmDlg = new brls::Dialog("Add this shop?\n" + summary);
    confirmDlg->addButton("Save", [confirmDlg, loc, title, onDone](brls::View*) {
        confirmDlg->close([loc, title, onDone]() {
            auto locs = loadLocations();
            locs.push_back(loc);
            saveLocations(locs);

            { std::lock_guard<std::mutex> lock(g_titleDataMutex);
              g_titleNames.clear(); g_titleIds.clear(); g_titleUrls.clear(); g_titleIconUrls.clear(); }
            g_fetchDone = false; g_fetchCancel = false;
            thrd_t ft;
            thrd_create(&ft, [](void*)->int{ BgThreadGuard bgGuard; doFetch(); return 0; }, nullptr);
            thrd_detach(ft);

            brls::Application::notify("Shop added! Fetching " + title + "...");
            if (onDone) onDone();
        });
    });
    confirmDlg->addButton("Cancel, don't save", [confirmDlg](brls::View*) {
        confirmDlg->close([](){});
    });
    confirmDlg->setCancelable(true);
    confirmDlg->open();
}


// options
void populateOptionsList(brls::List* list) {
    list->addView(new brls::ListItem("User-Agent", TINCLONE_UA));


    brls::ListItem* addCustom = new brls::ListItem("+ Add custom shop URL", "Press A to enter Protocol/Host/Path");
    addCustom->getClickEvent()->subscribe([](brls::View*) {
        showAddShopDialog();
    });
    list->addView(addCustom);


    brls::ListItem* buildIdx = new brls::ListItem("Build icon index", "Downloads ~80MB once, enables icons for new titles");
    buildIdx->getClickEvent()->subscribe([](brls::View*) {


        bool expected = false;
        if (!g_iconIndexBuildInProgress.compare_exchange_strong(expected, true)) {
            brls::Application::notify("Icon index build already in progress...");
            return;
        }
        brls::Application::notify("Building icon index...");
        thrd_t t;
        thrd_create(&t, [](void*) -> int {
            BgThreadGuard bgGuard;
            bool ok = buildIconIndex();
            g_iconIndexBuildInProgress.store(false);
            std::lock_guard<std::mutex> lock(g_pendingIconsMutex);
            g_pendingNotifications.push_back(ok ? "Icon index built!" : "Icon index build failed");
            return 0;
        }, nullptr);
        thrd_detach(t);
    });
    list->addView(buildIdx);


    brls::ListItem* migrateIcons = new brls::ListItem("Migrate icon cache to subfolders",
        "One-time fix for existing icon_cache/ — spreads icons across subfolders");
    migrateIcons->getClickEvent()->subscribe([](brls::View*) {
        bool expected = false;
        if (!g_iconCacheMigrationInProgress.compare_exchange_strong(expected, true)) {
            brls::Application::notify("Icon cache migration already in progress...");
            return;
        }
        brls::Application::notify("Migrating icon cache...");
        thrd_t t;
        thrd_create(&t, [](void*) -> int {
            BgThreadGuard bgGuard;
            int moved = migrateIconCacheToShards([](int done, int total) {
                int pct = total > 0 ? (done * 100 / total) : 0;
                char msg[48]; snprintf(msg, sizeof(msg), "Migrating icons: %d%%", pct);
                std::lock_guard<std::mutex> lock(g_pendingIconsMutex);
                g_pendingNotifications.push_back(msg);
            });
            g_iconCacheMigrationInProgress.store(false);
            char msg[64]; snprintf(msg, sizeof(msg), "Migrated %d icons into subfolders!", moved);
            std::lock_guard<std::mutex> lock(g_pendingIconsMutex);
            g_pendingNotifications.push_back(msg);
            return 0;
        }, nullptr);
        thrd_detach(t);
    });
    list->addView(migrateIcons);


    brls::ListItem* mtpItem = new brls::ListItem("MTP Install Mode", inst::mtp::IsInstallServerRunning() ? "Running - connect via USB" : "Press A to start");
    mtpItem->getClickEvent()->subscribe([mtpItem](brls::View*) {
        if (inst::mtp::IsInstallServerRunning()) {
            inst::mtp::StopInstallServer();
            brls::Application::notify("MTP stopped");
        } else {
            thrd_t t;
            thrd_create(&t, [](void*) -> int {
                BgThreadGuard bgGuard;
                inst::mtp::StartInstallServer(0);
                return 0;
            }, nullptr);
            thrd_detach(t);
            brls::Application::notify("MTP started - connect Switch to PC via USB");
        }
    });
    list->addView(mtpItem);


    brls::ListItem* updateItem = new brls::ListItem("Check for Updates", "Current version: " + inst::config::appVersion);
    updateItem->getClickEvent()->subscribe([](brls::View*) {
        brls::Application::notify("Checking for updates...");
        thrd_t t;
        thrd_create(&t, [](void*) -> int {
            BgThreadGuard bgGuard;
            FILE* dbgStart = fopen("sdmc:/switch/TheOtherSide/debug.txt", "a");
            if (dbgStart) { fprintf(dbgStart, "update thread started\n"); fclose(dbgStart); }


            std::string response = httpGet(
                "https://raw.githubusercontent.com/eradicatinglove/The-Other-Side/main/version.txt");

            FILE* dbgStart2 = fopen("sdmc:/switch/TheOtherSide/debug.txt", "a");
            if (dbgStart2) { fprintf(dbgStart2, "update response: size=%zu data='%.50s'\n", response.size(), response.c_str()); fclose(dbgStart2); }

            if (response.empty()) {
                std::lock_guard<std::mutex> lock(g_pendingIconsMutex);
                g_pendingNotifications.push_back("Update check failed: no connection");
                return 0;
            }


            while (!response.empty() && (response.back()=='\n'||response.back()=='\r'||response.back()==' '))
                response.pop_back();

            std::string remoteVer = response;

            if (!remoteVer.empty() && remoteVer[0] == 'v') remoteVer = remoteVer.substr(1);
            std::string currentVer = inst::config::appVersion;
            std::string tag = "v" + remoteVer;

            FILE* dbg2 = fopen("sdmc:/switch/TheOtherSide/debug.txt", "a");
            if (dbg2) { fprintf(dbg2, "update: current=%s remote=%s\n", currentVer.c_str(), remoteVer.c_str()); fclose(dbg2); }

            if (remoteVer == currentVer) {
                std::lock_guard<std::mutex> lock(g_pendingIconsMutex);
                g_pendingNotifications.push_back("Already up to date (v" + currentVer + ")");
            } else {
                std::lock_guard<std::mutex> lock(g_pendingIconsMutex);
                g_pendingNotifications.push_back("UPDATE:" + tag + ":" + remoteVer);
            }
            return 0;
        }, nullptr);
        thrd_detach(t);
    });
    list->addView(updateItem);
    brls::ListItem* refresh = new brls::ListItem("Refresh Shop", "Re-fetch titles from all shops");
    refresh->getClickEvent()->subscribe([](brls::View*) {
        {
            std::lock_guard<std::mutex> lock(g_titleDataMutex);
            g_titleNames.clear(); g_titleIconUrls.clear();
            g_titleIds.clear();
            g_titleUrls.clear();
        }
        g_fetchDone   = false;
        g_fetchCancel = false;
        doFetch();
        brls::Application::notify(g_fetchStatus);
    });
    list->addView(refresh);

    list->addView(new brls::ListItem("── Configured Shops ──", LOCATIONS_PATH));
    auto locs = loadLocations();
    if (locs.empty()) {
        list->addView(new brls::ListItem("No shops configured", "Press '+ Add Shop' above"));
    } else {
        for (size_t i = 0; i < locs.size(); i++) {
            std::string label = "[" + locs[i].protocol + "] " + locs[i].url;
            brls::ListItem* item = new brls::ListItem(label, "A: remove");
            size_t idx = i;
            item->getClickEvent()->subscribe([idx](brls::View*) {
                auto ls = loadLocations();
                if (idx < ls.size()) {
                    std::string url = ls[idx].url;
                    brls::Dialog* dlg = new brls::Dialog("Remove shop?\n\n" + url);
                    dlg->addButton("Remove", [dlg, idx](brls::View*) {
                        dlg->close([idx](){
                            auto ls2 = loadLocations();
                            if (idx < ls2.size()) {
                                ls2.erase(ls2.begin() + idx);
                                saveLocations(ls2);


                                { std::lock_guard<std::mutex> lock(g_titleDataMutex);
                                  g_titleNames.clear(); g_titleIds.clear(); g_titleUrls.clear(); g_titleIconUrls.clear(); }
                                g_fetchDone = false; g_fetchCancel = false;
                                thrd_t ft;
                                thrd_create(&ft, [](void*)->int{ BgThreadGuard bgGuard; doFetch(); return 0; }, nullptr);
                                thrd_detach(ft);

                                brls::Application::notify("Shop removed");
                            }
                        });
                    });
                    dlg->addButton("Cancel", [dlg](brls::View*) { dlg->close([](){}); });
                    dlg->setCancelable(true);
                    dlg->open();
                }
            });
            list->addView(item);
        }
    }
}


void frame_showOptions() {
    brls::AppletFrame* frame = new brls::AppletFrame(true, true);
    frame->setTitle("Options");
    frame->setIcon(BOREALIS_ASSET("icon/options.jpg"));
    brls::List* list = new brls::List();
    populateOptionsList(list);
    frame->setContentView(list);
    brls::Application::pushView(frame);
}


// main app
int main(int argc, char* argv[])
{


    if (std::filesystem::exists("sdmc:/switch/TheOtherSide/TheOtherSide_update.nro")) {
        bool hasNextLoad = envHasNextLoad();
        Result rc = envSetNextLoad("sdmc:/switch/TheOtherSide/updater.nro",
                                    "sdmc:/switch/TheOtherSide/updater.nro");
        FILE* dbg = fopen("sdmc:/switch/TheOtherSide/debug.txt", "a");
        if (dbg) {
            fprintf(dbg, "cold-start update handoff: envHasNextLoad=%s envSetNextLoad=0x%x (%s)\n",
                    hasNextLoad ? "true" : "false", rc, R_SUCCEEDED(rc) ? "SUCCEEDED" : "FAILED");
            fclose(dbg);
        }
        return 0;
    }

    nsInitialize();
    nifmInitialize(NifmServiceType_User);
    spsmInitialize();
    curl_global_init(CURL_GLOBAL_ALL);


    inst::ui::mainApp = new inst::ui::MainApplication();

    brls::Logger::setLogLevel(brls::LogLevel::DEBUG);
    i18n::loadTranslations();
    if (!brls::Application::init("main/name"_i18n))
    {
        brls::Logger::error("Unable to init Borealis application");
        return EXIT_FAILURE;
    }

    brls::TabFrame* rootFrame = new brls::TabFrame();
    rootFrame->setTitle("main/name"_i18n);
    rootFrame->setIcon(BOREALIS_ASSET("icon/joycons.jpg"));


    brls::List* installedTab  = new brls::List();
    brls::Label* installedStatusLabel = nullptr;
    brls::List* fileBrowserTab = new brls::List();
    brls::List* shopTab       = new brls::List();
    brls::List* optionsTab    = new brls::List();


    {


        installedStatusLabel = new brls::Label(brls::LabelStyle::DESCRIPTION, "", false);
        installedStatusLabel->setHorizontalAlign(NVG_ALIGN_CENTER);

        std::string statusText;


        struct statvfs st;
        if (statvfs("sdmc:/", &st) == 0) {
            double freeGB  = (double)(st.f_bavail * st.f_frsize) / (1024.0*1024.0*1024.0);
            double totalGB = (double)(st.f_blocks * st.f_frsize) / (1024.0*1024.0*1024.0);
            char buf[64];
            snprintf(buf, sizeof(buf), "SD: %.1f / %.1f GB free", freeGB, totalGB);
            statusText += buf;
        } else {
            statusText += "SD: unavailable";
        }

        statusText += "   |   ";


        NifmInternetConnectionType connType;
        u32 wifiStrength;
        NifmInternetConnectionStatus connStatus;
        Result rc = nifmGetInternetConnectionStatus(&connType, &wifiStrength, &connStatus);
        if (R_SUCCEEDED(rc) && connStatus == NifmInternetConnectionStatus_Connected) {
            if (connType == NifmInternetConnectionType_WiFi)
                statusText += "Wi-Fi connected";
            else
                statusText += "Ethernet connected";
        } else {
            statusText += "No internet connection";
        }

        installedStatusLabel->setText(statusText);

        NsApplicationRecord* records = new NsApplicationRecord[4096]();
        s32 count = 0;
        nsListApplicationRecord(records, 4096, 0, &count);

        if (count == 0) {
            installedTab->addView(new brls::ListItem("No installed titles found"));
        } else {
            constexpr int COLS = 5;
            NsApplicationControlData* ctrl = new NsApplicationControlData();
            brls::BoxLayout* row = nullptr;
            int col = 0;

            for (s32 i = 0; i < count; i++) {
                char name[0x201] = {};
                char tid[32]     = {};
                snprintf(tid, sizeof(tid), "%016lX", records[i].application_id);
                u64 appId = records[i].application_id;

                unsigned char* iconBuf = nullptr;
                size_t iconSize = 0;
                size_t ctrlSize = 0;

                if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage,
                        records[i].application_id, ctrl, sizeof(*ctrl), &ctrlSize))) {
                    NacpLanguageEntry* lang = nullptr;
                    if (R_SUCCEEDED(nacpGetLanguageEntry(&ctrl->nacp, &lang)) && lang)
                        strncpy(name, lang->name, sizeof(name)-1);
                    if (ctrlSize > sizeof(ctrl->nacp)) {
                        iconSize = ctrlSize - sizeof(ctrl->nacp);
                        iconBuf  = new unsigned char[iconSize];
                        memcpy(iconBuf, ctrl->icon, iconSize);
                    }
                }
                if (!name[0]) strncpy(name, tid, sizeof(name)-1);

                if (col == 0) {
                    row = new brls::BoxLayout(brls::BoxLayoutOrientation::HORIZONTAL);
                    row->setHeight(148);
                    row->setSpacing(6);
                }


                brls::ListItem* cell = new brls::ListItem("");
                cell->setWidth(148);
                if (iconBuf)
                    cell->setThumbnail(iconBuf, iconSize);

                cell->getClickEvent()->subscribe([appId](brls::View*) {
                    accountInitialize(AccountServiceType_Application);
                    AccountUid uid = {};
                    accountGetPreselectedUser(&uid);
                    accountExit();
                    Result rc = appletRequestLaunchApplication(appId, nullptr);
                    if (R_FAILED(rc))
                        brls::Application::notify("Launch failed");
                });

                row->addView(cell, false);
                col++;
                if (col == COLS) {
                    installedTab->addView(row);
                    row = nullptr;
                    col = 0;
                }
            }
            if (row) installedTab->addView(row);
            delete ctrl;
        }
        delete[] records;
    }

    brls::ListItem* openFiles = new brls::ListItem("Browse sdmc:/");
    openFiles->getClickEvent()->subscribe([](brls::View*) { frame_showFileBrowser("sdmc:/"); });
    fileBrowserTab->addView(openFiles);

    brls::ListItem* openGames = new brls::ListItem("Games", "Base games");
    openGames->getClickEvent()->subscribe([](brls::View*) { frame_showShop("Games", "games"); });
    shopTab->addView(openGames);

    brls::ListItem* openUpdates = new brls::ListItem("Updates & DLC", "Game updates and DLC");
    openUpdates->getClickEvent()->subscribe([](brls::View*) { frame_showShop("Updates & DLC", "updates"); });
    shopTab->addView(openUpdates);


    populateOptionsList(optionsTab);

    inst::ui::PinnedStatusView* installedPinned = new inst::ui::PinnedStatusView(installedStatusLabel, installedTab);
    rootFrame->addTab("Installed",    installedPinned);
    rootFrame->addTab("File Browser", fileBrowserTab);
    rootFrame->addSeparator();
    rootFrame->addTab("New Games",    shopTab);
    rootFrame->addSeparator();
    rootFrame->addTab("Options",      optionsTab);

    brls::Application::pushView(rootFrame);


    g_fetchDone   = false;
    g_fetchCancel = false;
    loadIconCacheSet();
    ensureIconApplyTaskRunning();
    thrd_t fetchThrd;
    thrd_create(&fetchThrd, [](void*) -> int { BgThreadGuard bgGuard; doFetch(); return 0; }, nullptr);


    while (brls::Application::mainLoop()) {
        if (g_deferredQuitRequested.load(std::memory_order_acquire))
            brls::Application::quit();
    }


    g_fetchCancel = true;
    g_iconApplyTaskStopped = true;


    if (inst::mtp::IsInstallServerRunning())
        inst::mtp::StopInstallServer();


    thrd_join(fetchThrd, nullptr);


    while (g_activeBgThreads.load(std::memory_order_relaxed) > 0)
        svcSleepThread(50'000'000ULL);


    g_titleNames.clear(); g_titleNames.shrink_to_fit();
    g_titleIconUrls.clear(); g_titleIconUrls.shrink_to_fit();
    g_titleIds.clear();   g_titleIds.shrink_to_fit();
    g_titleUrls.clear();  g_titleUrls.shrink_to_fit();
    g_iconCacheSet.clear();
    g_iconIndex.clear();

    curl_global_cleanup();
    spsmExit();
    nsExit();


    return EXIT_SUCCESS;
}
