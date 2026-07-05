#pragma once

#include <cstddef>
#include <string>

namespace inst::diag {
    struct InstallFailure {
        bool canceled = false;
        std::string category;
        std::string code;
        std::string summary;
        std::string recommendation;
        std::string rawMessage;
    };

    bool IsVerboseEnabled();
    const std::string& GetInstallLogPath();

    void StartSession(const std::string& source, std::size_t totalItems);
    void NoteTransferReceived(const std::string& item);
    void NoteInstallStarted(const std::string& item);
    void NoteStep(const std::string& step, bool verboseOnly = true);
    void RecordSuccess(const std::string& item);

    InstallFailure ClassifyFailure(const std::string& errorText);
    std::string BuildUserMessage(const InstallFailure& failure);
    void RecordFailure(const std::string& item, const InstallFailure& failure);
}
