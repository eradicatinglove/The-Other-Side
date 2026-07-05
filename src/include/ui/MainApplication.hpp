#pragma once
#include <string>
#include <vector>
#include <switch.h>

namespace inst::ui {
    struct MainApplication {
        int CreateShowDialog(const std::string& title, const std::string& desc,
                             const std::vector<std::string>& opts, bool cancel);
        void RefreshInputDevice(bool force = false) { (void)force; }
        void CallForRender() {}
        void UpdateButtons() {}
        u64 GetButtonsDown() { return 0; }
    };
    extern MainApplication* mainApp;
}
