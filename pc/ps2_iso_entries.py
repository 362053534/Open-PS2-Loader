#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""检查 PS2 ISO 的 ISO9660 结构，并把 AFS 包（例如 MOV.AFS）里的条目列出来。

用途：OPL 排查"汉化版 OP 卡死"这类问题时，需要两件事：
  1. 镜像有没有被重建过（PVD 卷大小和实际文件大小是否一致、文件 LBA 有没有变）；
  2. 目标文件（AFS）里每条视频的大小，和原版对比就能看出汉化版是不是换了高码率视频。

用法：
    python3 ps2_iso_entries.py game.iso                    # 列出根目录 + MOV.AFS 条目表
    python3 ps2_iso_entries.py game.iso --file MOV.AFS
    python3 ps2_iso_entries.py game.iso --lba 574094      # 574094 落在哪个文件/哪一条 AFS 条目里
    python3 ps2_iso_entries.py cn.iso --file MOV.AFS --compare jp.iso
"""

import argparse
import os
import struct
import sys

SECTOR = 2048
AFS_MAGIC = b'AFS\x00'


def be32(b, off):
    return struct.unpack_from('>I', b, off)[0]


def le32(b, off):
    return struct.unpack_from('<I', b, off)[0]


def read_sector(f, lba, count=1):
    f.seek(lba * SECTOR)
    return f.read(SECTOR * count)


def parse_dir_record(rec):
    if len(rec) < 33:
        return None
    length = rec[0]
    if length < 33:
        return None
    lba = le32(rec, 2)
    size = le32(rec, 10)
    flags = rec[25]
    name_len = rec[32]
    name = rec[33:33 + name_len]
    if name in (b'\x00', b'\x01'):
        ident = '.' if name == b'\x00' else '..'
    else:
        try:
            ident = name.decode('ascii', 'replace')
        except Exception:
            ident = repr(name)
    return {'len': length, 'lba': lba, 'size': size, 'dir': bool(flags & 2), 'name': ident}


def list_dir(f, root_lba, root_size):
    """只遍历一层目录；ISO9660 目录是“扇区里一串记录，length 为 0 就是本扇区结束”。"""
    out = []
    data = read_sector(f, root_lba, (root_size + SECTOR - 1) // SECTOR)
    pos = 0
    while pos + 33 <= len(data):
        if data[pos] == 0:
            pos = (pos // SECTOR + 1) * SECTOR  # 跳到下一个扇区
            continue
        rec = parse_dir_record(data[pos:pos + data[pos]])
        if rec is None:
            break
        out.append(rec)
        pos += rec['len']
    return out


def find_file(f, path):
    """在根目录（以及一层子目录）里按名字找文件，返回 dict 或 None。"""
    target = os.path.basename(path).lower()
    pvd = read_sector(f, 16)
    if pvd[1:6] != b'CD001':
        return None
    root = parse_dir_record(pvd[0x9C:0x9C + 34])
    entries = list_dir(f, root['lba'], root['size'])
    for rec in entries:
        if rec['dir']:
            continue
        if rec['name'].split(';')[0].lower() == target:
            return {'lba': rec['lba'], 'size': rec['size'], 'name': rec['name'], 'path': '/' + rec['name']}
    for drec in entries:
        if not drec['dir'] or drec['name'] == '..':
            continue
        for rec in list_dir(f, drec['lba'], drec['size']):
            if rec['dir']:
                continue
            if rec['name'].split(';')[0].lower() == target:
                return {'lba': rec['lba'], 'size': rec['size'], 'name': rec['name'],
                        'path': '/' + drec['name'] + '/' + rec['name']}
    return None


def parse_afs(f, file_lba, file_size):
    """AFS: 头 4 字节是 "AFS\\0"，接着大端条目数，之后每条 8 字节（大端 offset, size，相对 AFS 起点）。"""
    head = read_sector(f, file_lba)
    if head[0:4] != AFS_MAGIC:
        return None
    count = be32(head, 4)
    table = read_sector(f, file_lba, 1 + (count * 8 + 8 + SECTOR - 1) // SECTOR)
    entries = []
    for i in range(count):
        off = be32(table, 8 + i * 8)
        size = be32(table, 8 + i * 8 + 4)
        entries.append({'idx': i, 'off': off, 'size': size,
                        'lba': file_lba + off // SECTOR,
                        'sectors': (size + SECTOR - 1) // SECTOR if size else 0})
    return {'count': count, 'entries': entries}


def fmt_size(n):
    return '%9d B (%8.1f KB)' % (n, n / 1024.0)


def describe(iso_path, target, want_lba, compare_path):
    f = open(iso_path, 'rb')
    f.seek(0, os.SEEK_END)
    iso_size = f.tell()
    total_sectors = iso_size // SECTOR

    pvd = read_sector(f, 16)
    if pvd[1:6] != b'CD001':
        print('!! %s 不是 ISO9660 镜像（PVD 里没有 CD001）' % iso_path)
        return
    vol_sectors = le32(pvd, 0x50)
    root = parse_dir_record(pvd[0x9C:0x9C + 34])

    print('===== %s' % iso_path)
    print('镜像大小        : %d 字节 = %d 扇区 = %.2f GB' % (iso_size, total_sectors, iso_size / 2 ** 30))
    print('PVD 卷大小      : %d 扇区 (%.2f GB)' % (vol_sectors, vol_sectors * SECTOR / 2 ** 30))
    if vol_sectors != total_sectors:
        print('                ! 与镜像实际扇区数不一致，差 %+d 扇区（改版镜像常见的坑：'
              '卷大小没更新，OPL 的 mediaLsnCount 会受影响）' % (vol_sectors - total_sectors))
    print('根目录          : LBA %d, %d 字节' % (root['lba'], root['size']))

    rec = find_file(f, target)
    if rec is None:
        print('!! 根目录里没有找到 %s' % target)
        return
    print('目标文件 %-10s: LBA %d, %s' % (target, rec['lba'], fmt_size(rec['size'])))
    print('                路径 %s' % rec['path'])

    afs = parse_afs(f, rec['lba'], rec['size'])
    if afs is None:
        print('（%s 不是 AFS 包，跳过条目表）' % target)
        return

    print('AFS 条目数      : %d' % afs['count'])
    print(' #    AFS偏移     大小                  ISO LBA 区间           扇区数')
    for e in afs['entries']:
        rng = '%-7d - %-7d' % (e['lba'], e['lba'] + e['sectors'] - 1) if e['sectors'] else '        (空)'
        mark = ''
        if want_lba is not None and e['sectors'] and e['lba'] <= want_lba < e['lba'] + e['sectors']:
            mark = '   <== 命中 --lba %d' % want_lba
        print('%3d  %10d  %s  %s  %8d%s' % (e['idx'], e['off'], fmt_size(e['size']), rng, e['sectors'], mark))
    f.close()

    if not compare_path:
        return

    g = open(compare_path, 'rb')
    rec2 = find_file(g, target)
    if rec2 is None:
        print('!! 对比镜像里找不到 %s' % target)
        return
    afs2 = parse_afs(g, rec2['lba'], rec2['size'])
    print('\n===== 对比：%s（%s LBA %d）' % (compare_path, target, rec2['lba']))
    if afs2 is None:
        print('对比镜像里的 %s 不是 AFS' % target)
        return
    print(' #    %-22s %-22s 倍数' % ('本镜像大小', '对比镜像大小'))
    n = max(len(afs['entries']), len(afs2['entries']))
    for i in range(n):
        a = afs['entries'][i]['size'] if i < len(afs['entries']) else None
        b = afs2['entries'][i]['size'] if i < len(afs2['entries']) else None
        if a is None or b is None:
            print('%3d  %-22s %-22s' % (i, a, b))
            continue
        ratio = ('%6.2fx' % (a / b)) if b else '     -'
        print('%3d  %-22s %-22s %s' % (i, fmt_size(a), fmt_size(b), ratio))
    print('\n提示：两条视频如果时长差不多，大小倍数 ≈ 码率倍数；'
          'PCSX2 日志里测出来的读取速率倍数应和它接近。')


def main():
    ap = argparse.ArgumentParser(description='PS2 ISO / AFS 结构检查')
    ap.add_argument('iso')
    ap.add_argument('--file', default='MOV.AFS', help='要展开的 AFS 文件（默认 MOV.AFS）')
    ap.add_argument('--lba', type=int, default=None, help='给出 PCSX2 日志里的 LBA，标出它落在哪一条条目')
    ap.add_argument('--compare', default=None, help='对比镜像（例如原版 ISO）')
    args = ap.parse_args()
    describe(args.iso, args.file, args.lba, args.compare)


if __name__ == '__main__':
    main()
