/*
  Copyright 2009-2010, jimmikaelkael
  Licenced under Academic Free License version 3.0
  Review Open PS2 Loader README & LICENSE files for further details.
*/

#include "smstcpip.h"
#include "internal.h"

#include "device.h"

#define SMB_RECONNECT_INTERVAL_US 2000000
#define SMB_ECHO_IDLE_TICKS       60
/* 断线后退栈重试时拆成这么长再看一次放弃，总和仍是上面的 2 秒。 */
#define SMB_RECONNECT_POLL_US     100000
/* 套接字上的 SO_RCVTIMEO 仍是 30 秒。闹钟 2 秒一拍：已经放弃时第一拍叫醒，否则数满 15 拍。 */
#define SMB_ABORT_ALARM_LIMIT     1
#define SMB_BLOCK_ALARM_LIMIT     15
/* 读路径跟主机测试共用，避免两套条件。 */
#define SMB_WAIT_ABORT            1
#define SMB_WAIT_READY            2
#define SMB_WAIT_RETRY            3
#define SMB_WAIT_KICK             4

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
static OplSmbPwHashFunc_t smbHashCallback;
static volatile int smbConnectionState = 2;
static volatile int smbReconnectEnabled;
static volatile int smbTrayOpen;
static volatile unsigned int smbIdleTicks;
static volatile int smb_abort_wait;
static volatile int smb_alarm_armed;
static volatile int smb_link_service;
static volatile int smb_in_read_thread;
static volatile int smb_retry_link;
static volatile int smb_block_alarms;
static volatile unsigned int smb_block_gen_seen;
static volatile unsigned int smb_block_poked_gen;
static iop_sys_clock_t smb_alarm_clock;
static int (*pNetManGetGlobalNetIFLinkState)(void);

static int smbOpenGame(void);
static void smb_link_once(void);
static unsigned int smb_link_alarm_cb(void *arg);
void smb_HashPasswordCached(server_specs_t *ss);

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
    plwip_shutdown = info.exports[46];
    pinet_addr = info.exports[24];

    if (getModInfo("netman\0\0", &info))
        pNetManGetGlobalNetIFLinkState = info.exports[14];
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

static int smb_read_wait_action(int abort_wait, int enabled, int state, int in_read_thread)
{
    if (abort_wait)
        return SMB_WAIT_ABORT;
    if (!enabled || state == 1)
        return SMB_WAIT_READY;
    if (in_read_thread)
        return SMB_WAIT_RETRY;
    return SMB_WAIT_KICK;
}

static int smb_alarm_should_wake(int blocking, int abort_wait, int alarms, int already_poked)
{
    if (!blocking || already_poked || alarms <= 0)
        return 0;
    if (abort_wait)
        return alarms >= SMB_ABORT_ALARM_LIMIT;
    return alarms >= SMB_BLOCK_ALARM_LIMIT;
}

static int smb_should_kick_link(int kicked, int alarm_armed, unsigned int kick_wait)
{
    /* 第一次立刻踢。闹钟还挂着就别再踢；丢了才隔满 2 秒补一次。 */
    return !kicked || (!alarm_armed && kick_wait >= SMB_RECONNECT_INTERVAL_US);
}

/* 原来重连线程里的一轮。没有 DelayThread，调用方必须已经在浅栈上。
   这里不置读盘阻塞标志：放弃只该打断真正的读，不该打断 Echo。 */
static void smb_link_once(void)
{
    int result;

    if (smbReconnectEnabled && smbConnectionState == 1) {
        // 只在SMB连续空闲120秒后保活，避免Echo插入正常游戏读取。
        if (smbIdleTicks < SMB_ECHO_IDLE_TICKS)
            smbIdleTicks++;

        if (smbIdleTicks >= SMB_ECHO_IDLE_TICKS) {
            result = smb_Echo();
            if (result > 0) {
                smbIdleTicks = 0;
            } else if (result < 0) {
                smbIdleTicks = 0;
                smbConnectionState = 2;
            }
        }
    } else {
        smbIdleTicks = 0;
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

    if (smbReconnectEnabled && smbConnectionState == 0) {
        smbConnectionState = 2;

        if (smb_NegotiateProtocol(cdvdman_settings.smb_ip, cdvdman_settings.smb_port, cdvdman_settings.smb_user, cdvdman_settings.smb_password, &ServerCapabilities, smbHashCallback) > 0 &&
            smbOpenGame() > 0 && smbReconnectEnabled) {
            smbConnectionState = 1;
            smbIdleTicks = 0;
            if (smbTrayOpen) {
                sceCdTrayReq(SCECdTrayClose, NULL);
                smbTrayOpen = 0;
            }
            return;
        }

        smb_Disconnect();
        smbConnectionState = 0;
    }
}

static unsigned int smb_link_alarm_cb(void *arg)
{
    unsigned int gen;
    int r;

    (void)arg;
    smb_alarm_armed = 0;

    if (!smbReconnectEnabled)
        return 0;

    if (smb_read_is_blocking()) {
        gen = smb_read_block_gen();
        if (gen != smb_block_gen_seen) {
            smb_block_gen_seen = gen;
            smb_block_alarms = 0;
        }
        smb_block_alarms++;
        if (smb_alarm_should_wake(1, smb_abort_wait, smb_block_alarms, smb_block_poked_gen == gen)) {
            if (iReleaseWaitThread(cdvdman_read_thread_id()) == 0)
                smb_block_poked_gen = gen;
        }
        /* 读线程停在 recv，到不了 SetAlarm。回调返回 0，在这里续下一拍。 */
        if (smb_alarm_clock.lo != 0 || smb_alarm_clock.hi != 0) {
            r = iSetAlarm(&smb_alarm_clock, smb_link_alarm_cb, NULL);
            if (r == 0)
                smb_alarm_armed = 1;
        }
        return 0;
    }

    smb_block_alarms = 0;
    smb_block_gen_seen = 0;
    /* 没在收包时才让读线程去做 Echo 或重连，避免它还在 link_once 里又被叫进去。 */
    smb_link_service = 1;
    cdvdman_kick_rthread(1);
    /* 返回 0：这一拍已经从队列摘掉。下一拍由读线程 SetAlarm，不在这里再挂一只。 */
    return 0;
}

static void smb_arm_link_alarm(void)
{
    int r;

    if (smb_alarm_armed || !smbReconnectEnabled)
        return;

    if (smb_alarm_clock.hi == 0 && smb_alarm_clock.lo == 0)
        USec2SysClock(SMB_RECONNECT_INTERVAL_US, &smb_alarm_clock);

    r = SetAlarm(&smb_alarm_clock, smb_link_alarm_cb, NULL);
    /* -104 是 KE_FOUND_HANDLER：这一拍已经挂着。 */
    if (r == 0 || r == -104)
        smb_alarm_armed = 1;
}

static void smb_disarm_link_alarm(void)
{
    smb_alarm_armed = 0;
    CancelAlarm(smb_link_alarm_cb, NULL);
}

void smb_ensure_link_alarm(void)
{
    smb_arm_link_alarm();
}

void smb_read_thread_maintenance(int read_pending)
{
    int disconnected;

    if (smbReconnectEnabled && !smb_alarm_armed)
        smb_arm_link_alarm();

    disconnected = smbConnectionState != 1;
    if (!smb_link_service && !disconnected)
        return;

    /* 这次醒来就要读，而且已经断线：等 cdvdman_read 退栈再重连，避免同一轮连两次。 */
    if (read_pending && disconnected) {
        smb_link_service = 0;
        return;
    }

    smb_link_service = 0;
    if (smbReconnectEnabled)
        smb_link_once();
}

void smb_set_in_read_thread(int in_thread)
{
    smb_in_read_thread = in_thread ? 1 : 0;
}

int smb_finish_read_attempt(void)
{
    unsigned int waited;

    if (!smb_retry_link)
        return 0;
    smb_retry_link = 0;

    if (smb_abort_wait) {
        cdvdman_stat.err = SCECdErABRT;
        return 0;
    }

    /* 游戏还堵在 sceCdSync。重连期间保持读状态，内部退栈码不要露出去。 */
    cdvdman_stat.status = SCECdStatRead;
    cdvdman_stat.err = SCECdErNO;

    smb_link_once();

    if (smb_abort_wait) {
        cdvdman_stat.err = SCECdErABRT;
        return 0;
    }

    if (smbReconnectEnabled && smbConnectionState == 1)
        return 1;

    if (!smbReconnectEnabled) {
        cdvdman_stat.err = SCECdErTRMOPN;
        return 0;
    }

    waited = 0;
    while (waited < SMB_RECONNECT_INTERVAL_US) {
        if (smb_abort_wait) {
            cdvdman_stat.err = SCECdErABRT;
            return 0;
        }
        DelayThread(SMB_RECONNECT_POLL_US);
        waited += SMB_RECONNECT_POLL_US;
    }

    if (smb_abort_wait) {
        cdvdman_stat.err = SCECdErABRT;
        return 0;
    }

    return 1;
}

void smb_NegotiateProt(OplSmbPwHashFunc_t hash_callback)
{
    ps2ip_init();
    smbHashCallback = hash_callback;
    while (smb_NegotiateProtocol(cdvdman_settings.smb_ip, cdvdman_settings.smb_port, cdvdman_settings.smb_user, cdvdman_settings.smb_password, &ServerCapabilities, hash_callback) <= 0) {
        smb_Disconnect();
        DelayThread(SMB_RECONNECT_INTERVAL_US);
    }

    /* 循环里的那次回调已经把 LM/NT 散列写好。改挂常驻 DES，smbinit 才能卸载。 */
    smbHashCallback = smb_HashPasswordCached;
}

void DeviceInit(void)
{
    /* 重连不再另开线程。CreateThread 的栈从堆顶划 4KB，存档加载 mcman 时就差这块。 */
    RegisterLibraryEntries(&_exp_oplsmb);
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
    smbReconnectEnabled = 1;
    smbIdleTicks = 0;
    smb_abort_wait = 0;
    smb_retry_link = 0;
    if (smbOpenGame() > 0) {
        smbConnectionState = 1;
    } else {
        smb_Disconnect();
        smbConnectionState = 0;
    }
    smb_arm_link_alarm();
    if (smbConnectionState != 1) {
        smb_link_service = 1;
        cdvdman_kick_rthread(0);
    }
}

void DeviceLock(void)
{
    WaitSema(smb_io_sema);
}

void DeviceUnmount(void)
{
    smbReconnectEnabled = 0;
    smb_link_service = 0;
    smb_disarm_link_alarm();
    smbIdleTicks = 0;
    if (smbConnectionState == 1)
        smb_CloseAll();
    smbConnectionState = 2;
    smb_Disconnect();
}

void DeviceStop(void)
{
}

void DeviceAbortReadWait(void)
{
    /* 游戏已经不要这次读。不能在这里 close：lwip_close 会在 recv 仍停在 WaitSema 时释放邮箱。 */
    smb_abort_wait = 1;
}

void DeviceClearAbortReadWait(void)
{
    smb_abort_wait = 0;
}

int DeviceReadSectors(u32 lsn, void *buffer, unsigned int sectors)
{
    register u32 r, sectors_to_read, lbound, ubound, nlsn, offslsn;
    register int i, esc_flag = 0, result, bytes_to_read;
    u8 *p = (u8 *)buffer;
    int rv = SCECdErNO;
    int kicked = 0;
    unsigned int kick_wait = 0;

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
                int action;

                /* Break、停流、Stop、Pause 都在等这次读结束。继续空等的话 sceCdSync 不会返回。 */
                action = smb_read_wait_action(smb_abort_wait, smbReconnectEnabled, smbConnectionState, smb_in_read_thread);
                if (action == SMB_WAIT_ABORT)
                    return SCECdErABRT;

                /* 人已经在读线程里。这里 DelayThread 或重连会把自己堵死，也会把 SMB 叠进深栈。 */
                if (action == SMB_WAIT_RETRY) {
                    smb_retry_link = 1;
                    /* ProbeZSO 不看返回值写 err，这里直接记下，未对齐的循环才能停。 */
                    cdvdman_stat.err = SMB_READ_YIELD;
                    return SMB_READ_YIELD;
                }

                if (action == SMB_WAIT_KICK) {
                    /* 闹钟还在就只踢一次。闹钟丢了才按 2 秒再踢，避免 100ms 一轮把服务器打满。 */
                    if (smb_should_kick_link(kicked, smb_alarm_armed, kick_wait)) {
                        smb_link_service = 1;
                        cdvdman_kick_rthread(0);
                        kicked = 1;
                        kick_wait = 0;
                    }
                    DelayThread(SMB_RECONNECT_POLL_US);
                    kick_wait += SMB_RECONNECT_POLL_US;
                    continue;
                }

                if (!smbReconnectEnabled) {
                    result = -1;
                    break;
                }

                result = smb_ReadCD(offslsn, sectors_to_read, &p[r], i);
                if (result >= 0) {
                    smbIdleTicks = 0;
                    break;
                }

                if (smb_abort_wait)
                    return SCECdErABRT;

                smbIdleTicks = 0;

                // 只在真实读失败后查询物理链路，避免后台轮询干扰正常流式读取。
                if (pNetManGetGlobalNetIFLinkState && !pNetManGetGlobalNetIFLinkState() && !smbTrayOpen) {
                    smbTrayOpen = 1;
                    sceCdTrayReq(SCECdTrayOpen, NULL);
                }

                // 逻辑断线只触发静默重连，当前读取等待连接恢复后再重试。
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
