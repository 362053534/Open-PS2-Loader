/*
  Copyright 2009-2010, jimmikaelkael
  Licenced under Academic Free License version 3.0
  Review Open PS2 Loader README & LICENSE files for further details.
*/

#include "smstcpip.h"
#include "internal.h"

#include "device.h"
#include "smb_tuning.h"

extern struct cdvdman_settings_smb cdvdman_settings;

extern struct irx_export_table _exp_oplsmb;

extern int smb_io_sema;

static void ps2ip_init(void);

// !!! ps2ip exports functions pointers !!!
// Note: recvfrom() used here is not a standard recvfrom() function.
int (*plwip_close)(int s);                                                                                                                 // #6
int (*plwip_connect)(int s, struct sockaddr *name, socklen_t namelen);                                                                     // #7
int (*plwip_recv)(int s, void *mem, int len, unsigned int flags);                                                                          // #9
int (*plwip_recvfrom)(int s, void *mem, int hlen, void *payload, int plen, unsigned int flags, struct sockaddr *from, socklen_t *fromlen); // #10
int (*plwip_send)(int s, void *dataptr, int size, unsigned int flags);                                                                     // #11
int (*plwip_socket)(int domain, int type, int protocol);                                                                                   // #13
int (*plwip_setsockopt)(int s, int level, int optname, const void *optval, socklen_t optlen);                                              // #19
int (*plwip_shutdown)(int s, int how);                                                                                                      // #46
u32 (*pinet_addr)(const char *cp);                                                                                                         // #24

#if SMB_DIAG_LOG
// 诊断镜像通道：每条 SMBDIAG 日志除走 stdout（udptty 广播）外，
// 再单播一份到 SMB 服务器 IP 的 UDP 18194 端口（部分网络环境收不到 255.255.255.255 广播）。
int (*plwip_sendto)(int s, void *dataptr, int size, unsigned int flags, struct sockaddr *to, socklen_t tolen); // #12
static int smbDiagSock = -1;

void smbDiagEmit(const char *msg)
{
    struct sockaddr_in peer;
    u32 dest;
    int len;

    printf("%s", msg);

    if (!plwip_socket || !plwip_sendto || !pinet_addr)
        return;

    dest = pinet_addr(cdvdman_settings.smb_ip);
    if (!dest || dest == 0xFFFFFFFFu)
        return;

    if (smbDiagSock < 0) {
        smbDiagSock = plwip_socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (smbDiagSock < 0)
            return;
    }

    peer.sin_family = AF_INET;
    peer.sin_port = htons(18194);
    peer.sin_addr.s_addr = dest;

    len = strlen(msg);
    plwip_sendto(smbDiagSock, (void *)msg, len, 0, (struct sockaddr *)&peer, sizeof(peer));
}
#endif

static u32 ServerCapabilities;
#if SMB_FEAT_RECONNECT_THREADS
static OplSmbPwHashFunc_t smbHashCallback;
static volatile int smbConnectionState = 2;
static volatile int smbReconnectEnabled;
static volatile int smbPhysicalLinkDown;
static volatile int smbTrayOpen;
static int (*pNetManGetGlobalNetIFLinkState)(void);

static void smbReconnectThread(void *arg);
static void smbLinkMonitorThread(void *arg);
#endif

static int smbOpenGame(void);

#if SMB_DIAG_LOG
/* v4 diag counters: accumulated by DeviceReadSectors, summarized and reset by
   smbReconnectThread every 2 seconds. */
static unsigned int smbRdCount, smbRdFail, smbRdZero, smbRdMaxMs, smbRdSumMs;
static unsigned int smbRdSlow100, smbRdSlow250, smbRdSlow1000;
#endif

static void ps2ip_init(void)
{
    modinfo_t info;
    getModInfo("ps2ip\0\0\0", &info);

    // Set functions pointers here
    plwip_close = info.exports[6];
    plwip_connect = info.exports[7];
    plwip_recv = info.exports[9];
    plwip_recvfrom = info.exports[10];
    plwip_send = info.exports[11];
    plwip_socket = info.exports[13];
    plwip_setsockopt = info.exports[19];
    /* 注意：ps2ip 的导出表只有 41 项（下标 0..40），并不存在 lwip_shutdown。
       原来读 info.exports[46] 是越界读，拿到的是野指针；
       smb_AbortConnection() 一旦被启用会调到野地址。这里先置空。 */
    plwip_shutdown = NULL;
    pinet_addr = info.exports[24];
#if SMB_DIAG_LOG
    plwip_sendto = info.exports[12];
#endif

#if SMB_FEAT_RECONNECT_THREADS
    if (getModInfo("netman\0\0", &info))
        pNetManGetGlobalNetIFLinkState = info.exports[14];
#endif
}

static int smbOpenGame(void)
{
    int i = 0;
    char tmp_str[256];

    // 建立SMB会话
    if (smb_SessionSetupAndX(ServerCapabilities) <= 0)
        return -1;

    // 连接共享目录
    sprintf(tmp_str, "\\\\%s\\%s", cdvdman_settings.smb_ip, cdvdman_settings.smb_share);
    if (smb_TreeConnectAndX(tmp_str) <= 0)
        return -1;

    if (!(cdvdman_settings.common.flags & IOPCORE_SMB_FORMAT_USBLD)) {
        if (cdvdman_settings.smb_prefix[0]) {
            sprintf(tmp_str, "\\%s\\%s\\%s", cdvdman_settings.smb_prefix, cdvdman_settings.common.media == 0x12 ? "CD" : "DVD", cdvdman_settings.filename);
        } else {
            sprintf(tmp_str, "\\%s\\%s", cdvdman_settings.common.media == 0x12 ? "CD" : "DVD", cdvdman_settings.filename);
        }

        if (smb_OpenAndX(tmp_str, (u8 *)&cdvdman_settings.FIDs[i++], 0) <= 0)
            return -1;
    } else {
        // 打开全部分片文件
        for (i = 0; i < cdvdman_settings.common.NumParts; i++) {
            if (cdvdman_settings.smb_prefix[0])
                sprintf(tmp_str, "\\%s\\%s.%02x", cdvdman_settings.smb_prefix, cdvdman_settings.filename, i);
            else
                sprintf(tmp_str, "\\%s.%02x", cdvdman_settings.filename, i);

            if (smb_OpenAndX(tmp_str, (u8 *)&cdvdman_settings.FIDs[i], 0) <= 0)
                return -1;
        }
    }

    return 1;
}

#if SMB_FEAT_RECONNECT_THREADS
static void smbReconnectThread(void *arg)
{
#if SMB_FEAT_ECHO_KEEPALIVE
    int keepAliveCounter = 0;
    int result;
#endif

    (void)arg;

    while (1) {
#if SMB_FEAT_ECHO_KEEPALIVE
        if (smbReconnectEnabled && smbConnectionState == 1 && !smbPhysicalLinkDown) {
            // 每30秒发送一次SMB保活请求，防止服务器回收长时间空闲的会话。
            if (++keepAliveCounter >= 15) {
#if SMB_DIAG_LOG
                unsigned int echoStart = smbDiagNowMs();
#endif
                result = smb_Echo();
#if SMB_DIAG_LOG
                SMBDIAG("ECHO res=%d ms=%u", result, smbDiagNowMs() - echoStart);
#endif
                if (result) {
                    keepAliveCounter = 0;
                    if (result < 0)
                        smbConnectionState = 2;
                }
            }
        } else {
            keepAliveCounter = 0;
        }
#endif

        if (smbReconnectEnabled && smbConnectionState == 2) {
            SMBDIAG("STATE=2 close socket");
            if (smb_io_sema < 0) {
                smb_Disconnect();
                smbConnectionState = 0;
            } else if (PollSema(smb_io_sema) == 0) {
                smb_Disconnect();
                SignalSema(smb_io_sema);
                smbConnectionState = 0;
            }
        }

        if (smbReconnectEnabled && smbConnectionState == 0 && !smbPhysicalLinkDown &&
            (!pNetManGetGlobalNetIFLinkState || pNetManGetGlobalNetIFLinkState())) {
            smbConnectionState = 2;
            SMBDIAG("RECONNECT start");

            if (smb_NegotiateProtocol(cdvdman_settings.smb_ip, cdvdman_settings.smb_port, cdvdman_settings.smb_user, cdvdman_settings.smb_password, &ServerCapabilities, smbHashCallback) > 0 &&
                smbOpenGame() > 0 && smbReconnectEnabled && !smbPhysicalLinkDown &&
                (!pNetManGetGlobalNetIFLinkState || pNetManGetGlobalNetIFLinkState())) {
                smbConnectionState = 1;
                SMBDIAG("RECONNECT ok");
                if (smbTrayOpen) {
                    sceCdTrayReq(SCECdTrayClose, NULL);
                    smbTrayOpen = 0;
                }
                continue;
            }

            SMBDIAG("RECONNECT fail");
            smb_Disconnect();
            smbConnectionState = 0;
        }

#if SMB_DIAG_LOG
        SMBDIAG("HB n=%u fail=%u zero=%u max=%ums avg=%ums s100=%u s250=%u s1k=%u", smbRdCount, smbRdFail,
                smbRdZero, smbRdMaxMs, smbRdCount ? smbRdSumMs / smbRdCount : 0, smbRdSlow100, smbRdSlow250,
                smbRdSlow1000);
        smbRdCount = smbRdFail = smbRdZero = smbRdMaxMs = smbRdSumMs = 0;
        smbRdSlow100 = smbRdSlow250 = smbRdSlow1000 = 0;
#endif
        DelayThread(2000000);
    }
}

static void smbLinkMonitorThread(void *arg)
{
    (void)arg;

    while (1) {
        if (smbReconnectEnabled && pNetManGetGlobalNetIFLinkState) {
            if (!pNetManGetGlobalNetIFLinkState()) {
                if (!smbPhysicalLinkDown) {
                    SMBDIAG("LINK down");
                    smbPhysicalLinkDown = 1;
                    smbTrayOpen = 1;
                    sceCdTrayReq(SCECdTrayOpen, NULL);
                    if (smbConnectionState == 1)
                        smbConnectionState = 2;
                }
            } else {
                if (smbPhysicalLinkDown)
                    SMBDIAG("LINK up");
                smbPhysicalLinkDown = 0;
            }
        }

        DelayThread(500000);
    }
}
#endif /* SMB_FEAT_RECONNECT_THREADS */

void smb_NegotiateProt(OplSmbPwHashFunc_t hash_callback)
{
    ps2ip_init();
    SMBDIAG("NegotiateProt boot");
#if SMB_FEAT_RECONNECT_THREADS
    smbHashCallback = hash_callback;
    while (smb_NegotiateProtocol(cdvdman_settings.smb_ip, cdvdman_settings.smb_port, cdvdman_settings.smb_user, cdvdman_settings.smb_password, &ServerCapabilities, hash_callback) <= 0) {
        SMBDIAG("NegotiateProt fail, retry in 2s");
        smb_Disconnect();
        DelayThread(2000000);
    }
#else
    // 恢复旧行为：启动时只做一次协商
    smb_NegotiateProtocol(cdvdman_settings.smb_ip, cdvdman_settings.smb_port, cdvdman_settings.smb_user, cdvdman_settings.smb_password, &ServerCapabilities, hash_callback);
#endif
    SMBDIAG("NegotiateProt done");
}

void DeviceInit(void)
{
    RegisterLibraryEntries(&_exp_oplsmb);
    SMBDIAG("DeviceInit: diag build, features ECHO=%d TMO=%d KPALV=%d THREADS=%d",
        SMB_FEAT_ECHO_KEEPALIVE, SMB_FEAT_SOCK_TIMEOUT, SMB_FEAT_TCP_KEEPALIVE, SMB_FEAT_RECONNECT_THREADS);

#if SMB_FEAT_RECONNECT_THREADS
    iop_thread_t thread;

    thread.attr = TH_C;
    thread.option = 0;
    thread.thread = smbReconnectThread;
    thread.stacksize = 0x1000;
    thread.priority = 40;

    StartThread(CreateThread(&thread), NULL);

    thread.thread = smbLinkMonitorThread;
    StartThread(CreateThread(&thread), NULL);
#endif
}

void DeviceDeinit(void)
{ // Close all files and disconnect before IOP reboots. Note that this seems to help prevent VMC corruption in some games.
    DeviceUnmount();
}

int DeviceReady(void)
{
#if SMB_FEAT_RECONNECT_THREADS
    return smbConnectionState == 1 ? SCECdComplete : SCECdNotReady;
#else
    // 恢复旧行为：始终报告就绪
    return SCECdComplete;
#endif
}

void DeviceFSInit(void)
{
#if SMB_FEAT_RECONNECT_THREADS
    smbReconnectEnabled = 1;
    if (smbOpenGame() > 0) {
        smbConnectionState = 1;
    } else {
        smb_Disconnect();
        smbConnectionState = 0;
    }
#else
    // 恢复旧行为：单次打开会话/共享/文件，不检查结果
    smbOpenGame();
#endif
    SMBDIAG("DeviceFSInit done state ok");
}

void DeviceLock(void)
{
    WaitSema(smb_io_sema);
}

void DeviceUnmount(void)
{
#if SMB_FEAT_RECONNECT_THREADS
    smbReconnectEnabled = 0;
    if (smbConnectionState == 1)
        smb_CloseAll();
    smbConnectionState = 2;
#else
    // 恢复旧行为：无条件关闭全部文件句柄
    smb_CloseAll();
#endif
    smb_Disconnect();
}

void DeviceStop(void)
{
}

int DeviceReadSectors(u32 lsn, void *buffer, unsigned int sectors)
{
    register u32 r, sectors_to_read, lbound, ubound, nlsn, offslsn;
    register int i, esc_flag = 0, result, bytes_to_read;
    u8 *p = (u8 *)buffer;
    int rv = SCECdErNO;

    lbound = 0;
    ubound = (cdvdman_settings.common.NumParts > 1) ? 0x80000 : 0xFFFFFFFF;
    offslsn = lsn;
    r = nlsn = 0;
    sectors_to_read = sectors;

    for (i = 0; i < cdvdman_settings.common.NumParts; i++, lbound = ubound, ubound += 0x80000, offslsn -= 0x80000) {

        if (lsn >= lbound && lsn < ubound) {
            if ((lsn + sectors) > (ubound - 1)) {
                sectors_to_read = ubound - lsn;
                sectors -= sectors_to_read;
                nlsn = ubound;
            } else
                esc_flag = 1;

            bytes_to_read = sectors_to_read * 2048;
#if SMB_FEAT_RECONNECT_THREADS
#if SMB_DIAG_LOG
            int rdRetries = 0;
            int rdWaited = 0;
            unsigned int rdStart = smbDiagNowMs();
#endif
            for (;;) {
                while (smbReconnectEnabled && !smbPhysicalLinkDown && smbConnectionState != 1) {
#if SMB_DIAG_LOG
                    if (!rdWaited) {
                        SMBDIAG("RD wait-reconnect lsn=%u", (unsigned int)offslsn);
                        rdWaited = 1;
                    }
#endif
                    DelayThread(100000);
                }

                if (!smbReconnectEnabled || smbPhysicalLinkDown) {
                    result = -1;
                    break;
                }

                result = smb_ReadCD(offslsn, sectors_to_read, &p[r], i);
                if (result >= 0)
                    break;

                // 逻辑断线只触发静默重连，当前读取等待连接恢复后再重试。
                SMBDIAG("RD fail lsn=%u res=%d retry=%d", (unsigned int)offslsn, result, rdRetries + 1);
#if SMB_DIAG_LOG
                rdRetries++;
#endif
                smbConnectionState = 2;
            }
#if SMB_DIAG_LOG
            {
                unsigned int rdDur = smbDiagNowMs() - rdStart;

                smbRdCount++;
                if (rdDur > smbRdMaxMs)
                    smbRdMaxMs = rdDur;
                smbRdSumMs += rdDur;
                if (rdDur >= 1000)
                    smbRdSlow1000++;
                else if (rdDur >= 250)
                    smbRdSlow250++;
                else if (rdDur >= 100)
                    smbRdSlow100++;
                smbRdFail += rdRetries;
                if (rdDur >= 100 || rdRetries || rdWaited)
                    SMBDIAG("RD done lsn=%u sec=%u ms=%u res=%d retries=%d buf=%p", (unsigned int)offslsn, sectors_to_read, rdDur, result, rdRetries, &p[r]);
            }
#endif

            if (result < 0) {
                rv = SCECdErTRMOPN;
                break;
            }
#if SMB_FEAT_SHORTREAD_ZEROFILL
            if (result < bytes_to_read) {
#if SMB_DIAG_LOG
                SMBDIAG("ZEROFILL lsn=%u got=%d want=%d buf=%p", (unsigned int)offslsn, result, bytes_to_read, &p[r]);
                smbRdZero++;
#endif
                memset(&p[r + result], 0, bytes_to_read - result);
            }
#else
            if (result < bytes_to_read) {
                rv = SCECdErREAD;
                break;
            }
#endif
#else  /* !SMB_FEAT_RECONNECT_THREADS */
            // 恢复旧行为：单次读取，失败立即向游戏报告读错误
            result = smb_ReadCD(offslsn, sectors_to_read, &p[r], i);
            if (result <= 0) {
                rv = SCECdErREAD;
                break;
            }
#if SMB_FEAT_SHORTREAD_ZEROFILL
            if (result < bytes_to_read) {
#if SMB_DIAG_LOG
                SMBDIAG("ZEROFILL lsn=%u got=%d want=%d buf=%p", (unsigned int)offslsn, result, bytes_to_read, &p[r]);
                smbRdZero++;
#endif
                memset(&p[r + result], 0, bytes_to_read - result);
            }
#else
            if (result < bytes_to_read) {
                rv = SCECdErREAD;
                break;
            }
#endif
#endif /* SMB_FEAT_RECONNECT_THREADS */

            r += bytes_to_read;
            offslsn += sectors_to_read;
            sectors_to_read = sectors;
            lsn = nlsn;
        }

        if (esc_flag)
            break;
    }

    return rv;
}
