#include "ui/instPage.hpp"
#include "util/error.hpp"
#include <atomic>
#include <string>
#include <cstdio>
#include <switch.h>

namespace inst::ui::instPage {
    std::string g_installInfoText;
    std::string g_progressDetailText;
    double g_installBarPerc = 0.0;
    std::atomic<bool> g_cancelRequested{false};
    static int g_lastQueuedPerc = -1;
    static u64 g_lastNotifyTick = 0;
    // big files can take several seconds per percent - without a floor here
    // there'd be dead silence between ticks even though it's still moving
    static constexpr u64 kHeartbeatIntervalSeconds = 3;

    void setInstInfoText(std::string text)              { g_installInfoText = text; }
    void setTopInstInfoText(std::string text)           { g_installInfoText = text; }

    void setInstBarPerc(double percent) {
        g_installBarPerc = percent;
        int p = (int)percent;
        DBG_LOG("setInstBarPerc: %d%%\n", p);
        if (p == 0) return;
        // went backwards = new phase started, reset
        if (p < g_lastQueuedPerc) g_lastQueuedPerc = -1;

        const u64 now = armGetSystemTick();
        const u64 freq = armGetSystemTickFreq();
        const bool percentChanged = (p != g_lastQueuedPerc);
        const bool heartbeatDue = g_lastNotifyTick == 0 ||
            (now - g_lastNotifyTick) >= freq * kHeartbeatIntervalSeconds;

        // every percent step, or every few seconds if it hasn't moved
        if (percentChanged || heartbeatDue) {
            g_lastQueuedPerc = p;
            g_lastNotifyTick = now;
            std::string msg = g_progressDetailText.empty()
                ? std::to_string(p) + "%"
                : g_progressDetailText;
            std::lock_guard<std::mutex> lock(g_pendingIconsMutex);
            g_pendingNotifications.push_back(msg);
        }
    }

    void setProgressDetailText(const std::string& text) { g_progressDetailText = text; }
    void clearProgressDetailText()                      { g_progressDetailText.clear(); g_lastQueuedPerc = -1; g_lastNotifyTick = 0; }
    bool isInstallCancelRequested()                     { return g_cancelRequested.load(); }
    void requestInstallCancel()                         { g_cancelRequested.store(true); }
    void clearInstallCancel()                           { g_cancelRequested.store(false); g_lastQueuedPerc = -1; g_lastNotifyTick = 0; }
}
