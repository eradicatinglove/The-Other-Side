#include <filesystem>
#include <vector>
#include <algorithm>
#include <fstream>
#include <unistd.h>
#include <curl/curl.h>
#include <regex>
#include <mutex>
#include <arpa/inet.h>
#include <unistd.h>
#include "switch.h"
#include "util/util.hpp"
#include "nx/ipc/tin_ipc.h"
#include "util/config.hpp"
#include "util/curl.hpp"
#include "ui/MainApplication.hpp"
#include "util/usb_comms_awoo.h"
#include "util/json.hpp"
#include "nx/usbhdd.h"

namespace inst::util {
    namespace {
        std::mutex gAudioPlaybackMutex;
        Mix_Chunk* gNavigationClickChunk = nullptr;
        std::string gNavigationClickLoadedPath;
        bool gNavigationClickAudioOpen = false;

        bool ensureNavigationClickAudioReadyLocked(const std::string& audioPath) {
            int audio_rate = 22050;
            Uint16 audio_format = AUDIO_S16SYS;
            int audio_channels = 2;
            int audio_buffers = 1024;

            if (!gNavigationClickAudioOpen) {
                if (Mix_OpenAudio(audio_rate, audio_format, audio_channels, audio_buffers) != 0)
                    return false;
                gNavigationClickAudioOpen = true;
            }

            if (gNavigationClickChunk == nullptr || gNavigationClickLoadedPath != audioPath) {
                if (gNavigationClickChunk != nullptr) {
                    Mix_FreeChunk(gNavigationClickChunk);
                    gNavigationClickChunk = nullptr;
                }
                gNavigationClickChunk = Mix_LoadWAV(audioPath.c_str());
                if (gNavigationClickChunk == nullptr)
                    return false;
                gNavigationClickLoadedPath = audioPath;
            }
            return true;
        }

        std::string resolveNavigationClickPath() {
            std::string audioPath = "romfs:/audio/click.wav";
            const std::string customClickPath = inst::config::appDir + "/click.wav";
            if (std::filesystem::exists(customClickPath))
                audioPath = customClickPath;
            return audioPath;
        }

        std::string NormalizeReleaseNotes(std::string text) {
            if (text.empty())
                return "No changelog available for this release.";

            std::string out;
            out.reserve(text.size());
            bool lastWasNewline = false;
            int consecutiveNewlines = 0;
            for (char c : text) {
                if (c == '\r')
                    continue;
                if (c == '\n') {
                    if (!lastWasNewline) {
                        out.push_back('\n');
                        consecutiveNewlines = 1;
                    } else if (consecutiveNewlines < 2) {
                        out.push_back('\n');
                        consecutiveNewlines++;
                    }
                    lastWasNewline = true;
                    continue;
                }
                out.push_back(c);
                lastWasNewline = false;
                consecutiveNewlines = 0;
            }

            while (!out.empty() && (out.back() == '\n' || out.back() == ' ' || out.back() == '\t'))
                out.pop_back();
            if (out.empty())
                return "No changelog available for this release.";

            static constexpr std::size_t kMaxReleaseNotesLen = 3500;
            if (out.size() > kMaxReleaseNotesLen) {
                out = out.substr(0, kMaxReleaseNotesLen);
                out += "\n\n[Changelog truncated]";
            }
            return out;
        }
    }

    void SecureWipe(void* ptr, std::size_t len)
    {
        if (ptr == nullptr || len == 0)
            return;
        volatile unsigned char* p = reinterpret_cast<volatile unsigned char*>(ptr);
        for (std::size_t i = 0; i < len; i++)
            p[i] = 0;
    }

    void initApp () {
        if (!std::filesystem::exists("sdmc:/switch")) std::filesystem::create_directory("sdmc:/switch");
        if (!std::filesystem::exists(inst::config::appDir)) std::filesystem::create_directory(inst::config::appDir);
        inst::config::parseConfig();
        primeNavigationClickAudio();

        socketInitializeDefault();
        #ifdef __DEBUG__
            nxlinkStdio();
        #endif
        awoo_usbCommsInitialize();
        nx::hdd::init();
    }

    void deinitApp () {
        nx::hdd::exit();
        socketExit();
        awoo_usbCommsExit();
    }

    void initInstallServices() {
        ncmInitialize();
        nsextInitialize();
        esInitialize();
        splCryptoInitialize();
        splInitialize();
    }

    void deinitInstallServices() {
        ncmExit();
        nsextExit();
        esExit();
        splCryptoExit();
        splExit();
    }

    bool ignoreCaseCompare(const std::string &a, const std::string &b) {
        const auto case_insensitive_less = [](char x, char y) {
            return toupper(static_cast<unsigned char>(x)) < toupper(static_cast<unsigned char>(y));
        };

        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), case_insensitive_less);
    }

    std::vector<std::filesystem::path> getDirectoryFiles(const std::string & dir, const std::vector<std::string> & extensions) {
        std::vector<std::filesystem::path> files;
        for(auto & p: std::filesystem::directory_iterator(dir))
        {
            if (std::filesystem::is_regular_file(p))
            {
                std::string ourExtension = p.path().extension().string();
                std::transform(ourExtension.begin(), ourExtension.end(), ourExtension.begin(), ::tolower);
                if (extensions.empty() || std::find(extensions.begin(), extensions.end(), ourExtension) != extensions.end())
                {
                    files.push_back(p.path());
                }
            }
        }
        std::sort(files.begin(), files.end(), ignoreCaseCompare);
        return files;
    }

    std::vector<std::filesystem::path> getDirsAtPath(const std::string & dir) {
        std::vector<std::filesystem::path> files;
        for(auto & p: std::filesystem::directory_iterator(dir))
        {
            if (std::filesystem::is_directory(p))
            {
                    files.push_back(p.path());
            }
        }
        std::sort(files.begin(), files.end(), ignoreCaseCompare);
        return files;
    }

    bool removeDirectory(std::string dir) {
        try {
            for(auto & p: std::filesystem::recursive_directory_iterator(dir))
            {
                if (std::filesystem::is_regular_file(p))
                {
                    std::filesystem::remove(p);
                }
            }
            rmdir(dir.c_str());
            return true;
        }
        catch (std::filesystem::filesystem_error & e) {
            return false;
        }
    }

    bool copyFile(std::string inFile, std::string outFile) {
       char ch;
       std::ifstream f1(inFile);
       std::ofstream f2(outFile);

       if(!f1 || !f2) return false;
       
       while(f1 && f1.get(ch)) f2.put(ch);
       return true;
    }

    std::string formatUrlString(std::string ourString) {
        std::stringstream ourStream(ourString);
        std::string segment;
        std::vector<std::string> seglist;

        while(std::getline(ourStream, segment, '/')) {
            seglist.push_back(segment);
        }

        CURL *curl = curl_easy_init();
        int outlength;
        std::string finalString = curl_easy_unescape(curl, seglist[seglist.size() - 1].c_str(), seglist[seglist.size() - 1].length(), &outlength);
        curl_easy_cleanup(curl);

        return finalString;
    }

    std::string formatUrlLink(std::string ourString){
        std::string::size_type pos = ourString.find('/');
        if (pos != std::string::npos)
            return ourString.substr(0, pos);
        else
            return ourString;
    }

    std::string shortenString(std::string ourString, int ourLength, bool isFile) {
        std::filesystem::path ourStringAsAPath = ourString;
        std::string ourExtension = ourStringAsAPath.extension().string();
        if (ourString.size() - ourExtension.size() > (unsigned long)ourLength) {
            if(isFile) return (std::string)ourString.substr(0,ourLength) + "(...)" + ourExtension;
            else return (std::string)ourString.substr(0,ourLength) + "...";
        } else return ourString;
    }

    std::string readTextFromFile(std::string ourFile) {
        if (std::filesystem::exists(ourFile)) {
            FILE * file = fopen(ourFile.c_str(), "r");
            char line[1024];
            fgets(line, 1024, file);
            std::string url = line;
            fflush(file);
            fclose(file);
            return url;
        }
        return "";
    }

    std::string softwareKeyboard(std::string guideText, std::string initialText, int LenMax) {
        Result rc=0;
        SwkbdConfig kbd;
        char tmpoutstr[LenMax + 1] = {0};
        rc = swkbdCreate(&kbd, 0);
        if (R_SUCCEEDED(rc)) {
            swkbdConfigMakePresetDefault(&kbd);
            swkbdConfigSetGuideText(&kbd, guideText.c_str());
            swkbdConfigSetInitialText(&kbd, initialText.c_str());
            swkbdConfigSetStringLenMax(&kbd, LenMax);
            rc = swkbdShow(&kbd, tmpoutstr, sizeof(tmpoutstr));
            swkbdClose(&kbd);
            if (inst::ui::mainApp != nullptr)
                inst::ui::mainApp->RefreshInputDevice(true);
            if (R_SUCCEEDED(rc) && tmpoutstr[0] != 0) return(((std::string)(tmpoutstr)));
        }
        return "";
    }

    std::string getDriveFileName(std::string fileId) {
        std::string htmlData = inst::curl::downloadToBuffer("https://drive.google.com/file/d/" + fileId  + "/view");
        if (htmlData.size() > 0) {
            std::smatch ourMatches;
            std::regex ourRegex("<title>\\s*(.+?)\\s*</title>");
            std::regex_search(htmlData, ourMatches, ourRegex);
            if (ourMatches.size() > 1) {
                if (ourMatches[1].str() == "Google Drive -- Page Not Found") return "";
                return ourMatches[1].str().substr(0, ourMatches[1].str().size() - 15);
             }
        }
        return "";
    }

    std::vector<uint32_t> setClockSpeed(int deviceToClock, uint32_t clockSpeed) {
        uint32_t hz = 0;
        uint32_t previousHz = 0;

        if (deviceToClock > 2 || deviceToClock < 0) return {0,0};

        if(hosversionAtLeast(8,0,0)) {
            ClkrstSession session = {0};
            PcvModuleId pcvModuleId;
            pcvInitialize();
            clkrstInitialize();

            switch (deviceToClock) {
                case 0:
                    pcvGetModuleId(&pcvModuleId, PcvModule_CpuBus);
                    break;
                case 1:
                    pcvGetModuleId(&pcvModuleId, PcvModule_GPU);
                    break;
                case 2:
                    pcvGetModuleId(&pcvModuleId, PcvModule_EMC);
                    break;
            }

            clkrstOpenSession(&session, pcvModuleId, 3);
            clkrstGetClockRate(&session, &previousHz);
            clkrstSetClockRate(&session, clockSpeed);
            clkrstGetClockRate(&session, &hz);

            pcvExit();
            clkrstCloseSession(&session);
            clkrstExit();

            return {previousHz, hz};
        } else {
            PcvModule pcvModule;
            pcvInitialize();

            switch (deviceToClock) {
                case 0:
                    pcvModule = PcvModule_CpuBus;
                    break;
                case 1:
                    pcvModule = PcvModule_GPU;
                    break;
                case 2:
                    pcvModule = PcvModule_EMC;
                    break;
            }

            pcvGetClockRate(pcvModule, &previousHz);
            pcvSetClockRate(pcvModule, clockSpeed);
            pcvGetClockRate(pcvModule, &hz);
            
            pcvExit();

            return {previousHz, hz};
        }
    }

    std::string getIPAddress() {
        struct in_addr addr = {(in_addr_t) gethostid()};
        return inet_ntoa(addr);
    }
    
    bool usbIsConnected() {
        UsbState state = UsbState_Detached;
        usbDsGetState(&state);
        return state == UsbState_Configured;
    }

    void primeNavigationClickAudio() {
        std::lock_guard<std::mutex> lock(gAudioPlaybackMutex);
        const std::string audioPath = resolveNavigationClickPath();
        ensureNavigationClickAudioReadyLocked(audioPath);
    }

    void playAudio(std::string audioPath) {
        if (audioPath.empty())
            return;
        std::lock_guard<std::mutex> lock(gAudioPlaybackMutex);
        int audio_rate = 22050;
        Uint16 audio_format = AUDIO_S16SYS;
        int audio_channels = 2;
        int audio_buffers = 4096;

        if(Mix_OpenAudio(audio_rate, audio_format, audio_channels, audio_buffers) != 0) return;

        Mix_Chunk *sound = NULL;
        sound = Mix_LoadWAV(audioPath.c_str());
        if(sound == NULL) {
            Mix_FreeChunk(sound);
            Mix_CloseAudio();
            return;
        }

        int channel = Mix_PlayChannel(-1, sound, 0);
        if(channel == -1) {
            Mix_FreeChunk(sound);
            Mix_CloseAudio();
            return;
        }

        while(Mix_Playing(channel) != 0);

        Mix_FreeChunk(sound);
        Mix_CloseAudio();

        return;
    }

    void playNavigationClick() {
        if (!inst::config::soundEnabled)
            return;

        static u64 lastPlayTick = 0;
        const u64 now = armGetSystemTick();
        const u64 tickFreq = armGetSystemTickFreq();
        if (lastPlayTick != 0 && (now - lastPlayTick) < (tickFreq / 20))
            return;
        lastPlayTick = now;

        const std::string audioPath = resolveNavigationClickPath();

        std::lock_guard<std::mutex> lock(gAudioPlaybackMutex);
        if (!ensureNavigationClickAudioReadyLocked(audioPath))
            return;

        Mix_PlayChannel(-1, gNavigationClickChunk, 0);
    }

    void playNavigationClickIfNeeded(std::uint64_t buttonsDown) {
        constexpr std::uint64_t kNavButtons =
            HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right |
            HidNpadButton_StickLUp | HidNpadButton_StickLDown | HidNpadButton_StickLLeft | HidNpadButton_StickLRight |
            HidNpadButton_StickRUp | HidNpadButton_StickRDown | HidNpadButton_StickRLeft | HidNpadButton_StickRRight;

        if ((buttonsDown & kNavButtons) != 0)
            playNavigationClick();
    }
    
   std::vector<std::string> checkForAppUpdate () {
        try {
            std::string jsonData = inst::curl::downloadToBuffer("https://api.github.com/repos/luketanti/CyberFoil/releases/latest", 0, 0, 1000L);
            if (jsonData.size() == 0) return {};
            nlohmann::json ourJson = nlohmann::json::parse(jsonData);
            if (ourJson["tag_name"].get<std::string>() != inst::config::appVersion) {
                std::string downloadUrl;
                if (ourJson.contains("assets") && ourJson["assets"].is_array() && !ourJson["assets"].empty()
                    && ourJson["assets"][0].contains("browser_download_url") && ourJson["assets"][0]["browser_download_url"].is_string()) {
                    downloadUrl = ourJson["assets"][0]["browser_download_url"].get<std::string>();
                } else if (ourJson.contains("zipball_url") && ourJson["zipball_url"].is_string()) {
                    downloadUrl = ourJson["zipball_url"].get<std::string>();
                }

                std::string releaseNotes = "No changelog available for this release.";
                if (ourJson.contains("body") && ourJson["body"].is_string())
                    releaseNotes = NormalizeReleaseNotes(ourJson["body"].get<std::string>());

                std::vector<std::string> ourUpdateInfo = {
                    ourJson["tag_name"].get<std::string>(),
                    downloadUrl,
                    releaseNotes
                };
                inst::config::updateInfo = ourUpdateInfo;
                return ourUpdateInfo;
            }
        } catch (...) {}
        return {};
    }
}
