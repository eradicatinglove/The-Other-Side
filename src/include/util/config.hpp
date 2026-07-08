#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace inst::config {
    static const std::string appDir = "sdmc:/switch/TheOtherSide";
    static const std::string configPath = appDir + "/config.json";
    static const std::string remotesDir = appDir + "/remotes";
    static const std::string legacyShopsDir = appDir + "/shops";
    static const std::string remoteIconsDir = appDir + "/remote_icons";
    static const std::string legacyShopIconsDir = appDir + "/shop_icons";
    static const std::string appVersion = std::string(APP_VERSION_STR);
    static const std::string appGitMeta = std::string();
    static const std::string appVersionFull = std::string(APP_VERSION_STR);

    extern std::string gAuthKey;
    extern std::string lastNetUrl;
    extern std::string offlineDbManifestUrl;
    extern std::string remoteUrl;
    extern std::string remoteUser;
    extern std::string remotePass;
    extern std::string httpUserAgentMode;
    extern std::string httpUserAgent;
    extern std::vector<std::string> updateInfo;
    extern int languageSetting;
    extern bool ignoreReqVers;
    extern bool validateNCAs;
    extern bool overClock;
    extern bool deletePrompt;
    extern bool autoUpdate;
    extern bool gayMode;
    extern bool soundEnabled;
    extern bool oledMode;
    extern bool mtpExposeAlbum;
    extern bool usbAck;
    extern bool remoteHideInstalled;
    extern bool remoteHideInstalledSection;
    extern bool remoteAllBaseOnly;
    extern bool remoteLegacyMode;
    extern bool remoteStartGridMode;
    extern bool offlineDbAutoCheckOnStartup;
    extern bool verboseInstallLogging;

    struct RemoteProfile {
        std::string fileName;
        std::string protocol;
        std::string host;
        std::string path;
        int port = 8465;
        std::string username;
        std::string password;
        std::string title;
        bool favourite = false;
        std::int64_t updatedAt = 0;
    };

    int DefaultPortForProtocol(const std::string& protocol);
    std::string NormalizeHttpUserAgentMode(const std::string& mode);
    std::string NormalizeRemotePath(const std::string& path);
    bool ParseRemoteUrl(const std::string& rawUrl, std::string& protocol, std::string& host, int& port, std::string& path);
    std::string BuildRemoteUrl(const RemoteProfile& remote);
    std::vector<RemoteProfile> LoadRemotes();
    bool SaveRemote(const RemoteProfile& remote, std::string* error = nullptr);
    bool DeleteRemote(const std::string& fileName);
    bool SetActiveRemote(const RemoteProfile& remote, bool writeConfig = true);

    void setConfig();
    void parseConfig();
}

