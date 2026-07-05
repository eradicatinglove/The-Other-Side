#include "ui/instPage.hpp"
#include <atomic>
#include <string>
#include <cstdio>

namespace inst::ui::instPage {
    std::string g_installInfoText;
    std::string g_progressDetailText;
    double g_installBarPerc = 0.0;
    std::atomic<bool> g_cancelRequested{false};
    static int g_lastQueuedPerc = -1;

    void setInstInfoText(std::string text)              { g_installInfoText = text; }
    void setTopInstInfoText(std::string text)           { g_installInfoText = text; }

    void setInstBarPerc(double percent) {
        g_installBarPerc = percent;
        int p = (int)percent;
        FILE* dbg = fopen("sdmc:/switch/TheOtherSide/debug.txt", "a");
        if (dbg) { fprintf(dbg, "setInstBarPerc: %d%%\n", p); fclose(dbg); }
        if (p == 0) return;
        if (p == 100 || p >= g_lastQueuedPerc + 10) {
            g_lastQueuedPerc = p;
            std::string msg = g_progressDetailText.empty()
                ? std::to_string(p) + "%"
                : g_progressDetailText;
            std::lock_guard<std::mutex> lock(g_pendingIconsMutex);
            g_pendingNotifications.push_back(msg);
        }
    }

    void setProgressDetailText(const std::string& text) { g_progressDetailText = text; }
    void clearProgressDetailText()                      { g_progressDetailText.clear(); g_lastQueuedPerc = -1; }
    bool isInstallCancelRequested()                     { return g_cancelRequested.load(); }
    void requestInstallCancel()                         { g_cancelRequested.store(true); }
    void clearInstallCancel()                           { g_cancelRequested.store(false); g_lastQueuedPerc = -1; }
}
