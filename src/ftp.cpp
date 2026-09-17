// FTP server, runs on its own libnx thread.
//
// networking bits are lifted straight from the IconSwap FTP server (poll
// loop, PASV accept, sending 150 before accept, all of it). only new thing
// is the /install folder - STOR a .nsp/.nsz/.xci/.xcz there and it gets
// routed into the same stream installer MTP uses instead of hitting the SD
// card. always installs to SD, never NAND, same as MTP.
//
// everything outside /install is just normal sdmc:/ read/write, same two
// virtual storages as the MTP server (install vs sd card).

#include "ftp.h"
#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

#include <string>

#include "mtp_install.hpp" // shared stream installer, same one MTP uses
#include "ui/instPage.hpp"  // toast progress, same as HTTP installs

#define FTP_PORT        5000
#define FTP_BUF_SIZE    4096
#define FTP_MAX_CLIENTS 4
#define FTP_MAX_PATH    512
#define FTP_STACK_SIZE  (256 * 1024)
#define FTP_THREAD_PRIO 0x2C

// bigger buffer than the normal FTP one so multi-GB installs aren't stuck
// doing 4KB recv() chunks the whole way
#define FTP_INSTALL_BUF_SIZE (1024 * 1024)

// the ftp root just shows these two folders, same split as MTP
#define FTP_SD_DIR      "SD Card"
#define FTP_INSTALL_DIR "Install (NSP, XCI, NSZ, XCZ)"

typedef struct {
    int  ctrlFd;
    int  dataFd;
    int  passiveListenFd;
    char cwd[FTP_MAX_PATH];
    char recvBuf[FTP_BUF_SIZE];
    int  recvLen;
    char renamePending[FTP_MAX_PATH];
    bool loggedIn;
    bool binaryMode;
    u64  alloSize;   // optional size hint from ALLO, used for install progress
} FtpClient;

static Mutex         g_mutex;
static char          g_ipStr[32]     = "0.0.0.0";
static char          g_status[128]   = "FTP not started";
static int           g_filesReceived = 0;
static volatile bool g_running       = false;

static Thread    g_thread;
static int       g_listenFd = -1;
static FtpClient g_clients[FTP_MAX_CLIENTS];

static void setStatus(const char* msg) {
    mutexLock(&g_mutex);
    strncpy(g_status, msg, sizeof(g_status) - 1);
    g_status[sizeof(g_status) - 1] = '\0';
    mutexUnlock(&g_mutex);
}

static void utf8_strncpy(char* dst, const char* src, size_t maxBytes) {
    if (!maxBytes) return;
    size_t i = 0, last_safe = 0;
    while (src[i] && i < maxBytes - 1) {
        if ((src[i] & 0xC0) != 0x80) last_safe = i;
        i++;
    }
    if (src[i] && (src[i] & 0xC0) == 0x80) i = last_safe;
    memcpy(dst, src, i);
    dst[i] = '\0';
}

// nsp/nsz/xci/xcz
static bool ftpIsInstallable(const char* name) {
    const char* ext = strrchr(name, '.');
    if (!ext) return false;
    return strcasecmp(ext, ".nsp") == 0 || strcasecmp(ext, ".nsz") == 0 ||
           strcasecmp(ext, ".xci") == 0 || strcasecmp(ext, ".xcz") == 0;
}

// which virtual folder a path is under: root (just lists the two folders),
// sd card (passthrough), or install (routes into the stream installer)
enum FtpZone { ZONE_ROOT, ZONE_SD, ZONE_INSTALL, ZONE_BAD };

// p == base, or p starts with base + "/"
static bool ftpPathUnder(const char* p, const char* base) {
    size_t L = strlen(base);
    return strncmp(p, base, L) == 0 && (p[L] == '\0' || p[L] == '/');
}

static FtpZone ftpZoneOf(const char* ftpAbs) {
    if (ftpAbs[0] == '\0' || strcmp(ftpAbs, "/") == 0) return ZONE_ROOT;
    if (ftpPathUnder(ftpAbs, "/" FTP_SD_DIR))      return ZONE_SD;
    if (ftpPathUnder(ftpAbs, "/" FTP_INSTALL_DIR)) return ZONE_INSTALL;
    return ZONE_BAD;
}

// last path component
static const char* ftpBaseName(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// strips the "/SD Card" prefix and maps to a real sdmc:/ path. false if the
// path isn't under SD Card at all (root, install zone, whatever) so callers
// can just bail
static bool ftpMapToSdmc(const char* ftpAbs, char* out, size_t outSize) {
    if (ftpZoneOf(ftpAbs) != ZONE_SD) return false;
    const char* rel = ftpAbs + strlen("/" FTP_SD_DIR);   // "" or "/sub/dir"
    while (*rel == '/') rel++;
    if (*rel == '\0') snprintf(out, outSize, "sdmc:/");
    else              snprintf(out, outSize, "sdmc:/%.*s", (int)(outSize - 8), rel);
    return true;
}

static void ftpResolvePath(FtpClient* c, const char* arg, char* out, size_t outSize) {
    if (!arg || arg[0] == '\0') { snprintf(out, outSize, "/%s", c->cwd); return; }
    if (arg[0] == '/') { snprintf(out, outSize, "%s", arg); }
    else {
        if (c->cwd[0]) snprintf(out, outSize, "/%s/%s", c->cwd, arg);
        else           snprintf(out, outSize, "/%s", arg);
    }
}

static void ftpSend(int fd, const char* msg) {
    if (fd < 0) return;
    send(fd, msg, strlen(msg), 0);
}

static void ftpCloseClient(FtpClient* c) {
    if (c->dataFd >= 0)          { close(c->dataFd);          c->dataFd = -1; }
    if (c->passiveListenFd >= 0) { close(c->passiveListenFd); c->passiveListenFd = -1; }
    if (c->ctrlFd >= 0)          { close(c->ctrlFd);          c->ctrlFd = -1; }
    c->recvLen  = 0;
    c->loggedIn = false;
}

// select() with a 10s wait for the client to connect - blocking accept()
// with a socket timeout isn't reliable on libnx, this is
static int ftpOpenDataConn(FtpClient* c) {
    if (c->dataFd >= 0) return c->dataFd;
    if (c->passiveListenFd < 0) return -1;

    // select() is reliable on libnx; SO_RCVTIMEO on listen sockets is not
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(c->passiveListenFd, &fds);
    struct timeval tv = { 10, 0 };
    int ready = select(c->passiveListenFd + 1, &fds, NULL, NULL, &tv);

    int dataFd = -1;
    if (ready > 0) {
        struct sockaddr_in addr; socklen_t addrLen = sizeof(addr);
        dataFd = accept(c->passiveListenFd, (struct sockaddr*)&addr, &addrLen);
    }

    close(c->passiveListenFd); c->passiveListenFd = -1;

    if (dataFd >= 0) {
        // back to blocking, and no Nagle delay
        int flags = fcntl(dataFd, F_GETFL, 0);
        fcntl(dataFd, F_SETFL, flags & ~O_NONBLOCK);
        int nodelay = 1;
        setsockopt(dataFd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        struct timeval dtv = { 30, 0 };  // generous timeouts
        setsockopt(dataFd, SOL_SOCKET, SO_SNDTIMEO, &dtv, sizeof(dtv));
        setsockopt(dataFd, SOL_SOCKET, SO_RCVTIMEO, &dtv, sizeof(dtv));
    }

    c->dataFd = dataFd;
    return dataFd;
}

// always returns true (keeps the control connection open), sends its own
// ftp reply either way
static bool ftpStorInstall(FtpClient* c, const char* ftpAbs) {
    const char* base = ftpBaseName(ftpAbs);

    if (!ftpIsInstallable(base)) {
        ftpSend(c->ctrlFd, "550 Only NSP/NSZ/XCI/XCZ can be installed.\r\n");
        return true;
    }

    ftpSend(c->ctrlFd, "150 Opening data connection for install.\r\n");
    int dataFd = ftpOpenDataConn(c);
    if (dataFd < 0) { ftpSend(c->ctrlFd, "425 No data connection.\r\n"); return true; }

    // this is just for the progress bar - the nsp/xci installers parse
    // their own headers so an inaccurate total doesn't matter
    const u64 total = c->alloSize;
    c->alloSize = 0;

    // 0 = SD. never passing 1 (NAND) here on purpose.
    if (!inst::mtp::StartStreamInstall(base, total, 0)) {
        close(dataFd); c->dataFd = -1;
        ftpSend(c->ctrlFd, "550 Install could not start.\r\n");
        return true;
    }

    namespace ip = inst::ui::instPage;  // same toast progress as HTTP installs
    ip::clearInstallCancel();
    ip::clearProgressDetailText();
    ip::setInstInfoText(std::string("Installing: ") + base);

    char* buf = (char*)malloc(FTP_INSTALL_BUF_SIZE);
    if (!buf) {
        inst::mtp::CancelStreamInstall();
        close(dataFd); c->dataFd = -1;
        ip::setInstInfoText(std::string("Failed: ") + base);
        ftpSend(c->ctrlFd, "451 Out of memory.\r\n");
        return true;
    }

    bool ok = true, canceled = false;
    u64  offset = 0, lastReport = 0;
    int  n;
    while ((n = recv(dataFd, buf, FTP_INSTALL_BUF_SIZE, 0)) > 0) {
        if (ip::isInstallCancelRequested()) { canceled = true; break; }
        if (!inst::mtp::WriteStreamInstall(buf, (size_t)n, offset)) { ok = false; break; }
        offset += (u64)n;

        if (offset - lastReport >= (4u * 1024u * 1024u)) {  // ~every 4MB
            lastReport = offset;
            if (total > 0) {
                double pct = (double)offset * 100.0 / (double)total;
                if (pct > 99.0) pct = 99.0;   // save 100% for when finalize actually succeeds
                ip::clearProgressDetailText();
                ip::setInstBarPerc(pct);      // -> "NN%" toast
            } else {
                // no total, so just show MB received instead of a %
                char d[48];
                snprintf(d, sizeof(d), "%llu MB received",
                         (unsigned long long)(offset / (1024 * 1024)));
                ip::setProgressDetailText(d);
                ip::setInstBarPerc(50.0);     // nonzero so it keeps refreshing the detail text
            }
        }
    }
    if (n < 0) ok = false;   // recv error mid-transfer

    free(buf);
    close(dataFd); c->dataFd = -1;

    if (canceled) {
        inst::mtp::CancelStreamInstall();
        ip::clearProgressDetailText();
        ip::setInstInfoText(std::string("Canceled: ") + base);
        setStatus("Install canceled");
        ftpSend(c->ctrlFd, "426 Install canceled.\r\n");
        return true;
    }
    if (!ok) {
        inst::mtp::CancelStreamInstall();
        ip::clearProgressDetailText();
        ip::setInstInfoText(std::string("Failed: ") + base);
        setStatus("Install failed");
        ftpSend(c->ctrlFd, "550 Install failed.\r\n");
        return true;
    }

    inst::mtp::CloseStreamInstall();
    const bool complete = inst::mtp::ConsumeStreamInstallComplete();
    ip::clearProgressDetailText();
    ip::setInstBarPerc(100.0);

    char st[128];
    if (complete) {
        mutexLock(&g_mutex); g_filesReceived++; mutexUnlock(&g_mutex);
        ip::setInstInfoText(std::string("Done: ") + base);
        snprintf(st, sizeof(st), "Installed: %.80s", base);
        setStatus(st);
        ftpSend(c->ctrlFd, "226 Install complete.\r\n");
    } else {
        ip::setInstInfoText(std::string("Finished (verify): ") + base);
        snprintf(st, sizeof(st), "Install finished (verify): %.80s", base);
        setStatus(st);
        // transfer worked fine, just finalize came back not-complete
        ftpSend(c->ctrlFd, "226 Transfer complete (verify install on device).\r\n");
    }
    return true;
}

static bool ftpHandleCmd(FtpClient* c, char* line) {
    char cmd[16] = {0};
    char arg[FTP_MAX_PATH] = {0};

    int llen = strlen(line);
    while (llen > 0 && (line[llen-1] == '\r' || line[llen-1] == '\n')) line[--llen] = '\0';

    char* sp = strchr(line, ' ');
    if (sp) {
        int clen = (int)(sp - line);
        if (clen >= (int)sizeof(cmd)) clen = (int)sizeof(cmd) - 1;
        memcpy(cmd, line, clen);
        const char* a = sp + 1; while (*a == ' ') a++;
        utf8_strncpy(arg, a, sizeof(arg));
    } else {
        if (llen >= (int)sizeof(cmd)) llen = (int)sizeof(cmd) - 1;
        memcpy(cmd, line, llen);
    }
    for (int i = 0; cmd[i]; i++) if (cmd[i] >= 'a' && cmd[i] <= 'z') cmd[i] -= 32;

    if (strcmp(cmd, "USER") == 0) { ftpSend(c->ctrlFd, "331 Password required.\r\n"); return true; }
    if (strcmp(cmd, "PASS") == 0) { c->loggedIn = true; ftpSend(c->ctrlFd, "230 Logged in.\r\n"); return true; }
    if (strcmp(cmd, "QUIT") == 0) { ftpSend(c->ctrlFd, "221 Goodbye.\r\n"); return false; }
    if (strcmp(cmd, "SYST") == 0) { ftpSend(c->ctrlFd, "215 UNIX Type: L8\r\n"); return true; }
    if (strcmp(cmd, "FEAT") == 0) { ftpSend(c->ctrlFd, "211-Features:\r\n UTF8\r\n SIZE\r\n211 End\r\n"); return true; }
    if (strcmp(cmd, "OPTS") == 0) { ftpSend(c->ctrlFd, "200 OK.\r\n"); return true; }
    if (strcmp(cmd, "TYPE") == 0) { c->binaryMode = (arg[0]=='I'||arg[0]=='i'); ftpSend(c->ctrlFd, "200 Type set.\r\n"); return true; }
    if (strcmp(cmd, "MODE") == 0 || strcmp(cmd, "STRU") == 0) { ftpSend(c->ctrlFd, "200 OK.\r\n"); return true; }
    if (strcmp(cmd, "NOOP") == 0) { ftpSend(c->ctrlFd, "200 NOOP OK.\r\n"); return true; }
    if (!c->loggedIn) { ftpSend(c->ctrlFd, "530 Not logged in.\r\n"); return true; }

    if (strcmp(cmd, "ALLO") == 0) {  // optional upload size hint, only used for the progress bar
        c->alloSize = (arg[0]) ? strtoull(arg, NULL, 10) : 0;
        ftpSend(c->ctrlFd, "200 OK.\r\n"); return true;
    }

    if (strcmp(cmd, "PWD") == 0) {
        char resp[FTP_MAX_PATH + 16];
        snprintf(resp, sizeof(resp), "257 \"/%s\" is current directory.\r\n", c->cwd);
        ftpSend(c->ctrlFd, resp); return true;
    }
    if (strcmp(cmd, "CWD") == 0) {
        char ftpAbs[FTP_MAX_PATH], sdmcPath[FTP_MAX_PATH];
        ftpResolvePath(c, arg, ftpAbs, sizeof(ftpAbs));
        FtpZone z = ftpZoneOf(ftpAbs);
        // root and install are virtual, no need to stat anything
        if (z == ZONE_ROOT || z == ZONE_INSTALL) {
            const char* rel = ftpAbs; while (*rel == '/') rel++;
            strncpy(c->cwd, rel, sizeof(c->cwd) - 1); c->cwd[sizeof(c->cwd)-1] = '\0';
            ftpSend(c->ctrlFd, "250 Directory changed.\r\n");
            return true;
        }
        // "SD Card" itself always exists, anything deeper has to be real
        if (z == ZONE_SD && ftpMapToSdmc(ftpAbs, sdmcPath, sizeof(sdmcPath))) {
            struct stat st;
            if (strcmp(ftpAbs, "/" FTP_SD_DIR) == 0 ||
                (stat(sdmcPath, &st) == 0 && S_ISDIR(st.st_mode))) {
                const char* rel = ftpAbs; while (*rel == '/') rel++;
                strncpy(c->cwd, rel, sizeof(c->cwd) - 1); c->cwd[sizeof(c->cwd)-1] = '\0';
                ftpSend(c->ctrlFd, "250 Directory changed.\r\n");
                return true;
            }
        }
        ftpSend(c->ctrlFd, "550 Directory not found.\r\n");
        return true;
    }
    if (strcmp(cmd, "CDUP") == 0) {
        int len = strlen(c->cwd);
        while (len > 0 && c->cwd[len-1] != '/') len--;
        if (len > 0) len--;
        c->cwd[len] = '\0';
        ftpSend(c->ctrlFd, "250 Directory changed.\r\n"); return true;
    }
    if (strcmp(cmd, "PASV") == 0) {
        if (c->passiveListenFd >= 0) { close(c->passiveListenFd); c->passiveListenFd = -1; }
        int listenFd = socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd < 0) { ftpSend(c->ctrlFd, "425 Cannot create data socket.\r\n"); return true; }
        int opt = 1; setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(listenFd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));  // low latency on the data socket too
        struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET; addr.sin_addr.s_addr = inet_addr(g_ipStr); addr.sin_port = 0;
        if (bind(listenFd, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(listenFd, 1) < 0) {
            close(listenFd); ftpSend(c->ctrlFd, "425 Cannot bind data socket.\r\n"); return true;
        }
        socklen_t addrLen = sizeof(addr);
        getsockname(listenFd, (struct sockaddr*)&addr, &addrLen);
        c->passiveListenFd = listenFd;
        unsigned long ip = inet_addr(g_ipStr); unsigned char* ip4 = (unsigned char*)&ip;
        unsigned short port = ntohs(addr.sin_port);
        char resp[64];
        snprintf(resp, sizeof(resp), "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u).\r\n",
            ip4[0],ip4[1],ip4[2],ip4[3],(port>>8)&0xFF,port&0xFF);
        ftpSend(c->ctrlFd, resp);
        return true;
    }
    if (strcmp(cmd, "LIST") == 0 || strcmp(cmd, "NLST") == 0) {
        ftpSend(c->ctrlFd, "150 Opening data connection.\r\n");  // send 150 BEFORE accepting
        svcSleepThread(20000000LL); // 20ms flush
        int dataFd = ftpOpenDataConn(c);
        if (dataFd < 0) { ftpSend(c->ctrlFd, "425 No data connection.\r\n"); return true; }

        char ftpCwdAbs[FTP_MAX_PATH];
        snprintf(ftpCwdAbs, sizeof(ftpCwdAbs), "/%s", c->cwd);
        FtpZone z = ftpZoneOf(ftpCwdAbs);
        const bool nlst = (strcmp(cmd, "NLST") == 0);

        if (z == ZONE_ROOT) {
            const char* names[2] = { FTP_SD_DIR, FTP_INSTALL_DIR };  // just the two virtual folders
            for (int i = 0; i < 2; i++) {
                char linebuf[512];
                if (nlst) snprintf(linebuf, sizeof(linebuf), "%s\r\n", names[i]);
                else      snprintf(linebuf, sizeof(linebuf),
                              "drwxr-xr-x 1 ftp ftp %8d Jan  1 00:00 %s\r\n", 0, names[i]);
                send(dataFd, linebuf, strlen(linebuf), 0);
            }
        } else if (z == ZONE_SD) {
            char sdmcPath[FTP_MAX_PATH];
            ftpMapToSdmc(ftpCwdAbs, sdmcPath, sizeof(sdmcPath));
            DIR* dir = opendir(sdmcPath);
            if (dir) {
                struct dirent* ent;
                while ((ent = readdir(dir)) != NULL) {
                    char fp[FTP_MAX_PATH + 256]; snprintf(fp, sizeof(fp), "%s/%s", sdmcPath, ent->d_name);
                    struct stat st; stat(fp, &st);
                    char linebuf[512];
                    if (nlst)
                        snprintf(linebuf, sizeof(linebuf), "%s\r\n", ent->d_name);
                    else
                        snprintf(linebuf, sizeof(linebuf), "%srwxr-xr-x 1 ftp ftp %8ld Jan  1 00:00 %s\r\n",
                            S_ISDIR(st.st_mode) ? "d" : "-", (long)st.st_size, ent->d_name);
                    send(dataFd, linebuf, strlen(linebuf), 0);
                }
                closedir(dir);
            }
        }
        // install folder is write-only, nothing to list there
        close(dataFd); c->dataFd = -1;
        ftpSend(c->ctrlFd, "226 Transfer complete.\r\n"); return true;
    }
    if (strcmp(cmd, "STOR") == 0) {
        char ftpAbs[FTP_MAX_PATH], sdmcPath[FTP_MAX_PATH];
        ftpResolvePath(c, arg, ftpAbs, sizeof(ftpAbs));

        FtpZone z = ftpZoneOf(ftpAbs);

        if (z == ZONE_INSTALL) return ftpStorInstall(c, ftpAbs);

        // anywhere else, normal file write to SD only
        if (z != ZONE_SD || !ftpMapToSdmc(ftpAbs, sdmcPath, sizeof(sdmcPath))) {
            ftpSend(c->ctrlFd, "550 Upload only into 'SD Card' or the Install folder.\r\n");
            return true;
        }
        char dirPart[FTP_MAX_PATH]; strncpy(dirPart, sdmcPath, sizeof(dirPart)-1); dirPart[sizeof(dirPart)-1]='\0';
        char* slash = strrchr(dirPart, '/'); if (slash) { *slash='\0'; mkdir(dirPart, 0777); }
        ftpSend(c->ctrlFd, "150 Opening data connection for upload.\r\n");  // send 150 BEFORE accepting
        int dataFd = ftpOpenDataConn(c);
        if (dataFd < 0) { ftpSend(c->ctrlFd, "425 No data connection.\r\n"); return true; }
        FILE* f = fopen(sdmcPath, "wb");
        if (!f) { close(dataFd); c->dataFd=-1; ftpSend(c->ctrlFd, "550 Cannot create file.\r\n"); return true; }
        char buf[FTP_BUF_SIZE]; int n;
        while ((n = recv(dataFd, buf, sizeof(buf), 0)) > 0) fwrite(buf, 1, n, f);
        fclose(f); close(dataFd); c->dataFd = -1;
        char st[128]; snprintf(st, sizeof(st), "Uploaded: %.80s", arg[0] ? arg : "file");
        setStatus(st);
        ftpSend(c->ctrlFd, "226 Transfer complete.\r\n"); return true;
    }
    if (strcmp(cmd, "RETR") == 0) {
        char ftpAbs[FTP_MAX_PATH], sdmcPath[FTP_MAX_PATH];
        ftpResolvePath(c, arg, ftpAbs, sizeof(ftpAbs));
        struct stat st;
        if (!ftpMapToSdmc(ftpAbs, sdmcPath, sizeof(sdmcPath)) || stat(sdmcPath, &st) != 0) {
            if (c->passiveListenFd >= 0) { close(c->passiveListenFd); c->passiveListenFd=-1; }
            ftpSend(c->ctrlFd, "550 File not found.\r\n"); return true;
        }
        char resp[64]; snprintf(resp, sizeof(resp), "150 Opening data connection (%ld bytes).\r\n", (long)st.st_size);
        ftpSend(c->ctrlFd, resp);  // send 150 BEFORE accepting data conn so client knows to connect
        svcSleepThread(20000000LL); // 20ms: ensure 150 is flushed to network before blocking on accept
        int dataFd = ftpOpenDataConn(c);
        if (dataFd < 0) { ftpSend(c->ctrlFd, "425 No data connection.\r\n"); return true; }
        FILE* f = fopen(sdmcPath, "rb");
        if (!f) { close(dataFd); c->dataFd=-1; ftpSend(c->ctrlFd, "550 Cannot open file.\r\n"); return true; }
        char buf[FTP_BUF_SIZE]; int n;
        while ((n = (int)fread(buf, 1, sizeof(buf), f)) > 0) send(dataFd, buf, n, 0);
        fclose(f); close(dataFd); c->dataFd = -1;
        ftpSend(c->ctrlFd, "226 Transfer complete.\r\n"); return true;
    }
    if (strcmp(cmd, "DELE") == 0) {
        char ftpAbs[FTP_MAX_PATH], sdmcPath[FTP_MAX_PATH];
        ftpResolvePath(c, arg, ftpAbs, sizeof(ftpAbs));
        if (!ftpMapToSdmc(ftpAbs, sdmcPath, sizeof(sdmcPath))) { ftpSend(c->ctrlFd, "550 Delete failed.\r\n"); return true; }
        ftpSend(c->ctrlFd, remove(sdmcPath)==0 ? "250 File deleted.\r\n" : "550 Delete failed.\r\n"); return true;
    }
    if (strcmp(cmd, "MKD") == 0) {
        char ftpAbs[FTP_MAX_PATH], sdmcPath[FTP_MAX_PATH];
        ftpResolvePath(c, arg, ftpAbs, sizeof(ftpAbs));
        if (!ftpMapToSdmc(ftpAbs, sdmcPath, sizeof(sdmcPath))) { ftpSend(c->ctrlFd, "550 MKD failed.\r\n"); return true; }
        if (mkdir(sdmcPath, 0777)==0) { char r[FTP_MAX_PATH+8]; snprintf(r,sizeof(r),"257 \"%s\" created.\r\n",ftpAbs); ftpSend(c->ctrlFd,r); }
        else ftpSend(c->ctrlFd, "550 MKD failed.\r\n"); return true;
    }
    if (strcmp(cmd, "RMD") == 0) {
        char ftpAbs[FTP_MAX_PATH], sdmcPath[FTP_MAX_PATH];
        ftpResolvePath(c, arg, ftpAbs, sizeof(ftpAbs));
        if (!ftpMapToSdmc(ftpAbs, sdmcPath, sizeof(sdmcPath))) { ftpSend(c->ctrlFd, "550 RMD failed.\r\n"); return true; }
        ftpSend(c->ctrlFd, rmdir(sdmcPath)==0 ? "250 Directory removed.\r\n" : "550 RMD failed.\r\n"); return true;
    }
    if (strcmp(cmd, "RNFR") == 0) {
        char ftpAbs[FTP_MAX_PATH];
        ftpResolvePath(c, arg, ftpAbs, sizeof(ftpAbs));
        if (!ftpMapToSdmc(ftpAbs, c->renamePending, sizeof(c->renamePending))) {
            c->renamePending[0] = '\0';
            ftpSend(c->ctrlFd, "550 Rename source invalid.\r\n"); return true;
        }
        ftpSend(c->ctrlFd, "350 Ready for RNTO.\r\n"); return true;
    }
    if (strcmp(cmd, "RNTO") == 0) {
        char ftpAbs[FTP_MAX_PATH], sdmcPath[FTP_MAX_PATH];
        ftpResolvePath(c, arg, ftpAbs, sizeof(ftpAbs));
        if (!ftpMapToSdmc(ftpAbs, sdmcPath, sizeof(sdmcPath))) { c->renamePending[0]='\0'; ftpSend(c->ctrlFd, "550 Rename failed.\r\n"); return true; }
        ftpSend(c->ctrlFd, rename(c->renamePending,sdmcPath)==0 ? "250 Rename OK.\r\n" : "550 Rename failed.\r\n");
        c->renamePending[0]='\0'; return true;
    }
    if (strcmp(cmd, "SIZE") == 0) {
        char ftpAbs[FTP_MAX_PATH], sdmcPath[FTP_MAX_PATH];
        ftpResolvePath(c, arg, ftpAbs, sizeof(ftpAbs));
        struct stat st;
        if (ftpMapToSdmc(ftpAbs, sdmcPath, sizeof(sdmcPath)) && stat(sdmcPath, &st)==0) { char r[32]; snprintf(r,sizeof(r),"213 %ld\r\n",(long)st.st_size); ftpSend(c->ctrlFd,r); }
        else ftpSend(c->ctrlFd, "550 File not found.\r\n"); return true;
    }
    ftpSend(c->ctrlFd, "502 Command not implemented.\r\n"); return true;
}

static void ftpPoll() {
    // accept a new client
    if (g_listenFd >= 0) {
        struct sockaddr_in addr; socklen_t addrLen = sizeof(addr);
        int newFd = accept(g_listenFd, (struct sockaddr*)&addr, &addrLen);
        if (newFd >= 0) {
            for (int i = 0; i < FTP_MAX_CLIENTS; i++) {
                if (g_clients[i].ctrlFd < 0) {
                    FtpClient* c = &g_clients[i];
                    c->ctrlFd=newFd; c->dataFd=-1; c->passiveListenFd=-1;
                    c->recvLen=0; c->loggedIn=false; c->binaryMode=true;
                    c->cwd[0]='\0'; c->renamePending[0]='\0'; c->alloSize=0;
                    fcntl(newFd, F_SETFL, O_NONBLOCK);
                    int nodelay = 1;
                    setsockopt(newFd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
                    ftpSend(newFd, "220 The Other Side FTP Server Ready (UTF-8).\r\n");
                    char st[64]; snprintf(st, sizeof(st), "Client: %s", inet_ntoa(addr.sin_addr));
                    setStatus(st);
                    goto acceptDone;
                }
            }
            close(newFd);
            acceptDone:;
        }
    }
    for (int i = 0; i < FTP_MAX_CLIENTS; i++) {  // service everyone already connected
        FtpClient* c = &g_clients[i];
        if (c->ctrlFd < 0) continue;
        char tmp[FTP_BUF_SIZE];
        int n = recv(c->ctrlFd, tmp, sizeof(tmp), 0);
        if (n == 0) { ftpCloseClient(c); continue; }
        if (n < 0)  { if (errno != EAGAIN && errno != EWOULDBLOCK) ftpCloseClient(c); continue; }
        int space = FTP_BUF_SIZE - c->recvLen - 1;
        if (n > space) n = space;
        memcpy(c->recvBuf + c->recvLen, tmp, n);
        c->recvLen += n; c->recvBuf[c->recvLen] = '\0';
        char* lineStart = c->recvBuf;
        char* nl;
        while ((nl = strchr(lineStart, '\n')) != NULL) {
            *nl = '\0';
            bool keep = ftpHandleCmd(c, lineStart);
            if (!keep) { ftpCloseClient(c); goto nextClient; }
            lineStart = nl + 1;
        }
        {
            int remaining = (int)(c->recvBuf + c->recvLen - lineStart);
            if (remaining > 0 && lineStart != c->recvBuf) memmove(c->recvBuf, lineStart, remaining);
            c->recvLen = remaining; c->recvBuf[c->recvLen] = '\0';
        }
        nextClient:;
    }
}

static void ftpThreadFunc(void* arg) {
    (void)arg;
    setStatus("Waiting for connections...");
    while (g_running) {
        ftpPoll();
        svcSleepThread(1000000LL); // 1ms yield so we don't pin the CPU
    }
}

static bool getLocalIp(char* out, size_t outSize) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    struct sockaddr_in remote; memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET; remote.sin_port = htons(80);
    remote.sin_addr.s_addr = inet_addr("8.8.8.8");
    fcntl(fd, F_SETFL, O_NONBLOCK);
    connect(fd, (struct sockaddr*)&remote, sizeof(remote));
    struct sockaddr_in local; socklen_t localLen = sizeof(local);
    bool ok = (getsockname(fd, (struct sockaddr*)&local, &localLen) == 0 && local.sin_addr.s_addr != 0);
    if (ok) snprintf(out, outSize, "%s", inet_ntoa(local.sin_addr));
    close(fd);
    return ok;
}

bool ftpStart(void) {
    if (g_running) return true;
    mutexInit(&g_mutex);

    if (!getLocalIp(g_ipStr, sizeof(g_ipStr))) {
        nifmInitialize(NifmServiceType_User);
        u32 ipAddr = 0; nifmGetCurrentIpAddress(&ipAddr); nifmExit();
        if (ipAddr == 0) { setStatus("No network. Check Wi-Fi."); return false; }
        struct in_addr ia; ia.s_addr = ipAddr;
        snprintf(g_ipStr, sizeof(g_ipStr), "%s", inet_ntoa(ia));
    }

    g_listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listenFd < 0) { setStatus("socket() failed."); return false; }
    int opt = 1; setsockopt(g_listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(FTP_PORT);
    if (bind(g_listenFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        char msg[64]; snprintf(msg, sizeof(msg), "bind() failed: %d", errno);
        setStatus(msg); close(g_listenFd); g_listenFd=-1; return false;
    }
    if (listen(g_listenFd, FTP_MAX_CLIENTS) < 0) {
        setStatus("listen() failed."); close(g_listenFd); g_listenFd=-1; return false;
    }
    fcntl(g_listenFd, F_SETFL, O_NONBLOCK); // so accept() in ftpPoll doesn't block

    for (int i = 0; i < FTP_MAX_CLIENTS; i++) {
        memset(&g_clients[i], 0, sizeof(FtpClient));
        g_clients[i].ctrlFd=-1; g_clients[i].dataFd=-1; g_clients[i].passiveListenFd=-1;
    }

    g_running = true; g_filesReceived = 0;

    Result rc = threadCreate(&g_thread, ftpThreadFunc, NULL, NULL, FTP_STACK_SIZE, FTP_THREAD_PRIO, -2);
    if (R_FAILED(rc)) {
        g_running = false; close(g_listenFd); g_listenFd=-1;
        setStatus("threadCreate failed."); return false;
    }
    threadStart(&g_thread);
    return true;
}

void ftpStop(void) {
    if (!g_running) return;
    g_running = false;
    // clean up if an install was still mid-flight
    if (inst::mtp::IsStreamInstallActive()) inst::mtp::CancelStreamInstall();
    if (g_listenFd >= 0) { close(g_listenFd); g_listenFd=-1; }
    for (int i = 0; i < FTP_MAX_CLIENTS; i++) ftpCloseClient(&g_clients[i]);
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
    setStatus("FTP stopped.");
}

bool        ftpIsRunning(void)     { return g_running; }
const char* ftpGetIp(void)         { return g_ipStr; }
const char* ftpGetStatus(void)     { return g_status; }
int         ftpGetFilesReceived(void) {
    mutexLock(&g_mutex); int n = g_filesReceived; mutexUnlock(&g_mutex); return n;
}
