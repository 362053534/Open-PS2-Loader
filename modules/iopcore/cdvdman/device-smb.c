/*
  Copyright 2009-2010, jimmikaelkael
  Licenced under Academic Free License version 3.0
  Review Open PS2 Loader README & LICENSE files for further details.
*/

#include "smstcpip.h"
#include "internal.h"

#include "device.h"

/* 空闲保活：轻线程每 2 秒一拍，60 拍就是 120 秒，只在没有真实读盘时累加。 */
#define SMB_ECHO_INTERVAL_US 2000000
#define SMB_ECHO_IDLE_TICKS  60

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
static volatile int smbTrayOpen;
static volatile unsigned int smbIdleTicks;
static volatile int smb_echo_enabled;
static volatile int smb_in_read;
static volatile int smbKeepaliveStopping;
static volatile int smbKeepaliveStopped;
static int smbKeepaliveThreadID = -1;
static int (*pNetManGetGlobalNetIFLinkState)(void);
static int (*pSmapGetLinkStatus)(void);

static int smbOpenGame(void);
static void smbKeepaliveThread(void *arg);
static int smb_start_keepalive_thread(void);
static void smb_stop_keepalive_thread(void);

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

/* 返回 1 表示链路正常。两个查询接口都拿不到时按正常处理，不模拟开仓。 */
static int smb_link_up(void)
{
    if (pSmapGetLinkStatus)
        return pSmapGetLinkStatus() ? 1 : 0;
    if (pNetManGetGlobalNetIFLinkState)
        return pNetManGetGlobalNetIFLinkState() ? 1 : 0;
    return 1;
}

/* 开/关仓只走这里：已是目标状态就直接返回，避免轻线程和 stall_check 重复调 TrayReq。 */
static void smb_set_tray_open(int open)
{
    if (open) {
        if (smbTrayOpen)
            return;
        smbTrayOpen = 1;
        sceCdTrayReq(SCECdTrayOpen, NULL);
    } else {
        if (!smbTrayOpen)
            return;
        sceCdTrayReq(SCECdTrayClose, NULL);
        smbTrayOpen = 0;
    }
}

/* 只由读线程在数据读卡住的轮询里调用：链路断了才模拟开仓。
 * 返回 1 表示链路断开，调用方放弃本次读，让游戏拿到 SCECdErTRMOPN 去提示插盘。 */
int smb_read_stall_check(void)
{
    if (smb_link_up())
        return 0;
    smb_set_tray_open(1);
    return 1;
}

/* 轻线程：暂只做空闲 Echo（二分 Jak；链路开/关仓先关掉）。
 * 200ms 切片方便卸载时尽快退出；10 片凑满一拍约 2s。 */
#define SMB_KEEPALIVE_SLICE_US       200000
#define SMB_KEEPALIVE_SLICES_PER_TICK 10

static void smbKeepaliveThread(void *arg)
{
    unsigned int slice = 0;

    (void)arg;
    while (!smbKeepaliveStopping) {
        DelayThread(SMB_KEEPALIVE_SLICE_US);
        if (smbKeepaliveStopping)
            break;
        if (!smb_echo_enabled)
            continue;

        slice++;
        if (slice < SMB_KEEPALIVE_SLICES_PER_TICK)
            continue;
        slice = 0;

        /* 二分用：轻线程暂不查链路/开关仓，只发 Echo；开仓仍靠 stall_check。 */
        if (smb_in_read || smbTrayOpen)
            continue;

        if (smbIdleTicks < SMB_ECHO_IDLE_TICKS)
            smbIdleTicks++;
        if (smbIdleTicks < SMB_ECHO_IDLE_TICKS)
            continue;

        /* smb_Echo 内部 PollSema：拿不到锁就保持满拍，下个 2s 再试。 */
        if (smb_Echo() > 0)
            smbIdleTicks = 0;
    }

    smbKeepaliveStopped = 1;
}

static int smb_start_keepalive_thread(void)
{
    iop_thread_t thread;

    if (smbKeepaliveThreadID >= 0)
        return 0;

    smbKeepaliveStopping = 0;
    smbKeepaliveStopped = 0;

    thread.attr = TH_C;
    thread.option = 0;
    thread.thread = &smbKeepaliveThread;
    thread.stacksize = 0x800;
    thread.priority = 40;

    smbKeepaliveThreadID = CreateThread(&thread);
    if (smbKeepaliveThreadID < 0) {
        smbKeepaliveStopped = 1;
        return -1;
    }
    StartThread(smbKeepaliveThreadID, NULL);
    return 0;
}

static void smb_stop_keepalive_thread(void)
{
    smb_echo_enabled = 0;
    smbKeepaliveStopping = 1;
    if (smbKeepaliveThreadID < 0) {
        smbKeepaliveStopped = 1;
        return;
    }
    while (!smbKeepaliveStopped)
        DelayThread(100000);
    /* 线程已自行退出，只回收控制块。 */
    DeleteThread(smbKeepaliveThreadID);
    smbKeepaliveThreadID = -1;
}

/* 轻线程已接手 Echo/开关仓；这里只告诉读循环：没排队的读就别重读上一笔。 */
int smb_read_thread_on_wake(int read_busy)
{
    return !read_busy;
}

void smb_NegotiateProt(OplSmbPwHashFunc_t hash_callback)
{
    ps2ip_init();
    while (smb_NegotiateProtocol(cdvdman_settings.smb_ip, cdvdman_settings.smb_port, cdvdman_settings.smb_user, cdvdman_settings.smb_password, &ServerCapabilities, hash_callback) <= 0) {
        smb_Disconnect();
        DelayThread(SMB_ECHO_INTERVAL_US);
    }
}

void DeviceInit(void)
{
    RegisterLibraryEntries(&_exp_oplsmb);
}

void DeviceDeinit(void)
{ // Close all files and disconnect before IOP reboots. Note that this seems to help prevent VMC corruption in some games.
    DeviceUnmount();
}

int DeviceReady(void)
{
    // 拔线开仓期间报未就绪，让游戏停在插盘提示里等，不去读盘；关仓后恢复就绪
    return smbTrayOpen ? SCECdNotReady : SCECdComplete;
}

int DeviceFSInit(void)
{
    smbIdleTicks = 0;
    smb_in_read = 0;
    /* 先别启轻线程。smbinit 留下的套接字还要用来建会话。 */
    smb_echo_enabled = 0;
    if (smbOpenGame() > 0) {
        smb_echo_enabled = 1;
        if (smb_start_keepalive_thread() < 0)
            smb_echo_enabled = 0;
    } else {
        smb_Disconnect();
    }

    return 0;
}

void DeviceLock(void)
{
    // 只保证网络正常时 IGR/关机能拿到锁；拔线时进行中的读不会被打断
    WaitSema(smb_io_sema);
}

void DeviceUnmount(void)
{
    smb_stop_keepalive_thread();
    smbIdleTicks = 0;
    smb_CloseAll();
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
    /* 已开仓且链路仍断：像 BDM 拔盘一样立即报开仓，不去阻塞等待。 */
    if (smbTrayOpen && !smb_link_up())
        return SCECdErTRMOPN;
    /* 读的整段都算有流量。轻线程见 smb_in_read 就不发 Echo。 */
    smb_in_read = 1;

    for (i = 0; i < cdvdman_settings.common.NumParts; i++, lbound = ubound, ubound += 0x80000, offslsn -= 0x80000) {

        if (lsn >= lbound && lsn < ubound) {
            if ((lsn + sectors) > (ubound - 1)) {
                sectors_to_read = ubound - lsn;
                sectors -= sectors_to_read;
                nlsn = ubound;
            } else
                esc_flag = 1;

            bytes_to_read = sectors_to_read * 2048;
            result = smb_ReadCD(offslsn, sectors_to_read, &p[r], i);
            if (result >= 0)
                smbIdleTicks = 0;
            if (result == SMB_READ_LINK_DOWN) {
                rv = SCECdErTRMOPN;
                break;
            }
            if (result < 0) {
                rv = SCECdErREAD;
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

    smb_in_read = 0;
    return rv;
}
