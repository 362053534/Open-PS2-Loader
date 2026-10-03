#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 PCSX2 日志里量出 FMV 的真实数据率（码率）。

用法：
    python3 pc/ps2_log_fmv.py 原版日志.txt 汉化版日志.txt
    python3 pc/ps2_log_fmv.py 汉化版日志.txt --op-seconds 90      # 你秒表量的播放时长，用于校验
    python3 pc/ps2_log_fmv.py 日志.txt --detail                     # 打印每秒速率曲线

原理：
    游戏播 FMV 时是按块顺序读盘的，且缓冲有限 => 长时间平均读取速率 ≈ 视频的真实数据率。
    所以只要日志覆盖「整段 FMV」，就能算出：
      码率 = 读到的总字节 / 从第一笔读到最后两笔读的时间
    并且能看出峰值（开头有没有高码率段落）、稳态、轮询结构（每轮 192 扇区）、有没有重复读。

日志要求（重要）：
    1. PCSX2 控制台日志带时间戳（形如 7145.1060 DvdRead 574044 (001)）；
    2. 速度保持 100%（不要快进/加速/跳帧），关掉 Fast CDVD 之类影响读盘时序的 hack；
    3. 从 OP 开始之前一直录到 OP 结束、读盘停止之后再停几秒。
"""

import argparse
import os
import re
import sys
from collections import Counter

SECTOR = 2048

# 常见两种日志格式
RE_TS = re.compile(r'(?<![\d.])(\d+(?:\.\d+)?)\s+DvdRead\s+(\d+)\s+\((\d+)\)')
RE_VERBOSE = re.compile(r'(?<![\d.])(\d+(?:\.\d+)?)[^\n]*?DvdRead:\s*Reading Sector\s+(\d+)\s+\((\d+)\s*Blocks')
RE_NO_TS = re.compile(r'DvdRead\s+(\d+)\s+\((\d+)\)')


def parse_log(path):
    """返回 (reads, 有无时间戳)。reads = [(t, lba, blocks), ...]，按出现顺序。"""
    reads = []
    with open(path, 'r', errors='replace') as f:
        for line in f:
            m = RE_TS.search(line) or RE_VERBOSE.search(line)
            if m:
                reads.append((float(m.group(1)), int(m.group(2)), int(m.group(3))))
                continue
            m = RE_NO_TS.search(line)
            if m:
                reads.append((None, int(m.group(1)), int(m.group(2))))
    reads.sort(key=lambda r: (r[0] if r[0] is not None else 0.0))
    return reads, reads and reads[0][0] is not None


def split_runs(reads, gap=3.0):
    """按时间间隔切分成若干"连续读段"（OP / 菜单 / 后续加载 会自然分开）。"""
    runs = []
    cur = []
    for r in reads:
        if cur and r[0] is not None and cur[-1][0] is not None and (r[0] - cur[-1][0]) > gap:
            runs.append(cur)
            cur = []
        cur.append(r)
    if cur:
        runs.append(cur)
    return runs


def run_stats(run):
    t0 = run[0][0]
    t1 = run[-1][0]
    sectors = sum(r[2] for r in run)
    return {
        'reads': len(run),
        'sectors': sectors,
        'bytes': sectors * SECTOR,
        't0': t0,
        't1': t1,
        'span': (t1 - t0) if (t0 is not None and t1 is not None) else None,
        'lba0': min(r[1] for r in run),
        'lba1': max(r[1] + r[2] for r in run),
    }


def per_second(run):
    """按秒聚合速率，返回 [(秒序号, KB)]，用于看峰值和稳态。"""
    if run[0][0] is None:
        return []
    seconds = {}
    t0 = run[0][0]
    for t, _lba, blocks in run:
        k = int(t - t0)
        seconds[k] = seconds.get(k, 0) + blocks * SECTOR
    return sorted((k, v / 1024.0) for k, v in seconds.items())


def median(xs):
    xs = sorted(xs)
    if not xs:
        return 0.0
    n = len(xs)
    return xs[n // 2] if n % 2 else (xs[n // 2 - 1] + xs[n // 2]) / 2.0


def round_structure(run, target=192):
    """把读按「累计到 target 扇区」分组，看每轮周期（视频流的取数节奏）。"""
    rounds = []
    acc = 0
    start = None
    for t, _lba, blocks in run:
        if start is None:
            start = t
        acc += blocks
        if acc >= target:
            rounds.append((start, t, acc))
            acc = 0
            start = None
    periods = [rounds[k + 1][0] - rounds[k][0] for k in range(len(rounds) - 1)]
    return rounds, periods


def analyze(path, gap=3.0, target=192, op_seconds=None, detail=False):
    reads, has_ts = parse_log(path)
    print('=' * 78)
    print('日志：%s' % path)
    if not reads:
        print('  没找到任何 DvdRead 行 —— 确认日志里带时间戳、且含读盘记录。')
        return None
    print('  读到 %d 笔 DvdRead%s' % (len(reads), '' if has_ts else '（**没有时间戳，无法算速率**）'))
    if not has_ts:
        return None

    runs = split_runs(reads, gap)
    print('  切成 %d 个连续读段（间隔 > %.1fs 视为新段）' % (len(runs), gap))
    print('  %-4s %-12s %-12s %-10s %-12s %-10s' % ('#', '开始(s)', '结束(s)', '读段时长', '大小(MB)', '平均KB/s'))
    infos = []
    for i, run in enumerate(runs):
        st = run_stats(run)
        rate = (st['bytes'] / st['span'] / 1024.0) if st['span'] and st['span'] > 0 else 0
        infos.append((i, st, rate))
        print('  %-4d %-12.3f %-12.3f %-10s %-12.2f %-10.0f'
              % (i, st['t0'], st['t1'],
                 ('%.3fs' % st['span']) if st['span'] else '-',
                 st['bytes'] / 2 ** 20, rate))

    # 取最大的一段当作 FMV
    best = max(infos, key=lambda x: x[1]['bytes'])
    i, st, rate = best
    print('\n  >>> 取最大的一段（#%d）作为 FMV 分析对象' % i)

    seconds = per_second(runs[i])
    if not seconds:
        return None
    kb = [v for _k, v in seconds]
    peak = max(kb)
    # 稳态：去掉首尾各 10% 的秒，取中位数
    n = len(kb)
    core = kb[int(n * 0.1):max(int(n * 0.9), int(n * 0.1) + 1)] or kb
    steady = median(core)

    rounds, periods = round_structure(runs[i], target)
    print('      读段总大小        : %.2f MB (%d 扇区)' % (st['bytes'] / 2 ** 20, st['sectors']))
    print('      读段时长(日志时间) : %.3f s' % st['span'])
    print('      平均读取速率      : %.0f KB/s  = %.2f MB/s = %.2f Mbps' % (rate, rate / 1024.0, rate * 8 / 1000.0))
    print('      稳态读取速率(中位) : %.0f KB/s  = %.2f Mbps' % (steady, steady * 8 / 1000.0))
    print('      峰值(单秒)         : %.0f KB/s  = %.2f Mbps（首秒 %.0f KB/s）'
          % (peak, peak * 8 / 1000.0, kb[0] if kb else 0))
    if periods:
        print('      %d 扇区/轮         : %d 轮，轮周期中位 %.3f s（min %.3f / max %.3f）'
              % (target, len(rounds), median(periods), min(periods), max(periods)))
        print('      >>>> 每轮 384KB 折算 : %.0f KB/s' % (target * SECTOR / median(periods) / 1024.0 if median(periods) else 0))
    dup = Counter(r[1] for r in runs[i])
    repeats = sum(1 for _l, c in dup.items() if c > 1)
    print('      重复读同一 LBA     : %d 个' % repeats)
    print('      LBA 范围           : %d .. %d' % (st['lba0'], st['lba1']))

    if op_seconds:
        print('      你提供的 OP 时长    : %.1f s ⇒ 需要持续 %.0f KB/s (%.2f Mbps)'
              % (op_seconds, st['bytes'] / op_seconds / 1024.0, st['bytes'] * 8 / op_seconds / 1e6))
        print('        （和上面"平均读取速率"对照：差得多说明日志没覆盖整段/或播放时读盘提前停了）')

    if detail:
        print('      每秒速率(KB): ' + ' '.join('%d:%d' % (k, v) for k, v in seconds))
    return {'name': os.path.basename(path), 'bytes': st['bytes'], 'span': st['span'],
            'rate_kbps': rate, 'steady_kbps': steady, 'peak_kbps': peak,
            'round_period': median(periods) if periods else None, 'rounds': len(rounds),
            'sectors': st['sectors'], 'lba0': st['lba0'], 'lba1': st['lba1'],
            'seconds': seconds}


def compare(a, b):
    if not a or not b:
        return
    print('\n' + '=' * 78)
    print('对比：%s  vs  %s' % (a['name'], b['name']))
    def row(label, va, vb, unit=''):
        ratio = (vb / va) if va else 0
        print('  %-22s %12.3f %-6s %12.3f %-6s   倍数 %.2fx' % (label, va, unit, vb, unit, ratio))
    row('读段大小', a['bytes'] / 2 ** 20, b['bytes'] / 2 ** 20, 'MB')
    row('读段时长', a['span'], b['span'], 's')
    row('平均读取速率', a['rate_kbps'], b['rate_kbps'], 'KB/s')
    row('稳态读取速率', a['steady_kbps'], b['steady_kbps'], 'KB/s')
    row('峰值(单秒)', a['peak_kbps'], b['peak_kbps'], 'KB/s')
    if a['round_period'] and b['round_period']:
        row('轮周期(192扇区)', a['round_period'], b['round_period'], 's')
    print('\n  结论口径：')
    print('    * 稳态 KB/s ≈ 这段视频的真实数据率；')
    print('    * 两边稳态之比 ≈ 码率之比；把它和 PCSX2 日志里的轮周期之比（0.636 / 0.232 ≈ 2.7）对照；')
    print('    * 若两边大小接近但稳态差很多 ⇒ 汉化版那段要么码率真的高，要么时间轴被压短；')
    print('    * 若稳态接近 ⇒ 之前看到的 2.7 倍只是开头峰值/预读，需要另找原因。')


def main():
    ap = argparse.ArgumentParser(description='从 PCSX2 日志量 FMV 数据率')
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--gap', type=float, default=3.0, help='切段间隔（秒），默认 3')
    ap.add_argument('--round', type=int, default=192, help='每轮扇区数，默认 192（日志里是 1+64+64+63）')
    ap.add_argument('--op-seconds', type=float, default=None, help='用秒表量的 OP 播放时长（秒），用于校验')
    ap.add_argument('--detail', action='store_true', help='打印每秒速率')
    args = ap.parse_args()

    results = [analyze(p, args.gap, args.round, args.op_seconds, args.detail) for p in args.logs]
    if len(results) == 2:
        compare(results[0], results[1])
    return 0


if __name__ == '__main__':
    sys.exit(main())
