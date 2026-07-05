#include "util/install_diagnostics.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>

#include "util/config.hpp"
#include "util/error.hpp"

namespace inst::diag {
    namespace {
        std::mutex g_logMutex;
        std::string g_logPath;

        std::string ToLower(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }

        std::string TimestampNow()
        {
            const std::time_t t = std::time(nullptr);
            std::tm tmInfo{};
#if defined(_WIN32)
            localtime_s(&tmInfo, &t);
#else
            localtime_r(&t, &tmInfo);
#endif
            char buffer[32] = {};
            std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tmInfo);
            return std::string(buffer);
        }

        bool ContainsAny(const std::string& haystackLower, const std::initializer_list<const char*>& needles)
        {
            for (const char* needle : needles) {
                if (haystackLower.find(needle) != std::string::npos)
                    return true;
            }
            return false;
        }

        void AppendLine(const std::string& level, const std::string& message)
        {
            std::lock_guard<std::mutex> lock(g_logMutex);

            const std::filesystem::path logsDir = std::filesystem::path(inst::config::appDir) / "logs";
            std::error_code ec;
            std::filesystem::create_directories(logsDir, ec);

            if (g_logPath.empty())
                g_logPath = (logsDir / "install.log").string();

            std::ofstream out(g_logPath, std::ios::out | std::ios::app);
            if (!out)
                return;

            out << "[" << TimestampNow() << "] [" << level << "] " << message << "\n";
        }
    }

    bool IsVerboseEnabled()
    {
        return inst::config::verboseInstallLogging;
    }

    const std::string& GetInstallLogPath()
    {
        if (g_logPath.empty()) {
            const std::filesystem::path logsDir = std::filesystem::path(inst::config::appDir) / "logs";
            g_logPath = (logsDir / "install.log").string();
        }
        return g_logPath;
    }

    void StartSession(const std::string& source, std::size_t totalItems)
    {
        std::ostringstream line;
        line << "Session start source=" << source << " items=" << totalItems << " verbose=" << (IsVerboseEnabled() ? "on" : "off");
        AppendLine("INFO", line.str());
    }

    void NoteTransferReceived(const std::string& item)
    {
        AppendLine("INFO", "Transfer received: " + item);
    }

    void NoteInstallStarted(const std::string& item)
    {
        AppendLine("INFO", "Install started: " + item);
    }

    void NoteStep(const std::string& step, bool verboseOnly)
    {
        if (verboseOnly && !IsVerboseEnabled())
            return;
        AppendLine("DEBUG", step);
    }

    void RecordSuccess(const std::string& item)
    {
        AppendLine("INFO", "Install succeeded: " + item);
    }

    InstallFailure ClassifyFailure(const std::string& errorText)
    {
        InstallFailure failure{};
        failure.rawMessage = errorText;
        const std::string lower = ToLower(errorText);

        std::smatch m;
        const std::regex rcRegex("(0x[0-9a-fA-F]{8})");
        if (std::regex_search(errorText, m, rcRegex) && m.size() > 1)
            failure.code = m[1].str();

        if (ContainsAny(lower, {"installation canceled", "cancelled", "canceled"})) {
            failure.canceled = true;
            failure.category = "Canceled";
            failure.summary = "Installation canceled by user.";
            return failure;
        }

        if (ContainsAny(lower, {"signature", "master key", "key mismatch", "failed to import ticket", "ticket / cert", "tik", "cert", "rights id"})) {
            failure.category = "Signature/Keys issue";
            failure.summary = "[ERROR] Missing required keys or invalid signatures.";
            failure.recommendation = "Check signature patches, keyset, and ticket/cert validity.";
            return failure;
        }

        if (ContainsAny(lower, {"invalid nca magic", "bad dump", "unreadable", "truncated", "corrupt", "decompress", "hash", "cnmt"})) {
            failure.category = "Corrupt or incomplete file";
            failure.summary = "[ERROR] Package appears corrupt or incomplete.";
            failure.recommendation = "Re-download/re-dump the NSP/XCI and verify integrity.";
            return failure;
        }

        if (ContainsAny(lower, {
                "can't parse nca",
                "cant parse nca",
                "outdated firmware",
                "hos update required",
                "required system firmware",
                "too low to decrypt",
                "openfilesystemwithid",
                "failed to open file system with id"
            }) || lower.find("0x001fd602") != std::string::npos) {
            failure.category = "Unsupported format / Firmware mismatch";
            failure.summary = "[ERROR] Can't parse NCA. Outdated firmware. HOS update required.";
            failure.recommendation = "Update firmware/patches or use compatible content.";
            return failure;
        }

        if (ContainsAny(lower, {"failed to register", "failed to set content records", "commit content records", "failed to read file", "failed to write", "storage", "sd", "i/o", "no space", "filesystem"})) {
            failure.category = "Storage write failure";
            failure.summary = "[ERROR] Failed to write content to target storage.";
            failure.recommendation = "Check free space, filesystem health, and write permissions.";
            return failure;
        }

        if (ContainsAny(lower, {"firmware", "unsupported"})) {
            failure.category = "Unsupported format / Firmware mismatch";
            failure.summary = "[ERROR] Content requires unsupported firmware or format.";
            failure.recommendation = "Update firmware/patches or use compatible content.";
            return failure;
        }

        failure.category = "Unclassified";
        failure.summary = "[ERROR] Installation failed due to an unknown error.";
        failure.recommendation = "Review install log and raw error details.";
        return failure;
    }

    std::string BuildUserMessage(const InstallFailure& failure)
    {
        if (failure.canceled)
            return failure.summary;

        std::string text = failure.summary + "\nCategory: " + failure.category;
        if (!failure.code.empty())
            text += "\nCode: " + failure.code;
        if (!failure.recommendation.empty())
            text += "\nHint: " + failure.recommendation;
        if (IsVerboseEnabled() && !failure.rawMessage.empty())
            text += "\n\nRaw: " + failure.rawMessage;
        text += "\n\nLog: " + GetInstallLogPath();
        return text;
    }

    void RecordFailure(const std::string& item, const InstallFailure& failure)
    {
        std::ostringstream line;
        line << "Install failed: " << item << " category=" << failure.category;
        if (!failure.code.empty())
            line << " code=" << failure.code;
        if (!failure.rawMessage.empty())
            line << " raw=\"" << failure.rawMessage << "\"";
        AppendLine("ERROR", line.str());
    }
}
