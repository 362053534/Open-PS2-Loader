/*
  Copyright 2009-2010, jimmikaelkael
  Licenced under Academic Free License version 3.0
  Review Open PS2 Loader README & LICENSE files for further details.
*/

#include "smstcpip.h"
#include "internal.h"

#include "device.h"

/* 空闲 1 分钟发一次 Echo。拍长仍是 2 秒，重连的 DelayThread 不变。 */
#define SMB_ECHO_INTERVAL_US 2000000
#define SMB_ECHO_IDLE_TICKS  30

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
static volatile int smb_echo_due;
static volatile int smb_echo_busy;
static volatile int smb_in_read;
static volatile int smb_alarm_armed;
static iop_sys_clock_t smb_alarm_clock;
static int (*pNetManGetGlobalNetIFLinkState)(void);
static int (*pSmapGetLinkStatus)(void);

static int smbOpenGame(void);
static unsigned int smb_echo_alarm_cb(void *arg);

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

/* 只由读线程在数据读卡住的轮询里调用：链路断了才模拟开仓。开机和空闲时不会走到这里。
 * 返回 1 表示链路断开，调用方放弃本次读，让游戏拿到 SCECdErTRMOPN 去提示插盘。 */
int smb_read_stall_check(void)
{
    if (smb_link_up())
        return 0;
    // 断线时开仓一次，游戏才会弹插盘提示；同时放弃本次读
    if (!smbTrayOpen) {
        smbTrayOpen = 1;
        smb_echo_due = 0; // 开仓后不再发 Echo，已排队的也取消
        sceCdTrayReq(SCECdTrayOpen, NULL);
    }
    return 1;
}

/* 只由读线程在数据读重新收到数据时调用：之前开过仓就关回去。 */
void smb_read_progress(void)
{
    if (smbTrayOpen) {
        sceCdTrayReq(SCECdTrayClose, NULL);
        smbTrayOpen = 0;
    }
}

/* 非 0 表示内核按这个系统时钟把同一只闹钟续上。再 iSetAlarm 会多挂一只。 */
static unsigned int smb_alarm_interval(void)
{
    if (smb_alarm_clock.lo != 0)
        return smb_alarm_clock.lo;
    return 1;
}

/* 闹钟只排队，不能在中断里收发网络包。 */
static unsigned int smb_echo_alarm_cb(void *arg)
{
    (void)arg;
    if (!smb_echo_enabled)
        return 0;

    /* 仓已经开着时不排 Echo。链路是否恢复只在读线程里看，避免闹钟里读 PHY。 */
    if (smbTrayOpen) {
        if (!smb_in_read && !smb_echo_busy)
            cdvdman_kick_rthread(1);
        return smb_alarm_interval();
    }

    /* 空闲期间只安排 Echo。开仓仍只由卡住的数据读触发。 */
    if (smb_in_read || smb_echo_due)
        return smb_alarm_interval();

    if (smbIdleTicks < SMB_ECHO_IDLE_TICKS)
        smbIdleTicks++;

    if (smbIdleTicks == SMB_ECHO_IDLE_TICKS && !smb_echo_busy) {
        smb_echo_due = 1;
        cdvdman_kick_rthread(1);
    }
    /* 同一只周期闹钟由内核续上，避免读线程与回调重复挂载。 */
    return smb_alarm_interval();
}

static void smb_arm_echo_alarm(void)
{
    int r;

    if (smb_alarm_armed || !smb_echo_enabled)
        return;

    if (smb_alarm_clock.hi == 0 && smb_alarm_clock.lo == 0)
        USec2SysClock(SMB_ECHO_INTERVAL_US, &smb_alarm_clock);

    r = SetAlarm(&smb_alarm_clock, smb_echo_alarm_cb, NULL);
    /* -104 是 KE_FOUND_HANDLER：这一拍已经挂着。 */
    if (r == 0 || r == -104)
        smb_alarm_armed = 1;
}

static void smb_disarm_echo_alarm(void)
{
    smb_alarm_armed = 0;
    CancelAlarm(smb_echo_alarm_cb, NULL);
}

/* 返回 1 表示这次醒来没有排队的读，调用方不能再读上一笔 LBA。 */
int smb_read_thread_on_wake(int read_busy)
{
    if (!smb_echo_enabled)
        return read_busy ? 0 : 1;

    if (read_busy) {
        smb_echo_due = 0;
        return 0;
    }

    /* 插回网线后关仓，游戏才会离开插盘画面再发读。这次不发 Echo，避免占住读线程。 */
    if (smbTrayOpen) {
        if (smb_link_up()) {
            sceCdTrayReq(SCECdTrayClose, NULL);
            smbTrayOpen = 0;
        }
        smb_echo_due = 0;
        return 1;
    }

    if (!smb_echo_due)
        return 1;

    smb_echo_busy = 1;
    smb_echo_due = 0;
    /* 以发起时间计下一分钟。等待的 5 秒里闹钟照样计数，不把周期再往后推。 */
    smbIdleTicks = 0;
    /* Echo 与真实读盘共用读线程和 I/O 锁，收包结束或超时后才交回数据面。 */
    smb_Echo();
    smb_echo_busy = 0;
    return 1;
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
    // 数据读确认断线后才模拟开仓，空闲 Echo 不改变就绪状态。
    return smbTrayOpen ? SCECdNotReady : SCECdComplete;
}

int DeviceFSInit(void)
{
    smbIdleTicks = 0;
    smb_echo_due = 0;
    smb_echo_busy = 0;
    smb_in_read = 0;
    /* 先别开闹钟。smbinit 留下的套接字还要用来建会话。 */
    smb_echo_enabled = 0;
    if (smbOpenGame() > 0) {
        smb_echo_enabled = 1;
        smb_arm_echo_alarm();
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
    smb_echo_enabled = 0;
    smb_echo_due = 0;
    smb_echo_busy = 0;
    smb_disarm_echo_alarm();
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
    /* 读的整段都算有流量，期间不排 Echo。 */
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
            if (result >= 0) {
                smbIdleTicks = 0;
                smb_echo_due = 0;
            }
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
