#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把"限速到 R KB/s"换算成 PCSX2 的光驱时间参数，并对照两版 OP 的实测需求，预测会发生什么。

背景（见 notes/gundam_seed_ce_op_hang_analysis.md）：
  * 原版 / 汉化版 OP 总数据量、总时长、平均速率都一样（47345 扇区 / ~110 s / 0.88 MB/s）；
  * 差别在开头：前 5 轮（约 1 秒）汉化版要 1.932 MB/s，原版只要 0.728 MB/s；
  * 起播时两版都只预读了 193 扇区（0.377 MiB）；
  * PCSX2 默认的等效光驱速率 ≈ 2.21 MB/s（CLV 2x：675 * min(Speed,1.6) 扇区/秒），
    刚好高于汉化版开头的 1.93 MB/s ⇒ 所以模拟器上汉化版也能播过去。

用法：python3 rate_check.py
"""
PSXCLK = 36864000          # PCSX2 里 IOP 时钟
DVD_SECTORS_PER_SECOND = 675   # 1x DVD ≈ 1350 KB/s
PREBUFFER_SECTORS = 193    # 起播前已缓冲的扇区数（实测，两版相同）

# 实测需求（MB/s）：(标签, 扇区数, 秒数, 原版, 汉化版)
WINDOWS = [
    ("前 3 轮", 3 * 192, 0.5, 2.149, 2.142),
    ("前 5 轮", 5 * 192, 1.0, 0.728, 1.932),
    ("前 10 轮", 10 * 192, 3.0, 0.701, 1.267),
    ("全程", 47345, 110.0, 0.917, 0.881),
]

CANDIDATES = [
    # 2048 B/扇区 ⇒ KB/s = 扇区/秒 × 2
    ("PCSX2 默认（2x CLV 上限）", DVD_SECTORS_PER_SECOND * 1.6 * 2),
    ("1x CLV（PCSX2 模型）", DVD_SECTORS_PER_SECOND * 1.0 * 2),
    ("U 盘实测上限（约 1.25 MB/s）", 1.25 * 1048576 / 1024),
    ("USB 1.1 保守值 1 MB/s", 1024.0),
]


def fmt_rate(kb_s):
    return "%.0f KB/s (%.2f MB/s)" % (kb_s, kb_s * 1024 / 1048576.0)


def main():
    print("PCSX2 光驱时间模型：cdvd.ReadTime = PSXCLK / 每秒扇区数；16 扇区读缓冲按该速率补充。\n")
    print("%-28s %-24s %-10s %-12s" % ("等效链路", "每秒扇区", "每扇区", "64 扇区读"))
    for label, kb_s in CANDIDATES:
        sps = kb_s * 1024 / 2048.0
        print("%-28s %-24s %-10s %-12s" % (
            label, "%.0f" % sps, "%.2f ms" % (1000.0 / sps), "%.1f ms" % (64 * 1000.0 / sps)))

    print("\n各窗口需求 vs 链路能力（>100% 表示这个窗口喂不上，会开始吃预读缓冲）：\n")
    header = "%-22s" % "窗口（秒数）"
    for label, kb_s in CANDIDATES:
        header += "%14s" % label.split("（")[0]
    print(header)
    for wlabel, sectors, secs, jp, cn in WINDOWS:
        row = "%-22s" % ("%s / %.1fs" % (wlabel, secs))
        for label, kb_s in CANDIDATES:
            need = max(jp, cn) * 1048576.0 / 1024.0
            row += "%13.0f%%" % (100.0 * need / kb_s)
        print(row)

    print("\n预测（把 PCSX2_CDVD_KBPS 设成对应值后，各跑一次汉化版 + 原版 OP）：\n")
    print("%-28s %-34s %-30s" % ("等效链路", "汉化版", "原版"))
    for label, kb_s in CANDIDATES:
        cn_ok = kb_s >= 1.932 * 1048576 / 1024
        jp_ok = kb_s >= 0.728 * 1048576 / 1024
        cn = "能播（开头需求 %.2f <= %.2f MB/s）" % (1.932, kb_s * 1024 / 1048576) if cn_ok \
            else "应该在开头 1~2 秒内停住（需求 1.93 > %.2f）" % (kb_s * 1024 / 1048576)
        jp = "正常（需求 0.73）" if jp_ok else "可能也卡（需再降）"
        print("%-28s %-34s %-30s" % (label, cn, jp))

    print("\n判定：")
    print("  * 限到 1024~1250 KB/s 后：汉化版复现停住、原版正常  ⇒ H1（开头需求超过链路能力）定案；")
    print("  * 限到 1024~1250 KB/s 后：两版都正常           ⇒ H1 不成立，回头查坏数据/介质（§0.12）；")
    print("  * 两版都卡                                      ⇒ 限得太低，往上调一档再试。")


if __name__ == '__main__':
    main()
