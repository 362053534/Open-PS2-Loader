/*
  Copyright 2009-2010, jimmikaelkael
  Licenced under Academic Free License version 3.0
  Review Open PS2 Loader README & LICENSE files for further details.
*/

#include "smstcpip.h"
#include "internal.h"

#include "device.h"

/* 空闲保活的拍长。60 拍就是 120 秒，只在没有真实读盘时累加。 */
#define SMB_ECHO_INTERVAL_US  2000000
#define SMB_ECHO_IDLE_TICKS   60
/* 闹钟 2 秒一拍。已经放弃时第一拍叫醒，否则数满 15 拍，仍是 30 秒。 */
#define SMB_ABORT_ALARM_LIMIT 1
#define SMB_BLOCK_ALARM_LIMIT 15

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
static volatile unsigned int smbIdleTicks;
static volatile int smb_echo_enabled;
static volatile int smb_echo_due;
static volatile int smb_echo_busy;
static volatile int smb_in_read;
static volatile int smb_abort_wait;
static volatile int smb_alarm_armed;
static volatile int smb_block_alarms;
static volatile unsigned int smb_block_gen_seen;
static volatile unsigned int smb_block_poked_gen;
static iop_sys_clock_t smb_alarm_clock;

static int smbOpenGame(void);
static unsigned int smb_echo_alarm_cb(void *arg);

/* 返回 1 才发 Echo。正在读，或还没满 120 秒，都不发。 */
static int smb_echo_should_service(int in_read, unsigned int idle_ticks)
{
    if (in_read)
        return 0;
    if (idle_ticks < SMB_ECHO_IDLE_TICKS)
        return 0;
    return 1;
}

/* 返回 1 才叫醒卡在收包里的读线程。同一轮收包只叫一次。 */
static int smb_alarm_should_wake(int blocking, int abort_wait, int alarms, int already_poked)
{
    if (!blocking || already_poked || alarms <= 0)
        return 0;
    if (abort_wait)
        return alarms >= SMB_ABORT_ALARM_LIMIT;
    return alarms >= SMB_BLOCK_ALARM_LIMIT;
}

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

/* 非 0 表示内核按这个系统时钟把同一只闹钟续上。再 iSetAlarm 会多挂一只。 */
static unsigned int smb_alarm_interval(void)
{
    if (smb_alarm_clock.lo != 0)
        return smb_alarm_clock.lo;
    if (smb_alarm_clock.hi != 0)
        return 1;
    return 1;
}

static void smb_alarm_poke_blocked(void)
{
    unsigned int gen;
    int tid;

    gen = smb_read_block_gen();
    if (gen != smb_block_gen_seen) {
        smb_block_gen_seen = gen;
        smb_block_alarms = 0;
    }
    smb_block_alarms++;
    if (!smb_alarm_should_wake(1, smb_abort_wait, smb_block_alarms, smb_block_poked_gen == gen))
        return;

    tid = cdvdman_read_thread_id();
    if (tid <= 0)
        return;

    /* 闹钟在定时器中断里。这里只能用 iReleaseWaitThread，不能 close。 */
    if (iReleaseWaitThread(tid) == 0)
        smb_block_poked_gen = gen;
}

static unsigned int smb_echo_alarm_cb(void *arg)
{
    (void)arg;
    smb_alarm_armed = 0;

    if (!smb_echo_enabled)
        return 0;

    /* 读或 Echo 还没结束。不要把读线程叫去发 Echo，空闲也不记。 */
    if (smb_in_read || smb_echo_busy || smb_echo_due) {
        if (smb_read_is_blocking())
            smb_alarm_poke_blocked();
        smb_alarm_armed = 1;
        return smb_alarm_interval();
    }

    if (smb_read_is_blocking()) {
        smb_alarm_poke_blocked();
        smb_alarm_armed = 1;
        return smb_alarm_interval();
    }

    smb_block_alarms = 0;
    smb_block_gen_seen = 0;

    if (smbIdleTicks < SMB_ECHO_IDLE_TICKS)
        smbIdleTicks++;

    if (!smb_echo_should_service(0, smbIdleTicks)) {
        smb_alarm_armed = 1;
        return smb_alarm_interval();
    }

    smb_echo_due = 1;
    cdvdman_kick_rthread(1);
    /* 返回 0：这一拍已经从队列摘掉。下一拍由读线程 SetAlarm。 */
    return 0;
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
    if (smb_echo_enabled && !smb_alarm_armed)
        smb_arm_echo_alarm();

    if (read_busy) {
        smb_echo_due = 0;
        return 0;
    }

    if (!smb_echo_due)
        return 1;

    smb_echo_busy = 1;
    smb_echo_due = 0;
    /* 失败也不拆套接字、不再协商。下一轮空闲满了再试一次。 */
    smb_Echo();
    smbIdleTicks = 0;
    smb_echo_busy = 0;
    return 1;
}

void smb_NegotiateProt(OplSmbPwHashFunc_t hash_callback)
{
    ps2ip_init();
    /* smbinit 还在。连不上就拆掉再试。游戏起来之后不再走这里。 */
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
    return SCECdComplete;
}

int DeviceFSInit(void)
{
    smbIdleTicks = 0;
    smb_echo_due = 0;
    smb_echo_busy = 0;
    smb_in_read = 0;
    smb_abort_wait = 0;
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

    lbound = 0;
    ubound = (cdvdman_settings.common.NumParts > 1) ? 0x80000 : 0xFFFFFFFF;
    offslsn = lsn;
    r = nlsn = 0;
    sectors_to_read = sectors;
    /* 读的整段都算有流量。闹钟不要在收包途中把读线程叫去发 Echo。 */
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
            if (smb_abort_wait) {
                rv = SCECdErABRT;
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
