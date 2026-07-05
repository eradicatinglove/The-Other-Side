#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>
#include <string>

namespace inst::util {
    void SecureWipe(void* ptr, std::size_t len);
    void initApp ();
    void deinitApp ();
    void initInstallServices();
    void deinitInstallServices();
    bool ignoreCaseCompare(const std::string &a, const std::string &b);
    std::vector<std::filesystem::path> getDirectoryFiles(const std::string & dir, const std::vector<std::string> & extensions);
    std::vector<std::filesystem::path> getDirsAtPath(const std::string & dir);
    bool removeDirectory(std::string dir);
    bool copyFile(std::string inFile, std::string outFile);
    std::string formatUrlString(std::string ourString);
    std::string formatUrlLink(std::string ourString);
    std::string shortenString(std::string ourString, int ourLength, bool isFile);
    std::string readTextFromFile(std::string ourFile);
    std::string softwareKeyboard(std::string guideText, std::string initialText, int LenMax);
    std::string getDriveFileName(std::string fileId);
    std::vector<uint32_t> setClockSpeed(int deviceToClock, uint32_t clockSpeed);
    std::string getIPAddress();
    bool usbIsConnected();
    void primeNavigationClickAudio();
    void playAudio(std::string audioPath);
    void playNavigationClick();
    void playNavigationClickIfNeeded(std::uint64_t buttonsDown);
    std::vector<std::string> checkForAppUpdate();
}
