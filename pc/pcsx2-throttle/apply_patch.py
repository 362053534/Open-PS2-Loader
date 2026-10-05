#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把「光驱限速」补丁应用到 PCSX2 源码树（不依赖 git / 行尾，可重复执行）。

为什么不直接用 git apply：CI 上 checkout 出来的文件行尾/上下文可能和补丁不一致
（Windows runner 的 autocrlf、浅克隆都会影响 git apply）。这里用字符串锚点定位，
按照文件自己的行尾做替换，打完自检；已经打过就跳过。

用法：
    python3 apply_patch.py <PCSX2 源码根目录>        # 默认 pcsx2
"""
import os
import sys

MARK = "PCSX2_CDVD_KBPS"

BLOCK = '''// ─────────────────────────────────────────────────────────────────────────────
// 实验补丁：把光驱等效速率限制到指定值（默认关闭，正常使用完全不受影响）
//
// 用途：在 PCSX2 里复现 PS2 这类慢速链路（USB 1.1 ≈ 1 MB/s）下的读盘行为，
//       用来验证「游戏要数据的速度 > 链路给数据的速度」这类结论。
//
//   PCSX2_CDVD_KBPS=1024    光驱等效速率压到 1024 KB/s（= 1 MB/s）
//   PCSX2_CDVD_CMD_US=1500  每笔读命令再额外加 1.5 ms 固定开销（可选，模拟命令往返）
//
// 原理：cdvd.ReadTime 是「每个扇区多少 PSXCLK 周期」——光驱按这个速率把扇区补进
//       16 扇区的读缓冲，游戏从缓冲取走数据。默认 = 675 * min(Speed,1.6) 扇区/秒
//       （游戏请求 2x CLV 时 ≈ 2.11 MB/s）；设了 PCSX2_CDVD_KBPS 就换成
//       (KBps * 1024 / 2048) 扇区/秒，恒定不随盘片位置 / 游戏请求的速度变化。
// ─────────────────────────────────────────────────────────────────────────────
static u32 s_cdvd_kbps = 0;
static u32 s_cdvd_cmd_us = 0;
static bool s_cdvd_override_loaded = false;

static void cdvdLoadSpeedOverride()
{
	if (s_cdvd_override_loaded)
		return;

	s_cdvd_override_loaded = true;

	const char* kbps = std::getenv("PCSX2_CDVD_KBPS");
	const char* cmd_us = std::getenv("PCSX2_CDVD_CMD_US");
	s_cdvd_kbps = kbps ? static_cast<u32>(std::strtoul(kbps, nullptr, 10)) : 0;
	s_cdvd_cmd_us = cmd_us ? static_cast<u32>(std::strtoul(cmd_us, nullptr, 10)) : 0;

	if (s_cdvd_kbps)
	{
		const u32 sectors_per_second = (s_cdvd_kbps * 1024) / 2048;
		Console.WriteLn("CDVD throttle: %u KB/s (%u sectors/s, %u cycles/sector), +%u us/command",
			s_cdvd_kbps, sectors_per_second, sectors_per_second ? static_cast<u32>(PSXCLK / sectors_per_second) : 0, s_cdvd_cmd_us);
	}
}

'''

EDITS = [
    # 1) 需要的头文件
    (
        "#include <cctype>\n#include <ctime>\n",
        "#include <cctype>\n#include <cstdlib>\n#include <ctime>\n",
    ),
    # 2) 速率覆盖块 + cdvdBlockReadTime() 开头提前返回
    (
        "static uint cdvdBlockReadTime(CDVD_MODE_TYPE mode) noexcept\n{\n"
        "\t// CAV Read speed is roughly 41% in the centre full speed on outer edge.",
        BLOCK +
        "static uint cdvdBlockReadTime(CDVD_MODE_TYPE mode) noexcept\n{\n"
        "\tcdvdLoadSpeedOverride();\n"
        "\tif (s_cdvd_kbps)\n"
        "\t{\n"
        "\t\tconst u32 sectors_per_second = (s_cdvd_kbps * 1024) / 2048;\n"
        "\t\tif (sectors_per_second)\n"
        "\t\t\treturn static_cast<uint>(PSXCLK / sectors_per_second);\n"
        "\t}\n\n"
        "\t// CAV Read speed is roughly 41% in the centre full speed on outer edge.",
    ),
    # 3) 每笔读命令的固定开销（cdvdStartSeek 的结尾）
    (
        "\telse // We're seeking, so kick off the buffering after the seek finishes.\n"
        "\t{\n"
        "\t\tCDVDSECTORREADY_INT(seektime);\n"
        "\t}\n\n"
        "\treturn seektime;\n}",
        "\telse // We're seeking, so kick off the buffering after the seek finishes.\n"
        "\t{\n"
        "\t\tCDVDSECTORREADY_INT(seektime);\n"
        "\t}\n\n"
        "\tif (s_cdvd_cmd_us)\n"
        "\t\tseektime += static_cast<u32>((static_cast<double>(PSXCLK) / 1000000.0) * s_cdvd_cmd_us);\n\n"
        "\treturn seektime;\n}",
    ),
]


def adapt(text, nl):
    return text.replace("\n", nl) if nl != "\n" else text


def main():
    # Windows 的 cmd/CI 里 stdout 编码可能不是 UTF-8，打印中文会直接抛异常；这里兜底。
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

    root = sys.argv[1] if len(sys.argv) > 1 else "pcsx2"
    path = os.path.join(root, "pcsx2", "CDVD", "CDVD.cpp")
    if not os.path.isfile(path):
        print("找不到 %s（参数应是 PCSX2 源码根目录）" % path)
        return 2

    with open(path, "rb") as f:
        raw = f.read()

    if MARK.encode() in raw:
        print("已经打过补丁了：%s" % path)
        return 0

    text = raw.decode("utf-8")
    nl = "\r\n" if "\r\n" in text else "\n"
    print("文件行尾：%s，大小 %d 字节" % ("CRLF" if nl != "\n" else "LF", len(raw)))

    for i, (old, new) in enumerate(EDITS):
        old_s, new_s = adapt(old, nl), adapt(new, nl)
        found = text.count(old_s)
        if found != 1:
            print("第 %d 处锚点没找到（出现 %d 次）—— PCSX2 源码可能变了，请对照 .patch.txt 检查" % (i + 1, found))
            return 3
        text = text.replace(old_s, new_s)

    with open(path, "wb") as f:
        f.write(text.encode("utf-8"))

    out = open(path, encoding="utf-8").read()
    for token in ("PCSX2_CDVD_KBPS", "PCSX2_CDVD_CMD_US", "std::getenv", "cdvdLoadSpeedOverride();", "s_cdvd_cmd_us"):
        if token not in out:
            print("自检失败：缺少 %s" % token)
            return 4
    print("已应用 3 处修改并自检通过：%s" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
