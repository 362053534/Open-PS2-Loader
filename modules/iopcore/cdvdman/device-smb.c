/*
  Copyright 2009-2010, jimmikaelkael
  Licenced under Academic Free License version 3.0
  Review Open PS2 Loader README & LICENSE files for further details.
*/

#include "smstcpip.h"
#include "internal.h"

#include "device.h"

#define SMB_RECONNECT_INTERVAL_US 2000000
#define SMB_RECOVERY_WAIT_US      100000
#define SMB_ECHO_IDLE_TICKS       60
#define SMB_ECHO_RETRY_COUNT      2
#define SMB_CONNECTION_WAIT_LINK  3
#define SMB_CONNECTION_RETRY_WAIT 4

#define SMB_RECONNECT_IDLE    0
#define SMB_RECONNECT_PENDING 1
#define SMB_RECONNECT_SUCCESS 2
#define SMB_RECONNECT_FAILED  3

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
u32 (*pinet_addr)(const char *cp);                                                                                                         // #24

static u32 ServerCapabilities;
static OplSmbPwHashFunc_t smbHashCallback;
static volatile int smbConnectionState = 2;
static volatile int smbReconnectEnabled;
static volatile int smbReconnectResult;
static volatile int smbTrayOpen;
static volatile int smbReconnectStopping;
static volatile int smbReconnectStopped;
static volatile unsigned int smbIdleTicks;
static volatile unsigned int smbEchoRetryCount;
static int smbReconnectThreadID = -1;
static int (*pNetManGetGlobalNetIFLinkState)(void);
static int (*pSmapGetLinkStatus)(void);

static int smbOpenGame(void);
static void smbReconnectThread(void *arg);

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
    pinet_addr = info.exports[24];

    if (getModInfo("netman\0\0", &info))
        pNetManGetGlobalNetIFLinkState = info.exports[14];

    if (getModInfo("smaplink", &info))
        pSmapGetLinkStatus = info.exports[4];
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

static void smbReconnectThread(void *arg)
{
    int result;

    (void)arg;

    while (1) {
        if (smbReconnectStopping) {
            smbReconnectStopped = 1;
            // 不永久休眠线程，避免下一次重新初始化时无法恢复重连线程。
            DelayThread(SMB_RECONNECT_INTERVAL_US);
            continue;
        }

        if (smbReconnectEnabled && smbConnectionState == 1) {
            // 只在SMB连续空闲120秒后保活，避免Echo插入正常游戏读取。
            if (smbIdleTicks < SMB_ECHO_IDLE_TICKS)
                smbIdleTicks++;

            if (smbIdleTicks >= SMB_ECHO_IDLE_TICKS) {
                result = smb_Echo();
                if (result > 0) {
                    smbIdleTicks = 0;
                    smbEchoRetryCount = 0;
                } else if (result < 0) {
                    // 单次网络抖动不足以判定断线，连续补测两次仍失败才重连。
                    if (smbEchoRetryCount < SMB_ECHO_RETRY_COUNT) {
                        smbEchoRetryCount++;
                    } else {
                        smbIdleTicks = 0;
                        smbEchoRetryCount = 0;
                        smbReconnectResult = SMB_RECONNECT_PENDING;
                        smbConnectionState = 2;
                    }
                }
            }
        } else {
            smbIdleTicks = 0;
            smbEchoRetryCount = 0;
        }

        if (smbReconnectEnabled && smbConnectionState == 2) {
            if (smb_io_sema < 0) {
                smb_Disconnect();
                smbConnectionState = 0;
            } else if (PollSema(smb_io_sema) == 0) {
                smb_Disconnect();
                SignalSema(smb_io_sema);
                smbConnectionState = 0;
            }
        }

        if (smbReconnectEnabled && smbConnectionState == SMB_CONNECTION_WAIT_LINK) {
            result = pSmapGetLinkStatus ? pSmapGetLinkStatus() :
                                         (pNetManGetGlobalNetIFLinkState ? pNetManGetGlobalNetIFLinkState() : 1);
            if (result) {
                smbReconnectResult = SMB_RECONNECT_PENDING;
                smbConnectionState = 0;
            }
        }

        if (smbReconnectEnabled && smbConnectionState == SMB_CONNECTION_RETRY_WAIT) {
            result = pSmapGetLinkStatus ? pSmapGetLinkStatus() :
                                         (pNetManGetGlobalNetIFLinkState ? pNetManGetGlobalNetIFLinkState() : 1);
            if (result) {
                smbReconnectResult = SMB_RECONNECT_PENDING;
                smbConnectionState = 0;
            } else
                smbConnectionState = SMB_CONNECTION_WAIT_LINK;
        }

        if (smbReconnectEnabled && smbConnectionState == 0) {
            smbReconnectResult = SMB_RECONNECT_PENDING;
            smbConnectionState = 2;

            if (smb_NegotiateProtocol(cdvdman_settings.smb_ip, cdvdman_settings.smb_port, cdvdman_settings.smb_user, cdvdman_settings.smb_password, &ServerCapabilities, smbHashCallback) > 0 &&
                smbOpenGame() > 0 && smbReconnectEnabled && smbConnectionState == 2) {
                smbReconnectResult = SMB_RECONNECT_SUCCESS;
                smbConnectionState = 1;
                smbIdleTicks = 0;
                smbEchoRetryCount = 0;
                continue;
            }

            smb_Disconnect();
            if (smbReconnectEnabled) {
                smbReconnectResult = SMB_RECONNECT_FAILED;
                if (smbConnectionState != SMB_CONNECTION_WAIT_LINK) {
                    result = pSmapGetLinkStatus ? pSmapGetLinkStatus() :
                                                 (pNetManGetGlobalNetIFLinkState ? pNetManGetGlobalNetIFLinkState() : 1);
                    smbConnectionState = result ? SMB_CONNECTION_RETRY_WAIT : SMB_CONNECTION_WAIT_LINK;
                }
            }
        }

        DelayThread(SMB_RECONNECT_INTERVAL_US);
    }
}

void smb_NegotiateProt(OplSmbPwHashFunc_t hash_callback)
{
    ps2ip_init();
    smbHashCallback = hash_callback;
    while (smb_NegotiateProtocol(cdvdman_settings.smb_ip, cdvdman_settings.smb_port, cdvdman_settings.smb_user, cdvdman_settings.smb_password, &ServerCapabilities, hash_callback) <= 0) {
        smb_Disconnect();
        DelayThread(SMB_RECONNECT_INTERVAL_US);
    }
}

void DeviceInit(void)
{
    iop_thread_t thread;

    RegisterLibraryEntries(&_exp_oplsmb);

    thread.attr = TH_C;
    thread.option = 0;
    thread.thread = smbReconnectThread;
    thread.stacksize = 0x1000;
    thread.priority = 40;

    smbReconnectThreadID = CreateThread(&thread);
    if (smbReconnectThreadID >= 0)
        StartThread(smbReconnectThreadID, NULL);
    else
        smbReconnectStopped = 1;
}

void DeviceDeinit(void)
{ // Close all files and disconnect before IOP reboots. Note that this seems to help prevent VMC corruption in some games.
    DeviceUnmount();
}

int DeviceReady(void)
{
    return smbConnectionState == 1 ? SCECdComplete : SCECdNotReady;
}

void DeviceFSInit(void)
{
    smbReconnectStopping = 0;
    smbReconnectStopped = 0;
    smbReconnectEnabled = 1;
    smbReconnectResult = SMB_RECONNECT_IDLE;
    smbIdleTicks = 0;
    smbEchoRetryCount = 0;
    if (smbOpenGame() > 0) {
        smbConnectionState = 1;
    } else {
        smb_Disconnect();
        smbReconnectResult = SMB_RECONNECT_PENDING;
        smbConnectionState = 0;
    }
}

void DeviceLock(void)
{
    // IGR必须先让后台重连进入静止状态，避免等待锁时又产生新的SMB操作。
    smbReconnectStopping = 1;
    smbReconnectEnabled = 0;
    // 不在此处调用关闭网络的RPC，避免与后台线程正在等待的recv发生RPC互锁。
    if (smbReconnectThreadID >= 0) {
        while (!smbReconnectStopped)
            DelayThread(1000);
    }

    WaitSema(smb_io_sema);
}

void DeviceUnmount(void)
{
    smbReconnectEnabled = 0;
    smbReconnectResult = SMB_RECONNECT_IDLE;
    smbIdleTicks = 0;
    smbEchoRetryCount = 0;
    if (smbConnectionState == 1)
        smb_CloseAll();
    smbConnectionState = 2;
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
            for (;;) {
                while (smbReconnectEnabled && smbConnectionState != 1) {
                    DelayThread(SMB_RECOVERY_WAIT_US);

                    if (!smbReconnectEnabled)
                        break;

                    if (smbReconnectResult == SMB_RECONNECT_FAILED || smbReconnectResult == SMB_RECONNECT_PENDING) {
                        result = pSmapGetLinkStatus ? pSmapGetLinkStatus() :
                                                     (pNetManGetGlobalNetIFLinkState ? pNetManGetGlobalNetIFLinkState() : 1);
                        if (!result) {
                            if (!smbTrayOpen) {
                                smbTrayOpen = 1;
                                sceCdTrayReq(SCECdTrayOpen, NULL);
                            }
                            smbConnectionState = SMB_CONNECTION_WAIT_LINK;
                        }
                    }
                }

                if (!smbReconnectEnabled) {
                    result = -1;
                    break;
                }

                result = smb_ReadCD(offslsn, sectors_to_read, &p[r], i);
                if (result >= 0) {
                    smbReconnectResult = SMB_RECONNECT_IDLE;
                    smbIdleTicks = 0;
                    smbEchoRetryCount = 0;
                    if (smbTrayOpen) {
                        sceCdTrayReq(SCECdTrayClose, NULL);
                        smbTrayOpen = 0;
                    }
                    break;
                }

                smbIdleTicks = 0;
                smbEchoRetryCount = 0;

                // 逻辑断线只触发静默重连，当前读取等待连接恢复后再重试。
                smbReconnectResult = SMB_RECONNECT_PENDING;
                smbConnectionState = 2;
            }

            if (result < 0) {
                rv = SCECdErTRMOPN;
                break;
            }
            if (result < bytes_to_read)
                memset(&p[r + result], 0, bytes_to_read - result);

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
