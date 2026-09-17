#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// FTP server. normal paths just passthrough to sdmc:/, anything under
// /install/*.nsp|.nsz|.xci|.xcz goes into the shared stream installer (SD
// only, never touches NAND). socketInitializeDefault() already happened in
// userAppInit() so ftpStart() doesn't need to set up networking itself.

bool        ftpStart(void);            // start server + worker thread on port 5000
void        ftpStop(void);             // stop server, join thread, cancel any install
bool        ftpIsRunning(void);
const char* ftpGetIp(void);            // detected LAN IP (for display)
const char* ftpGetStatus(void);        // last status line (for display)
int         ftpGetFilesReceived(void); // count of completed installs this session

#ifdef __cplusplus
}
#endif
