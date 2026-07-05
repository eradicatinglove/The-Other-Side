#pragma once
#include <string>
#include <atomic>
#include <vector>
#include <mutex>

// Thread-safe progress notification queue — background install thread pushes
// messages here; IconApplyTask drains them on the UI thread where
// brls::Application::notify() is safe to call.
extern std::vector<std::string> g_pendingNotifications;
extern std::mutex g_pendingIconsMutex;

namespace inst::ui::instPage {
    void setInstInfoText(std::string text);
    void setInstBarPerc(double percent);
    void setProgressDetailText(const std::string& text);
    void clearProgressDetailText();
    void setTopInstInfoText(std::string text);
    bool isInstallCancelRequested();
    void requestInstallCancel();
    void clearInstallCancel();

    extern std::string g_installInfoText;
    extern std::string g_progressDetailText;
    extern double g_installBarPerc;
    extern std::atomic<bool> g_cancelRequested;
}
