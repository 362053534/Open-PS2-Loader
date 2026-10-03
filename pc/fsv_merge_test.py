#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""在 PC 上验证 cdvdfsv 的「合并小额读」算法（FSV_MERGE_SMALLREAD）。

做法：把 modules/iopcore/cdvdfsv/ncmd.c 里那两段真实代码原样抠出来，配上
模拟的 sceCdRead/sceCdSync（含 device-bdm 的"越界补零"行为），跑一遍游戏
实际的读盘序列（汉化版 1+64+64+63 / 原版 3+64+64+61，264 KB/轮），检查：

  1. 交付给调用者的数据永远等于介质上对应 LSN 的内容；
  2. 不做非顺序读时，carry 不会串数据（插入随机跳读再回流的用例）；
  3. 每一笔发往设备的读都不超过 FSV_MERGE_SECTORS 扇区、也不越过缓冲；
  4. 统计设备读笔数/扇区数，看合并到底省了多少笔（USB/BOT 每笔都有固定开销）。

用法：python3 pc/fsv_merge_test.py            # 默认测 N=8 / 16 / 32
      python3 pc/fsv_merge_test.py 8           # 只测 N=8
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NCMD_C = os.path.join(ROOT, "modules", "iopcore", "cdvdfsv", "ncmd.c")


def extract_code():
    """从 ncmd.c 抠出 #ifdef FSV_MERGE_SMALLREAD ... fsv_read_sectors() 结束。"""
    src = open(NCMD_C, encoding="utf-8").read()
    start = src.index("#ifdef FSV_MERGE_SMALLREAD")
    end = src.index("//--------------------------------------------------------------", start)
    block = src[start:end]
    if "fsv_read_sectors" not in block:
        raise SystemExit("没找到 fsv_read_sectors（ncmd.c 结构变了？）")
    return block


HARNESS = r"""
/* 由 pc/fsv_merge_test.py 生成：真实代码 + 模拟设备 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;

#define CDVDMAN_FS_SECTORS 8
#define FSV_MERGE_SMALLREAD {N}

/* ---- 模拟介质（内容与 LSN 相关，便于逐字节校验） ---- */
#define MEDIA_SECTORS 3000
static u8 g_media[MEDIA_SECTORS * 2048];

static int g_reads, g_sectors, g_maxsize, g_overrun;

static int sceCdRead(u32 lsn, u32 sectors, void *buf, void *mode)
{
    u32 i;
    (void)mode;
    g_reads++;
    g_sectors += sectors;
    if ((int)sectors > g_maxsize) g_maxsize = sectors;
    if (sectors > FSV_MERGE_SMALLREAD) {
        printf("!! 设备读 %u 扇区 > 缓冲 %d\n", sectors, FSV_MERGE_SMALLREAD);
        g_overrun++;
    }
    for (i = 0; i < (u32)sectors; i++) {
        u8 *dst = (u8 *)buf + i * 2048;
        if (lsn + i < MEDIA_SECTORS)
            memcpy(dst, g_media + (lsn + i) * 2048, 2048);
        else /* device-bdm 的 ZERO-FILL：越界补零并返回成功 */
            memset(dst, 0, 2048);
    }
    return 1;
}

static int sceCdSync(int mode) { (void)mode; return 0; }

/* ======== 以下为 ncmd.c 里原样抠出来的代码 ======== */
{CODE}
/* ======== 抠出部分结束 ======== */

static int g_fail;
static u8 g_chunk[8 * 2048]; /* 对应 cdvdfsv 的 fsvRbuf（8 扇区） */

static void expect(const char *what, u32 lsn, u32 sectors)
{
    /* 把刚才交付的 g_chunk 与介质比对 */
    u32 i;
    for (i = 0; i < sectors * 2048; i++) {
        u8 want = (lsn * 2048 + i < (u32)MEDIA_SECTORS * 2048) ?
                  g_media[lsn * 2048 + i] : 0;
        if (g_chunk[i] != want) {
            printf("!! %s: lsn=%u 扇区内第 %u 字节不符 (got %02x want %02x)\n",
                   what, lsn, i, g_chunk[i], want);
            g_fail++;
            return;
        }
    }
}

/* 按 cdvd_readee 的方式切块：每次最多 CDVDMAN_FS_SECTORS 个扇区 */
static void do_request(u32 *lsn, u32 sectors, int pattern)
{
    u32 rem = sectors;
    while (rem) {
        u32 temp = (rem < CDVDMAN_FS_SECTORS) ? rem : CDVDMAN_FS_SECTORS;
        /* pattern=1 时模拟"未对齐 EE 缓冲"路径：nsectors+1 */
        fsv_read_sectors(*lsn, temp, g_chunk, 2048);
        expect("顺序读", *lsn, temp);
        *lsn += temp;
        rem -= temp;
    }
}

int main(void)
{
    u32 i, lsn;
    int r, q, rounds = 6;
    /* 汉化版：1+64+64+63 = 192 扇区/轮；原版：3+64+64+61 */
    u32 cn[4] = {1, 64, 64, 63};
    u32 jp[4] = {3, 64, 64, 61};

    for (i = 0; i < (u32)MEDIA_SECTORS * 2048; i++)
        g_media[i] = (u8)((i * 7 + (i >> 9) * 13) & 0xff);

    printf("== N=%d ==\n", (int)FSV_MERGE_SMALLREAD);

    /* 用例 1：汉化版顺序流 */
    lsn = 100; g_reads = g_sectors = g_maxsize = 0;
    for (r = 0; r < rounds; r++)
        for (q = 0; q < 4; q++)
            do_request(&lsn, cn[q], 0);
    printf("  汉化版 %d 轮: 设备读 %d 笔, %d 扇区, 最大单笔 %d 扇区（原文 25 笔/轮，192 扇区/轮）\n",
           rounds, g_reads, g_sectors, g_maxsize);

    /* 用例 2：原版顺序流 */
    lsn = 500; g_reads = g_sectors = g_maxsize = 0;
    for (r = 0; r < rounds; r++)
        for (q = 0; q < 4; q++)
            do_request(&lsn, jp[q], 0);
    printf("  原版   %d 轮: 设备读 %d 笔, %d 扇区, 最大单笔 %d 扇区\n",
           rounds, g_reads, g_sectors, g_maxsize);

    /* 用例 3：顺序流中间插入乱序小额读，再回到顺序流 */
    lsn = 900; g_reads = g_sectors = g_maxsize = 0;
    do_request(&lsn, 64, 0);
    { /* 乱序：回到很前面的位置读 2 个扇区 */
        u32 jump = 300;
        fsv_read_sectors(jump, 2, g_chunk, 2048);
        expect("跳读", jump, 2);
    }
    do_request(&lsn, 64, 0);
    printf("  乱序插入: 设备读 %d 笔, 最大单笔 %d 扇区（数据校验通过）\n", g_reads, g_maxsize);

    /* 用例 4：未对齐 EE 缓冲的路径（cdv_readee 会一次读 nsectors+1 个扇区） */
    lsn = 1200; g_reads = g_sectors = g_maxsize = 0;
    {
        u32 rem = 64;
        while (rem) {
            u32 ns = (rem < CDVDMAN_FS_SECTORS - 1) ? rem : CDVDMAN_FS_SECTORS - 1;
            u32 temp = ns + 1;
            fsv_read_sectors(lsn, temp, g_chunk, 2048);
            expect("未对齐路径", lsn, temp);
            lsn += ns;
            rem -= ns;
        }
    }
    printf("  未对齐路径: 设备读 %d 笔, 最大单笔 %d 扇区（数据校验通过）\n", g_reads, g_maxsize);

    /* 用例 5：非 2048 字节扇区（raw 模式）必须绕过合并，不做任何多读 */
    lsn = 1500; g_reads = g_sectors = g_maxsize = 0;
    fsv_read_sectors(lsn, 3, g_chunk, 2328);
    expect("raw 模式", lsn, 3);
    printf("  raw(2328) 单笔: 设备读 %d 笔, %d 扇区, 最大单笔 %d 扇区（应为 1/3/3）\n",
           g_reads, g_sectors, g_maxsize);

    /* 用例 6：读到介质末尾之后（模拟 device-bdm 补零） */
    lsn = MEDIA_SECTORS - 2; g_reads = g_sectors = g_maxsize = 0;
    do_request(&lsn, 8, 0);
    do_request(&lsn, 8, 0);
    printf("  末尾越界: 设备读 %d 笔, %d 扇区（其中越界补零，未崩溃）\n", g_reads, g_sectors);

    if (g_fail || g_overrun) {
        printf("结果: FAIL (%d 处数据错误, %d 处超缓冲)\n", g_fail, g_overrun);
        return 1;
    }
    printf("结果: PASS\n");
    return 0;
}
"""


def run(n):
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, "sim.c")
        code = extract_code().replace("FSV_MERGE_SMALLREAD", "FSV_MERGE_SMALLREAD")
        open(src, "w", encoding="utf-8").write(
            HARNESS.replace("{N}", str(n)).replace("{CODE}", code))
        exe = os.path.join(td, "sim")
        r = subprocess.run(["gcc", "-O1", "-Wall", "-Wno-unused-variable",
                            "-o", exe, src], capture_output=True, text=True)
        if r.returncode:
            print(r.stderr)
            return 1
        return subprocess.run([exe]).returncode


if __name__ == "__main__":
    ns = [int(a) for a in sys.argv[1:]] or [8, 16, 32]
    rc = 0
    for n in ns:
        rc |= run(n)
    sys.exit(rc)
