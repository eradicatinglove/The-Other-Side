// inst::util stubs for MTP install services
#include <switch.h>

extern "C" {
#include "nx/ipc/ns_ext.h"
#include "nx/ipc/es.h"
}

namespace inst::util {

void initInstallServices() {
    ncmInitialize();
    nsextInitialize();
    esInitialize();
    splCryptoInitialize();
    splInitialize();
}

void deinitInstallServices() {
    splExit();
    splCryptoExit();
    esExit();
    nsextExit();
    ncmExit();
}

} // namespace inst::util
