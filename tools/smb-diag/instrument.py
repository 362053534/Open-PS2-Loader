#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
SMB FMV 杂音排查：向 cdvdman 注入"读盘节奏"观测点。

设计要点
--------
* 与代码树无关：同一份脚本可以作用在"有杂音的当前树(6254970)"和"无杂音的基线树(edf39ddc)"
  上 —— 只依赖两棵树中完全相同的代码形状（sceCdRead / cdvdman_read / DeviceReadSectors
  的短读补零 / smb_ReadAndX 的返回长度）。这样两份日志的字段名、单位、触发条件完全一致，
  可以直接逐行做差。
* 稀疏输出：稳态下几乎不打日志（每次 sceCdRead 只有一次 GetSystemTime 的开销），
  只在"异常"时打印 —— 避免诊断版把 IOP 的 printf/udptty 通道打爆导致游戏卡死
  （逐读全量打印 + 每条日志一次阻塞式 UDP sendto，在 FMV 每秒数百次读的节奏下
   足以把整个 IOP 拖死）。
* 两种构建模式：
*   FULL  = cdvdman 带 __IOPCORE_DEBUG（逐读 DPRINTF 全开）——日志详尽，但每条 printf
*           都是一次 WaitSema + 阻塞式 UDP sendto，FMV 下每秒上千次，本身就会把音频拖到
*           欠载产生杂音，"有/无杂音"的 A/B 因此失真（BASE/FULL 实测同样有杂音）。
*   QUIET = cdvdman 不带 __IOPCORE_DEBUG，只靠 -DDIAG_OBSERVE=1 打开本脚本注入的稀疏
*           观测点——时序几乎等同 release，用于做"有杂音 / 无杂音"的对照。

输出的行（全部带毫秒时间戳，走 IOP stdout -> udptty UDP 18194）
-------------------------------------------------------------
  CDVD N    ms=<now> n=<累计读次数> gaps=<累计空洞数>       每 256 次读一行（存活 + 读速率）
  CDVD GAP  ms=<now> gap=<距上次读的间隔ms> lsn=.. n=..     读节奏出现空洞（>= 50ms）
  CDVD SLOW ms=<now> dur=<本次读耗时ms> lsn=.. sec=.. buf=.. 单次读耗时 >= 100ms
  CDVD ZERO ms=<now> lsn=.. got=.. want=.. buf=..           短读：尾部被静默补零（数据被填 0）
  CDVD SHORT ms=<now> off=.. want=.. got=..                 服务器返回字节数少于请求数

用法： python3 tools/smb-diag/instrument.py [源码树根目录]
"""

import io
import os
import sys

CDVDMAN = "modules/iopcore/cdvdman"

STAMP_H = r'''/*
 * 读盘节奏观测点（诊断构建专用；由 tools/smb-diag/instrument.py 注入）。
 * 在 __IOPCORE_DEBUG 或 DIAG_OBSERVE 下展开；两者都没有时全部为空，
 * release 构建编译结果与未注入时完全一致。
 */
#ifndef DIAG_STAMP_H
#define DIAG_STAMP_H

#if defined(__IOPCORE_DEBUG) || defined(DIAG_OBSERVE)

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

#else /* 非调试构建：全部展开为空 */

#define DIAG_GAP(lsn, nsectors)
#define DIAG_T0(var)
#define DIAG_SLOW(var, lsn, nsectors, buf)
#define DIAG_ZERO(lsn, got, want, buf)
#define DIAG_SHORT(off, want, got)

#endif /* __IOPCORE_DEBUG || DIAG_OBSERVE */

#endif /* DIAG_STAMP_H */
'''

# 状态量定义（注入到 cdvdman.c 顶部，与 #include 一起只出现一次）
STATE_DEFS = r'''
#if defined(__IOPCORE_DEBUG) || defined(DIAG_OBSERVE)
unsigned int diagLastReadMs, diagReadCount, diagGapCount;
#endif
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


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    root = os.path.abspath(root)
    cm = os.path.join(root, CDVDMAN)

    # 1) diag_stamp.h
    write(os.path.join(cm, "diag_stamp.h"), STAMP_H)
    print("  wrote %s/diag_stamp.h" % CDVDMAN)

    # 2) imports.lst: 时间戳需要 GetSystemTime / SysClock2USec（基线树缺这两个）
    p = os.path.join(cm, "imports.lst")
    t = read(p)
    for sym in ("I_SysClock2USec", "I_GetSystemTime"):
        if sym not in t:
            t = sub(t, "thbase_IMPORTS_start\n", "thbase_IMPORTS_start\n" + sym + "\n", 1,
                    "imports.lst add " + sym)
    write(p, t)
    print("  patched imports.lst (thbase timestamps)")

    # 3) cdvdman.c: 状态量定义 + cdvdman_read 的耗时测量
    p = os.path.join(cm, "cdvdman.c")
    t = read(p)
    t = add_include(t, '#include "internal.h"', "cdvdman.c include")
    if "diagLastReadMs" in t and "unsigned int diagLastReadMs" not in t:
        pass  # 只 include 了头文件
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

    # 4) ncmd.c: sceCdRead 入口的读节奏空洞检测
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

    # 5) device-smb.c: 短读补零（两棵树的补零语句形状一致；当前树有 2 处，互斥编译）
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

    # 6) cdvdman/Makefile: 新增 DIAG_OBSERVE 开关。
    #    QUIET 构建用 CDVDMAN_DEBUG_FLAGS="DIAG_OBSERVE=1"（不带 IOPCORE_DEBUG=1）——
    #    这样 cdvdman 里所有逐读 DPRINTF 为空（时序与 release 一致），只有本脚本注入的
    #    稀疏观测点会打印。FULL 构建则传 IOPCORE_DEBUG=1 DIAG_OBSERVE=1，日志全量。
    p = os.path.join(cm, "Makefile")
    t = read(p)
    if "DIAG_OBSERVE" not in t:
        t = sub(t,
                "ifeq ($(IOPCORE_DEBUG),1)\nIOP_CFLAGS += -D__IOPCORE_DEBUG\nendif\n",
                "ifeq ($(IOPCORE_DEBUG),1)\nIOP_CFLAGS += -D__IOPCORE_DEBUG\nendif\n\n"
                "ifeq ($(DIAG_OBSERVE),1)\nIOP_CFLAGS += -DDIAG_OBSERVE=1\nendif\n",
                1, "cdvdman/Makefile DIAG_OBSERVE")
        write(p, t)
        print("  patched Makefile (DIAG_OBSERVE)")

    # 7) smb.c: 服务器短回
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
