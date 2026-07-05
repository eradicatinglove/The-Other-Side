#include "util/util.hpp"
#include "util/config.hpp"
#include <cstring>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <switch.h>

namespace inst::util {

    // Called by http_nsp/http_xci to format display names from URLs
    std::string formatUrlString(std::string ourString) {
        if (ourString.empty()) return ourString;
        // Get filename portion after last /
        size_t pos = ourString.rfind('/');
        if (pos != std::string::npos)
            ourString = ourString.substr(pos + 1);
        // URL-decode %XX sequences
        std::string result;
        result.reserve(ourString.size());
        for (size_t i = 0; i < ourString.size(); i++) {
            if (ourString[i] == '%' && i + 2 < ourString.size()) {
                int val = 0;
                std::istringstream ss(ourString.substr(i + 1, 2));
                ss >> std::hex >> val;
                result += static_cast<char>(val);
                i += 2;
            } else if (ourString[i] == '+') {
                result += ' ';
            } else {
                result += ourString[i];
            }
        }
        return result;
    }

    // Strip filename from URL path
    std::string formatDirectoryString(std::string ourString) {
        size_t pos = ourString.rfind('/');
        return pos != std::string::npos ? ourString.substr(0, pos + 1) : ourString;
    }

    // No-op stubs for features we don't use
    void initApp()   {}
    void deinitApp() {}
    void playAudio(std::string) {}
    void playNavigationClick() {}

    bool isTitleInstalled(u64 tid, bool isChecking) {
        (void)tid; (void)isChecking;
        return false;
    }

    std::string getIPAddress() {
        u32 addr = 0;
        nifmGetCurrentIpAddress(&addr);
        char buf[32];
        snprintf(buf, sizeof(buf), "%d.%d.%d.%d",
            (addr >> 0) & 0xFF, (addr >> 8) & 0xFF,
            (addr >> 16) & 0xFF, (addr >> 24) & 0xFF);
        return std::string(buf);
    }

    std::string softwareKeyboard(std::string guideText, std::string initialText, int maxLength) {
        // Return initial text - keyboard not supported in this build
        return initialText;
    }

    bool getMenuOptionList(std::vector<std::string>& optionList, std::string fileName) {
        (void)optionList; (void)fileName;
        return false;
    }

    bool setMenuOptionList(std::vector<std::string>& optionList, std::string fileName) {
        (void)optionList; (void)fileName;
        return false;
    }
}
