#include "ui/MainApplication.hpp"
#include <borealis.hpp>

namespace inst::ui {
    MainApplication* mainApp = nullptr;

    // stub - install engine calls this for user confirmations
    // We always proceed (return 0 = first option)
    int MainApplication::CreateShowDialog(const std::string& title,
                                          const std::string& desc,
                                          const std::vector<std::string>& opts,
                                          bool cancel) {
        (void)title; (void)desc; (void)opts; (void)cancel;
        return 0;
    }
}
