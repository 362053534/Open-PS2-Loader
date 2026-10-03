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
import re
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


FRAME_RATES = {1: 23.976, 2: 24.0, 3: 25.0, 4: 29.97, 5: 30.0, 6: 50.0, 7: 59.94, 8: 60.0}


def _parse_pts(b, i):
    """从 PES 起始码位置解析 PTS（兼容 MPEG-2 和 MPEG-1 两种 PES 头布局）。"""
    if i + 14 > len(b):
        return None
    # MPEG-2 PES: 00 00 01 id | len(2) | 0x80|x | flags | hdrlen | PTS(5)
    if (b[i + 6] & 0xC0) == 0x80 and (b[i + 9] & 0xF0) in (0x20, 0x30):
        p = b[i + 9:i + 14]
        if (p[0] & 1) and (p[2] & 1) and (p[4] & 1):
            return ((p[0] >> 1) & 0x07) << 30 | p[1] << 22 | ((p[2] >> 1) & 0x7F) << 15 | \
                   p[3] << 7 | (p[4] >> 1)
    # MPEG-1 PES: 00 00 01 id | len(2) | PTS(5)
    if (b[i + 6] & 0xF0) in (0x20, 0x30):
        p = b[i + 6:i + 11]
        if (p[0] & 1) and (p[2] & 1) and (p[4] & 1):
            return ((p[0] >> 1) & 0x07) << 30 | p[1] << 22 | ((p[2] >> 1) & 0x7F) << 15 | \
                   p[3] << 7 | (p[4] >> 1)
    return None


def scan_pss(f, lba, size, chunk=4 << 20):
    """扫 MPEG-PS 结构：pack/SCR 时间轴、视频/音频 PTS、序列头（分辨率/帧率）。

    关键输出是「需要的持续读取速率」= 条目大小 / 时间轴时长：
    播放器要按这个速度持续喂数据，喂不上就会停住等待；
    拿它和 PCSX2 日志里量到的「每秒请求扇区数」对照，能直接判断谁对。
    """
    if size <= 0:
        return None
    f.seek(lba * SECTOR)
    remaining = size
    base = 0
    carry = b''
    first_pack = last_pack = None
    packs = 0
    pts = {0xE0: [None, None, 0], 0xC0: [None, None, 0]}
    seq = None
    while remaining > 0:
        data = f.read(min(chunk, remaining))
        if not data:
            break
        buf = carry + data
        off0 = base - len(carry)

        for m in re.finditer(rb'\x00\x00\x01', buf):
            i = m.start()
            code = buf[i + 3] if i + 3 < len(buf) else None
            if code is None:
                continue
            if code == 0xBA and i + 14 <= len(buf):
                b = buf[i:i + 14]
                if (b[4] & 0xC0) == 0x40:  # MPEG-2 pack
                    scr = (((b[4] >> 3) & 0x07) << 30) | ((b[4] & 0x03) << 28) | (b[5] << 20) \
                        | (((b[6] >> 3) & 0x1F) << 15) | ((b[6] & 0x03) << 13) | (b[7] << 5) \
                        | ((b[8] >> 3) & 0x1F)
                    if first_pack is None:
                        first_pack = scr
                    last_pack = scr
                    packs += 1
            elif code in pts and i + 16 <= len(buf):
                v = _parse_pts(buf, i)
                if v is not None:
                    slot = pts[code]
                    if slot[0] is None:
                        slot[0] = v
                    slot[1] = v
                    slot[2] += 1
            elif code == 0xB3 and i + 8 <= len(buf):
                b = buf[i + 4:i + 8]
                w = (b[0] << 4) | (b[1] >> 4)
                h = ((b[1] & 0x0F) << 8) | b[2]
                fr = b[3] & 0x0F
                if seq is None:
                    seq = (w, h, FRAME_RATES.get(fr, fr))

        base += len(data)
        carry = buf[-16:]
        remaining -= len(data)

    if first_pack is None or last_pack is None or last_pack <= first_pack:
        return None
    duration = (last_pack - first_pack) / 90000.0
    out = {
        'count': packs,
        'duration': duration,
        'bitrate_mbps': size * 8.0 / duration / 1e6,
        'required_kbps': size / duration / 1024.0,
        'seq': seq,
        'pts': {},
    }
    for code, name in ((0xE0, 'video'), (0xC0, 'audio')):
        a, b_, n = pts[code]
        if a is not None and b_ is not None and b_ > a:
            out['pts'][name] = {'span': (b_ - a) / 90000.0, 'count': n, 'first': a / 90000.0}
    return out


def fmt_pss(info, label=''):
    if info is None:
        return '%s(扫不到 MPEG-PS 结构：可能不是 MPEG-PS，或整段是别的东西)' % label
    lines = ['%s时长 %.2f s，平均码率 %.2f Mbps，共 %d 个 pack' % (label, info['duration'], info['bitrate_mbps'], info['count'])]
    if info.get('seq'):
        w, h, fps = info['seq']
        lines.append('%s序列头：%dx%d, %.2f fps' % (' ' * len(label), w, h, fps))
    for name, zh in (('video', '视频'), ('audio', '音频')):
        p = info['pts'].get(name)
        if p:
            lines.append('%s%s PTS 跨度 %.2f s（共 %d 个包）' % (' ' * len(label), zh, p['span'], p['count']))
    lines.append('%s>>> 播放需要的持续读取速率 = %.0f KB/s (%.2f MB/s)' % (' ' * len(label), info['required_kbps'], info['required_kbps'] / 1024.0))
    return '\n'.join(lines)


def fmt_size(n):
    return '%9d B (%8.1f KB)' % (n, n / 1024.0)


def describe(iso_path, target, want_lba, compare_path, pss_idx=None):
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
    hit = None
    for e in afs['entries']:
        rng = '%-7d - %-7d' % (e['lba'], e['lba'] + e['sectors'] - 1) if e['sectors'] else '        (空)'
        mark = ''
        if want_lba is not None and e['sectors'] and e['lba'] <= want_lba < e['lba'] + e['sectors']:
            mark = '   <== 命中 --lba %d' % want_lba
            hit = e
        print('%3d  %10d  %s  %s  %8d%s' % (e['idx'], e['off'], fmt_size(e['size']), rng, e['sectors'], mark))
    print()

    # 码率核对：默认扫「--lba 命中的那一条」，没有就给 --pss 指定条目号。
    scan_idx = pss_idx if pss_idx is not None else (hit['idx'] if hit is not None else None)
    if scan_idx is not None and 0 <= scan_idx < len(afs['entries']):
        e = afs['entries'][scan_idx]
        info = scan_pss(f, e['lba'], e['size'])
        print('条目 %d 码率扫描 : %s' % (scan_idx, fmt_pss(info)))
        if info is not None and want_lba is not None:
            inside = (want_lba - e['lba']) * SECTOR
            print('                 : 日志里的 LBA %d 在这条里偏移 %.1f MB（%.1f%% 处）'
                  % (want_lba, inside / 2 ** 20, 100.0 * inside / e['size']))
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

    # 码率实测对比：同样扫这条条目（有 --pss/命中条目时）。
    if scan_idx is not None and scan_idx < len(afs['entries']) and scan_idx < len(afs2['entries']):
        e1 = afs['entries'][scan_idx]
        e2 = afs2['entries'][scan_idx]
        with open(iso_path, 'rb') as ff:
            i1 = scan_pss(ff, e1['lba'], e1['size'])
        i2 = scan_pss(g, e2['lba'], e2['size'])
        print('\n条目 %d 码率/时间轴对比：' % scan_idx)
        print('  本镜像  : %s' % fmt_pss(i1))
        print('  对比镜像: %s' % fmt_pss(i2))
        if i1 and i2:
            r = i1['required_kbps'] / i2['required_kbps'] if i2['required_kbps'] else 0
            print('  >>> 需要读取速率的倍数：**%.2fx**' % r)
            print('      和 PCSX2 日志里量到的「每秒请求扇区数」倍数对照：'
                  '一致 ⇒ 就是码率/时间轴决定的；'
                  '不一致 ⇒ 是播放器按扇区模式多读（不是码率问题）')


def main():
    ap = argparse.ArgumentParser(description='PS2 ISO / AFS 结构检查')
    ap.add_argument('iso')
    ap.add_argument('--file', default='MOV.AFS', help='要展开的 AFS 文件（默认 MOV.AFS）')
    ap.add_argument('--lba', type=int, default=None, help='给出 PCSX2 日志里的 LBA，标出它落在哪一条条目')
    ap.add_argument('--compare', default=None, help='对比镜像（例如原版 ISO）')
    ap.add_argument('--pss', type=int, default=None, help='指定要测码率的 AFS 条目号（默认用 --lba 命中的那条）')
    args = ap.parse_args()
    describe(args.iso, args.file, args.lba, args.compare, args.pss)


if __name__ == '__main__':
    main()
