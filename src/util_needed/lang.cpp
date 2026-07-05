// Minimal language stub - returns the key as-is
// CyberFoil's install engine uses "key"_lang for strings
// We don't need localisation, just return the key
#include "util/lang.hpp"
#include <unordered_map>

namespace Language {
    // Minimal human-readable fallbacks for keys the install engine uses
    static std::unordered_map<std::string, std::string> s_table = {
        {"inst.info_page.downloading",      "Downloading: "},
        {"inst.info_page.at",               " at "},
        {"inst.info_page.top_info0",        "Installing: "},
        {"inst.net.retry.title",            "Download failed"},
        {"inst.net.retry.desc",             "Retry download?"},
        {"inst.net.retry.yes",              "Retry"},
        {"inst.net.retry.no",               "Cancel"},
        {"inst.net.transfer_interput",      "Transfer interrupted"},
        {"inst.nca_verify.title",           "NCA verification failed"},
        {"inst.nca_verify.desc",            "NCA signature invalid. Install anyway?"},
        {"common.cancel",                   "Cancel"},
        {"inst.nca_verify.opt1",            "Install anyway"},
    };

    void Load() {}

    std::string LanguageEntry(std::string key) {
        auto it = s_table.find(key);
        return it != s_table.end() ? it->second : key;
    }

    std::string GetRandomMsg() { return ""; }
    std::string GetRemoteHeaderLanguage() { return "en-US"; }
}
