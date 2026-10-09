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
                result = smb_Echo();
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

            if (smb_NegotiateProtocol(cdvdman_settings.smb_ip, cdvdman_settings.smb_port, cdvdman_settings.smb_user, cdvdman_settings.smb_password, &ServerCapabilities, smbHashCallback) > 0 &&
                smbOpenGame() > 0 && smbReconnectEnabled && !smbPhysicalLinkDown &&
                (!pNetManGetGlobalNetIFLinkState || pNetManGetGlobalNetIFLinkState())) {
                smbConnectionState = 1;
                if (smbTrayOpen) {
                    sceCdTrayReq(SCECdTrayClose, NULL);
                    smbTrayOpen = 0;
                }
                continue;
            }

            smb_Disconnect();
            smbConnectionState = 0;
        }

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
                    smbPhysicalLinkDown = 1;
                    smbTrayOpen = 1;
                    sceCdTrayReq(SCECdTrayOpen, NULL);
                    if (smbConnectionState == 1)
                        smbConnectionState = 2;
                }
            } else {
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
#if SMB_FEAT_RECONNECT_THREADS
    smbHashCallback = hash_callback;
    while (smb_NegotiateProtocol(cdvdman_settings.smb_ip, cdvdman_settings.smb_port, cdvdman_settings.smb_user, cdvdman_settings.smb_password, &ServerCapabilities, hash_callback) <= 0) {
        smb_Disconnect();
        DelayThread(2000000);
    }
#else
    // 恢复旧行为：启动时只做一次协商
    smb_NegotiateProtocol(cdvdman_settings.smb_ip, cdvdman_settings.smb_port, cdvdman_settings.smb_user, cdvdman_settings.smb_password, &ServerCapabilities, hash_callback);
#endif
}

void DeviceInit(void)
{
    RegisterLibraryEntries(&_exp_oplsmb);

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
            for (;;) {
                while (smbReconnectEnabled && !smbPhysicalLinkDown && smbConnectionState != 1)
                    DelayThread(100000);

                if (!smbReconnectEnabled || smbPhysicalLinkDown) {
                    result = -1;
                    break;
                }

                result = smb_ReadCD(offslsn, sectors_to_read, &p[r], i);
                if (result >= 0)
                    break;

                // 逻辑断线只触发静默重连，当前读取等待连接恢复后再重试。
                smbConnectionState = 2;
            }

            if (result < 0) {
                rv = SCECdErTRMOPN;
                break;
            }
#if SMB_FEAT_SHORTREAD_ZEROFILL
            if (result < bytes_to_read)
                memset(&p[r + result], 0, bytes_to_read - result);
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
            if (result < bytes_to_read)
                memset(&p[r + result], 0, bytes_to_read - result);
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
