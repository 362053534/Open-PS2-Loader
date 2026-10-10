#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
SMB FMV 杂音排查：向 cdvdman 注入“读盘节奏”观测点。

设计要点
--------
* 与代码树无关：同一份脚本可以作用在“有杂音的当前树(6254970)”和“无杂音的基线树(edf39ddc)”
  上 —— 只依赖两棵树中完全相同的代码形状（sceCdRead / cdvdman_read / DeviceReadSectors
  的短读补零 / smb_ReadAndX 的返回长度）。这样两份日志的字段名、单位、触发条件完全一致，
  可以直接逐行做差。

* 三种剖面（由传给 cdvdman 的宏决定）：
    FULL    = -D__IOPCORE_DEBUG  逐事件 printf。已知不可用：每条 printf 都是一次
              WaitSema + 阻塞式 UDP 广播 sendto，FMV 下每秒上千次，本身就会把音频拖到
              欠载产生杂音 —— 连基线版本都会“被杂音”。保留仅为历史对照。
    QUIET   = -DDIAG_OBSERVE=1 + 走 IOP stdout/udptty。cdvdman 不带 __IOPCORE_DEBUG，
              但 OPL 的 INGAME_DEBUG 构建链会做两件严重改变时序的事：
                (a) 加载 udptty-ingame.irx（KPRTTY：连 IOP 内核的 kprintf 都会被
                    一个 priority-8 线程搬到 UDP 上）+ ioptrap.irx；
                (b) SMSTCPIP_INGAME_CFLAGS 被清空（Makefile:198），游戏内的 SMSTCPIP
                    不再是 INGAME_DRIVER 版本 —— PBUF_POOL 25(vs 8)、TCP_WND
                    32768(vs 10240)、软件校验和全开。这是换了一套 TCP/IP 栈，不是
                    “release + 打印”。实测 BASE-QUIET 也会出现杂音。
    OBSERVE = -DDIAG_OBSERVE=1 且整机构建 stay release：
                * 不加载 udptty / ioptrap / ps2link，EE 侧无 __DEBUG 打印；
                * 游戏内 SMSTCPIP 仍是 INGAME_DRIVER=1（只额外打开 UDP 与一个 netconn，
                  TCP 行为与 release 逐字节一致）；
                * 日志不经过 printf/tty，由 diag_net.c 直接调 ps2ip 的 lwip_sendto
                  单播到 SMB 服务器 IP:18194；
                * 不逐事件打印：每 500ms 或 64 次读聚合发一个约 110 字节的报文
                  （稳态 2 包/秒），只有“短读补零/服务器短回”这种直接损坏数据的事件
                  才立即上报，且每窗口最多 4 条。

输出的行（OBSERVE 剖面）
------------------------
  CDVDS w=<窗口号> ms=<结束时刻> win=<窗口时长ms> n=<读次数> sec=<扇区数> \
        dur=<平均读耗时>/<最大读耗时> h50/h100/h250/h500=<各档读耗时计数，互斥> \
        gap=<窗口内最大读间隔> zero=<短读补零次数> shrt=<服务器短回次数>
  CDVDZ ms=<now> lsn=<lsn> got=<实际字节> want=<请求字节>       短读：尾部被静默补零
  CDVDSH ms=<now> off=<偏移> want=<请求字节> got=<实际字节>      服务器回包少于请求

用法： python3 tools/smb-diag/instrument.py [源码树根目录]
"""

import io
import os
import sys

CDVDMAN = "modules/iopcore/cdvdman"
LWIP = "modules/network/SMSTCPIP"

STAMP_H = r'''/*
 * 读盘节奏观测点（诊断构建专用；由 tools/smb-diag/instrument.py 注入）。
 *
 * 两套实现，编译期二选一：
 *   __IOPCORE_DEBUG -> FULL：逐事件 printf（历史剖面，已知自带杂音）
 *   DIAG_OBSERVE    -> OBSERVE：聚合统计 + diag_net.c 直发 UDP（推荐）
 * 两者都没有时全部展开为空，release 构建编译结果与未注入时完全一致。
 */
#ifndef DIAG_STAMP_H
#define DIAG_STAMP_H

#if defined(__IOPCORE_DEBUG)

#include <thbase.h>
#include <stdio.h>

#define DIAG_GAP_MS   50  /* 相邻两次 sceCdRead 的间隔超过该值即视为"读节奏空洞" */
#define DIAG_SLOW_MS  100 /* 单次读盘耗时超过该值即打印 */
#define DIAG_N_MASK   255 /* 每 (DIAG_N_MASK+1) 次读打印一行存活/速率统计 */

static inline unsigned int diagNowMs(void)
{
    iop_sys_clock_t now;
    u32 sec, usec;

    GetSystemTime(&now);
    SysClock2USec(&now, &sec, &usec);

    return sec * 1000 + usec / 1000;
}

/* 状态量定义在 cdvdman.c 中，其余文件通过 extern 引用。 */
extern unsigned int diagLastReadMs, diagReadCount, diagGapCount;

/* 读节奏空洞 + 存活心跳（放在 sceCdRead 入口）。 */
#define DIAG_GAP(lsn, nsectors)                                                          \
    do {                                                                                 \
        unsigned int __now = diagNowMs();                                                \
        if (diagLastReadMs) {                                                            \
            unsigned int __gap = __now - diagLastReadMs;                                 \
            if (__gap >= DIAG_GAP_MS) {                                                  \
                diagGapCount++;                                                          \
                printf("CDVD GAP ms=%u gap=%u lsn=%u n=%u\n", __now, __gap,              \
                       (unsigned int)(lsn), (unsigned int)(nsectors));                   \
            }                                                                            \
        }                                                                                \
        diagLastReadMs = __now;                                                          \
        if ((++diagReadCount & DIAG_N_MASK) == 0)                                        \
            printf("CDVD N ms=%u n=%u gaps=%u\n", __now, diagReadCount, diagGapCount);   \
    } while (0)

/* 单次读盘耗时超标告警（包裹 cdvdman_read 的整体耗时）。 */
#define DIAG_T0(var) unsigned int var = diagNowMs()
#define DIAG_SLOW(var, lsn, nsectors, buf)                                               \
    do {                                                                                 \
        unsigned int __dur = diagNowMs() - (var);                                        \
        if (__dur >= DIAG_SLOW_MS)                                                       \
            printf("CDVD SLOW ms=%u dur=%u lsn=%u sec=%u buf=%p\n", (var), __dur,        \
                   (unsigned int)(lsn), (unsigned int)(nsectors), (buf));                \
    } while (0)

/* 短读补零：请求到的字节数不足，尾部被 memset 填 0 —— 解码侧直接表现为花屏/杂音。 */
#define DIAG_ZERO(lsn, got, want, buf)                                                   \
    printf("CDVD ZERO ms=%u lsn=%u got=%d want=%d buf=%p\n", diagNowMs(),                \
           (unsigned int)(lsn), (int)(got), (int)(want), (buf))

/* 服务器回包数据长度少于请求长度。 */
#define DIAG_SHORT(off, want, got)                                                       \
    printf("CDVD SHORT ms=%u off=%u want=%d got=%d\n", diagNowMs(),                      \
           (unsigned int)(off), (int)(want), (int)(got))

#elif defined(DIAG_OBSERVE) && defined(SMB_DRIVER)

/* OBSERVE：所有统计/发送都在 diag_net.c 里，这里只留下调用点。
   注意 SMB_DRIVER 这一半条件：CDVDMAN_DEBUG_FLAGS 会传给全部 5 个 cdvdman 变体
   （BDM / BDM_ATA / SMB / HDD / HDPRO），只有 SMB 变体才有 network/common 头文件
   与 cdvdman_settings.smb_ip，其余变体必须展开为空，否则编不过。 */

unsigned int diag_now_ms(void);
void diag_read_tick(unsigned int lsn, unsigned int sectors);
void diag_read_done(unsigned int t0, unsigned int lsn, unsigned int sectors);
void diag_zero(unsigned int lsn, int got, int want);
void diag_short(unsigned int off, int want, int got);

#define DIAG_GAP(lsn, nsectors)            diag_read_tick((unsigned int)(lsn), (unsigned int)(nsectors))
#define DIAG_T0(var)                       unsigned int var = diag_now_ms()
#define DIAG_SLOW(var, lsn, nsectors, buf) diag_read_done((var), (unsigned int)(lsn), (unsigned int)(nsectors))
#define DIAG_ZERO(lsn, got, want, buf)     diag_zero((unsigned int)(lsn), (int)(got), (int)(want))
#define DIAG_SHORT(off, want, got)         diag_short((unsigned int)(off), (int)(want), (int)(got))

#else /* 非诊断构建：全部展开为空 */

#define DIAG_GAP(lsn, nsectors)
#define DIAG_T0(var)
#define DIAG_SLOW(var, lsn, nsectors, buf)
#define DIAG_ZERO(lsn, got, want, buf)
#define DIAG_SHORT(off, want, got)

#endif /* __IOPCORE_DEBUG || DIAG_OBSERVE */

#endif /* DIAG_STAMP_H */
'''

# 状态量定义（注入到 cdvdman.c 顶部，只有 FULL 剖面需要）
STATE_DEFS = r'''
#if defined(__IOPCORE_DEBUG)
unsigned int diagLastReadMs, diagReadCount, diagGapCount;
#endif
'''

# OBSERVE 剖面的实现：聚合统计 + 直发 UDP。
DIAG_NET_C = r'''/*
 * 读盘节奏观测：聚合统计 + 直发 UDP（由 tools/smb-diag/instrument.py 生成）。
 *
 * 为什么不走 printf/udptty（v4/v6 的血泪教训）：
 *   tty_write() = WaitSema(tty_sema) + lwip_sendto(255.255.255.255:18194)，
 *   每一次都是阻塞式发送；udptty-ingame 还开了 KPRTTY，连 IOP 内核的 kprintf 都会被
 *   一个 priority-8 线程搬到 UDP 上。FMV 下这套通道足以把音频拖到欠载 —— 实测连基线
 *   edf39ddc 的诊断版都会出现杂音，A/B 因此完全失真。
 *
 * 所以这里：
 *   * 直接调 ps2ip 导出表里的 lwip_sendto，单播到 SMB 服务器 IP:18194，
 *     不经过 tty、不 WaitSema、不广播；
 *   * 不做逐事件打印，每 DIAG_WINDOW_MS 毫秒或 DIAG_WINDOW_READS 次读聚合发一包，
 *     稳态 2 包/秒、约 110 字节；
 *   * 只有"短读补零 / 服务器短回"这种直接损坏数据的事件才立即上报，每窗口最多
 *     DIAG_EVENT_CAP 条。
 */

#include "internal.h"

/* CDVDMAN_DEBUG_FLAGS 会传给全部 5 个 cdvdman 变体，只有 SMB 变体才带
   -DSMB_DRIVER 与 -I../../network/common；其余变体本文件编译成一个空目标。 */
#if defined(DIAG_OBSERVE) && defined(SMB_DRIVER)

#include "smstcpip-common.h"

#define DIAG_DEST_PORT   18194
#define DIAG_WINDOW_MS   500
#define DIAG_WINDOW_READS 64
#define DIAG_EVENT_CAP   4

/* ps2ip 导出表下标（与 device-smb.c 同一套）。 */
static int (*plwip_socket)(int domain, int type, int protocol);                                                        /* #13 */
static int (*plwip_sendto)(int s, void *dataptr, int size, unsigned int flags, struct sockaddr *to, socklen_t tolen);  /* #12 */
static u32 (*pinet_addr)(const char *cp);                                                                             /* #24 */

static int diagSock = -1;
static int diagResolved;

static unsigned int diagLastMs;
static unsigned int diagWinStartMs;
static unsigned int diagWin;
static unsigned int diagReads, diagSectors, diagDurSum, diagDurMax;
static unsigned int diagH50, diagH100, diagH250, diagH500;
static unsigned int diagGapMax;
static unsigned int diagZeroes, diagShorts;
static unsigned int diagEvents;

unsigned int diag_now_ms(void)
{
    iop_sys_clock_t now;
    u32 sec, usec;

    GetSystemTime(&now);
    SysClock2USec(&now, &sec, &usec);

    return sec * 1000 + usec / 1000;
}

static void diag_resolve(void)
{
    modinfo_t info;

    if (diagResolved)
        return;
    diagResolved = 1;

    if (getModInfo("ps2ip\0\0\0", &info)) {
        plwip_socket = info.exports[13];
        plwip_sendto = info.exports[12];
        pinet_addr = info.exports[24];
    }
}

static void diag_send(const char *s)
{
    struct sockaddr_in peer;
    u32 dest;
    int len;

    diag_resolve();
    if (!plwip_socket || !plwip_sendto || !pinet_addr)
        return;

    dest = pinet_addr(cdvdman_settings.smb_ip);
    if (!dest)
        return;
    if (dest == 0xFFFFFFFFu)
        dest = 0xFFFFFFFFu; /* 拿不到服务器 IP 就退回广播 */

    if (diagSock < 0) {
        diagSock = plwip_socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (diagSock < 0)
            return;
    }

    peer.sin_family = AF_INET;
    peer.sin_port = htons(DIAG_DEST_PORT);
    peer.sin_len = sizeof(peer);
    peer.sin_addr.s_addr = dest;

    len = strlen(s);
    plwip_sendto(diagSock, (void *)s, len, 0, (struct sockaddr *)&peer, sizeof(peer));
}

static void diag_flush(unsigned int now)
{
    char b[192];

    if (!diagReads)
        return;

    sprintf(b,
            "CDVDS w=%u ms=%u win=%u n=%u sec=%u dur=%u/%u h50=%u h100=%u h250=%u h500=%u gap=%u zero=%u shrt=%u\n",
            diagWin, now, now - diagWinStartMs, diagReads, diagSectors,
            diagDurSum / diagReads, diagDurMax,
            diagH50, diagH100, diagH250, diagH500, diagGapMax, diagZeroes, diagShorts);
    diag_send(b);

    diagWin++;
    diagWinStartMs = now;
    diagReads = 0;
    diagSectors = 0;
    diagDurSum = 0;
    diagDurMax = 0;
    diagH50 = diagH100 = diagH250 = diagH500 = 0;
    diagGapMax = 0;
    diagZeroes = 0;
    diagShorts = 0;
    diagEvents = 0;
}

/* sceCdRead 入口：统计读节奏（间隔）+ 驱动窗口刷新。 */
void diag_read_tick(unsigned int lsn, unsigned int sectors)
{
    unsigned int now = diag_now_ms();

    (void)lsn;

    if (diagLastMs) {
        unsigned int gap = now - diagLastMs;
        if (gap > diagGapMax)
            diagGapMax = gap;
    }
    diagLastMs = now;

    if (!diagWinStartMs)
        diagWinStartMs = now;

    diagReads++;
    diagSectors += sectors;

    if (diagReads >= DIAG_WINDOW_READS || (now - diagWinStartMs) >= DIAG_WINDOW_MS)
        diag_flush(now);
}

/* cdvdman_read 结束：记录本次读耗时。 */
void diag_read_done(unsigned int t0, unsigned int lsn, unsigned int sectors)
{
    unsigned int dur = diag_now_ms() - t0;

    (void)lsn;
    (void)sectors;

    diagDurSum += dur;
    if (dur > diagDurMax)
        diagDurMax = dur;

    if (dur >= 500)
        diagH500++;
    else if (dur >= 250)
        diagH250++;
    else if (dur >= 100)
        diagH100++;
    else if (dur >= 50)
        diagH50++;
}

/* 短读补零：解码侧直接表现为花屏/杂音，立即上报。 */
void diag_zero(unsigned int lsn, int got, int want)
{
    char b[96];

    diagZeroes++;
    if (diagEvents >= DIAG_EVENT_CAP)
        return;
    diagEvents++;

    sprintf(b, "CDVDZ ms=%u lsn=%u got=%d want=%d\n", diag_now_ms(), lsn, got, want);
    diag_send(b);
}

/* 服务器回包数据长度少于请求长度。 */
void diag_short(unsigned int off, int want, int got)
{
    char b[96];

    diagShorts++;
    if (diagEvents >= DIAG_EVENT_CAP)
        return;
    diagEvents++;

    sprintf(b, "CDVDSH ms=%u off=%u want=%d got=%d\n", diag_now_ms(), off, want, got);
    diag_send(b);
}

#endif /* DIAG_OBSERVE && SMB_DRIVER */

/* 非 SMB 变体编译进来时留下的占位声明，避免空翻译单元。 */
typedef int diag_net_dummy_t;
'''


def read(path):
    with io.open(path, encoding="utf-8") as f:
        return f.read()


def write(path, text):
    with io.open(path, "w", encoding="utf-8") as f:
        f.write(text)


def sub(text, old, new, expected, what):
    """替换并严格校验出现次数，形状对不上就立刻失败，绝不留下半注入的树。"""
    n = text.count(old)
    if n != expected:
        raise SystemExit("FAIL %s: expected %d occurrence(s), found %d" % (what, expected, n))
    return text.replace(old, new)


def add_include(text, anchor, what):
    """在 anchor 之后加入 #include "diag_stamp.h"（幂等）。"""
    if "diag_stamp.h" in text:
        return text
    return sub(text, anchor, anchor + '\n#include "diag_stamp.h"', 1, what)


def patch_lwipopts(root):
    """让 INGAME_DRIVER 版本也能带 UDP（OBSERVE 剖面需要它发日志），
    但 TCP 侧的一切（PBUF_POOL / TCP_WND / 校验和…）保持与 release 逐字节一致。"""
    p = os.path.join(root, LWIP, "include/lwipopts.h")
    t = read(p)

    if "INGAME_DRIVER_UDP" not in t:
        t = sub(t,
                "#ifdef INGAME_DRIVER\n#define LWIP_UDP 0\n#else\n#define LWIP_UDP 1\n#endif\n",
                "#ifdef INGAME_DRIVER\n"
                "#if defined(INGAME_DRIVER_UDP)\n#define LWIP_UDP 1\n"
                "#else\n#define LWIP_UDP 0\n#endif\n"
                "#else\n#define LWIP_UDP 1\n#endif\n",
                1, "lwipopts.h LWIP_UDP")

        t = sub(t,
                "#ifdef INGAME_DRIVER\n#define MEMP_NUM_NETCONN 1\n#endif\n",
                "#ifdef INGAME_DRIVER\n#if defined(INGAME_DRIVER_UDP)\n#define MEMP_NUM_NETCONN 2\n"
                "#else\n#define MEMP_NUM_NETCONN 1\n#endif\n#endif\n",
                1, "lwipopts.h MEMP_NUM_NETCONN")

        t = sub(t,
                "#ifdef INGAME_DRIVER\n#define MEM_SIZE 0x400\n#else\n",
                "#ifdef INGAME_DRIVER\n#if defined(INGAME_DRIVER_UDP)\n#define MEM_SIZE 0x800\n"
                "#else\n#define MEM_SIZE 0x400\n#endif\n#else\n",
                1, "lwipopts.h MEM_SIZE")

        write(p, t)
        print("  patched %s/include/lwipopts.h (INGAME_DRIVER_UDP)" % LWIP)

    p = os.path.join(root, LWIP, "Makefile")
    t = read(p)
    if "INGAME_DRIVER_UDP" not in t:
        t = sub(t,
                "ifeq ($(INGAME_DRIVER),1)\nIOP_CFLAGS += -DINGAME_DRIVER\nendif\n",
                "ifeq ($(INGAME_DRIVER),1)\nIOP_CFLAGS += -DINGAME_DRIVER\nendif\n\n"
                "ifeq ($(INGAME_DRIVER_UDP),1)\nIOP_CFLAGS += -DINGAME_DRIVER_UDP\nendif\n",
                1, "SMSTCPIP/Makefile INGAME_DRIVER_UDP")
        write(p, t)
        print("  patched %s/Makefile (INGAME_DRIVER_UDP)" % LWIP)


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    root = os.path.abspath(root)
    cm = os.path.join(root, CDVDMAN)

    # 1) diag_stamp.h
    write(os.path.join(cm, "diag_stamp.h"), STAMP_H)
    print("  wrote %s/diag_stamp.h" % CDVDMAN)

    # 2) diag_net.c：OBSERVE 剖面的聚合统计 + 直发 UDP
    write(os.path.join(cm, "diag_net.c"), DIAG_NET_C)
    print("  wrote %s/diag_net.c" % CDVDMAN)

    # 3) 游戏内 SMSTCPIP：允许 INGAME_DRIVER + UDP 共存
    patch_lwipopts(root)

    # 4) imports.lst: 时间戳需要 GetSystemTime / SysClock2USec（基线树缺这两个）
    p = os.path.join(cm, "imports.lst")
    t = read(p)
    for sym in ("I_SysClock2USec", "I_GetSystemTime"):
        if sym not in t:
            t = sub(t, "thbase_IMPORTS_start\n", "thbase_IMPORTS_start\n" + sym + "\n", 1,
                    "imports.lst add " + sym)
    write(p, t)
    print("  patched imports.lst (thbase timestamps)")

    # 5) cdvdman.c: 状态量定义（FULL）+ cdvdman_read 的耗时测量
    p = os.path.join(cm, "cdvdman.c")
    t = read(p)
    t = add_include(t, '#include "internal.h"', "cdvdman.c include")
    if "unsigned int diagLastReadMs" not in t:
        t = sub(t, '#include "diag_stamp.h"\n', '#include "diag_stamp.h"\n' + STATE_DEFS, 1,
                "cdvdman.c state defs")
    t = sub(t,
            "static int cdvdman_read(u32 lsn, u32 sectors, u16 sector_size, void *buf)\n{\n"
            "    cdvdman_stat.status = SCECdStatRead;",
            "static int cdvdman_read(u32 lsn, u32 sectors, u16 sector_size, void *buf)\n{\n"
            "    DIAG_T0(diagT0);\n"
            "    cdvdman_stat.status = SCECdStatRead;",
            1, "cdvdman_read entry")
    t = sub(t,
            "    ReadPos = 0; /* Reset the buffer offset indicator. */",
            "    DIAG_SLOW(diagT0, lsn, sectors, buf);\n\n"
            "    ReadPos = 0; /* Reset the buffer offset indicator. */",
            1, "cdvdman_read exit")
    write(p, t)
    print("  patched cdvdman.c (DIAG_T0 / DIAG_SLOW / state)")

    # 6) ncmd.c: sceCdRead 入口的读节奏统计
    p = os.path.join(cm, "ncmd.c")
    t = read(p)
    t = add_include(t, '#include "internal.h"', "ncmd.c include")
    t = sub(t,
            "int sceCdRead(u32 lsn, u32 sectors, void *buf, sceCdRMode *mode)\n{\n"
            "    int result;\n\n    u16 sector_size = 2048;",
            "int sceCdRead(u32 lsn, u32 sectors, void *buf, sceCdRMode *mode)\n{\n"
            "    int result;\n\n    u16 sector_size = 2048;\n\n    DIAG_GAP(lsn, sectors);",
            1, "sceCdRead gap monitor")
    write(p, t)
    print("  patched ncmd.c (DIAG_GAP)")

    # 7) device-smb.c: 短读补零（两棵树的补零语句形状一致；当前树有 2 处，互斥编译）
    p = os.path.join(cm, "device-smb.c")
    t = read(p)
    t = add_include(t, '#include "internal.h"', "device-smb.c include")
    fill = "memset(&p[r + result], 0, bytes_to_read - result);"
    n = t.count(fill)
    if n not in (1, 2):
        raise SystemExit("FAIL device-smb.c: found %d zero-fill sites" % n)
    t = t.replace(fill, "{ DIAG_ZERO(offslsn, result, bytes_to_read, &p[r]); " + fill + " }")
    write(p, t)
    print("  patched device-smb.c (DIAG_ZERO x%d)" % n)

    # 8) cdvdman/Makefile: 新增 DIAG_OBSERVE 开关 + diag_net.o。
    p = os.path.join(cm, "Makefile")
    t = read(p)
    if "DIAG_OBSERVE" not in t:
        t = sub(t,
                "ifeq ($(IOPCORE_DEBUG),1)\nIOP_CFLAGS += -D__IOPCORE_DEBUG\nendif\n",
                "ifeq ($(IOPCORE_DEBUG),1)\nIOP_CFLAGS += -D__IOPCORE_DEBUG\nendif\n\n"
                "ifeq ($(DIAG_OBSERVE),1)\nIOP_CFLAGS += -DDIAG_OBSERVE=1\nIOP_OBJS += diag_net.o\nendif\n",
                1, "cdvdman/Makefile DIAG_OBSERVE")
        write(p, t)
        print("  patched Makefile (DIAG_OBSERVE + diag_net.o)")

    # 9) smb.c: 服务器短回
    p = os.path.join(cm, "smb.c")
    t = read(p)
    t = add_include(t, '#include "smb.h"', "smb.c include")
    t = sub(t,
            "    expected_size = nb_GetSessionMessageLength() + 4;\n"
            "    DataLength = (int)(((u32)RRsp->DataLengthHigh << 16) | RRsp->DataLengthLow);",
            "    expected_size = nb_GetSessionMessageLength() + 4;\n"
            "    DataLength = (int)(((u32)RRsp->DataLengthHigh << 16) | RRsp->DataLengthLow);\n"
            "    if (DataLength < nbytes)\n"
            "        DIAG_SHORT(offsetlow, nbytes, DataLength);",
            1, "smb.c short reply")
    write(p, t)
    print("  patched smb.c (DIAG_SHORT)")

    print("instrumentation OK ->", root)


if __name__ == "__main__":
    main()
