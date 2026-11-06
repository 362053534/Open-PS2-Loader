#!/usr/bin/env python3
"""在主机上编译实际 SMB 源码，只替换 PS2 内核和网络边界。"""

import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
TEST = ROOT / "tests/smb"
DEBUG = "--debug" in sys.argv[1:]


def function(source, name):
    match = re.search(r"(?:static void\n|err_t |int )" + name + r"\([^;]*?\)\n\{", source)
    assert match, name
    end = source.index("\n}", match.end()) + 2
    return source[match.start():end]


with tempfile.TemporaryDirectory(prefix="opl-smb-test-") as tmp:
    build = pathlib.Path(tmp)
    for header in ("sysclib.h", "thbase.h", "thsemap.h", "intrman.h", "sifman.h",
                   "smstcpip.h", "smsutils.h", "tamtypes.h", "usbhdfsd-common.h"):
        (build / header).write_text('#include "host.h"\n')
    sockets = (ROOT / "modules/network/SMSTCPIP/sockets.c").read_text()
    receive = sockets[sockets.index("/* NBSS keepalive"):sockets.index("#ifdef FULL_LWIP", sockets.index("/* NBSS keepalive"))]
    # 原样编译线上收包函数，避免测试替身掩盖半包、粘包处理错误。
    receive = receive.replace("sin.sin_len = sizeof(sin);", "/* Linux sockaddr_in 没有 sin_len。 */")
    (build / "receive.c").write_text('#include "transport.h"\n' + receive)
    cmd = ["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare", "-g",
           "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
           "-I" + str(build), "-I" + str(TEST),
           "-I" + str(ROOT / "modules/iopcore/common"),
           "-I" + str(ROOT / "modules/iopcore/cdvdman"),
           "-include", str(TEST / "host.h"), str(TEST / "test_smb.c"),
           str(ROOT / "modules/iopcore/cdvdman/smb.c"),
           str(ROOT / "modules/iopcore/cdvdman/device-smb.c"),
           str(build / "receive.c"),
           "-o", str(build / "test_smb")]
    if DEBUG:
        cmd.insert(1, "-D__IOPCORE_DEBUG")
    subprocess.run(cmd, check=True)
    for case in ("echo", "timeout", "late_partial", "idle", "send_nonblocking", "timeout_cadence",
                 "lock_busy", "send_blocked", "wrong_mid", "bad_echo", "closed", "shutdown",
                 "read_priority", "read_after_missing", "slow_poll", "ignored_replies", "delayed_success", "server_error"):
        result = subprocess.run([str(build / "test_smb"), case], capture_output=True, text=True)
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        result.check_returncode()
        if DEBUG:
            timeouts = result.stdout.count("[SMBECHO] timeout ")
            expected = 2 if case == "timeout_cadence" else 1 if case in (
                "timeout", "late_partial", "read_after_missing", "slow_poll", "ignored_replies") else 0
            assert timeouts == expected, (case, result.stdout)
            if expected:
                assert f"missed={expected}" in result.stdout
    for prefix in (1, 3, 4, 5, 10, 35, 36, 40, 41):
        subprocess.run([str(build / "test_smb"), "late_partial", str(prefix)], check=True)
    api_header = (ROOT / "modules/network/SMSTCPIP/include/lwip/api_msg.h").read_text()
    api_types = api_header[api_header.index("enum api_msg_type"):api_header.index("typedef void (*api_msg_decode)")]
    sender = '#include "send_host.h"\n' + api_types
    sender += function((ROOT / "modules/network/SMSTCPIP/api_msg.c").read_text(), "do_trywrite")
    sender += function((ROOT / "modules/network/SMSTCPIP/api_lib.c").read_text(), "netconn_trywrite")
    sender += function(sockets, "lwip_send")
    sender += (TEST / "test_send.c").read_text()
    (build / "send.c").write_text(sender)
    subprocess.run(["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-g",
                    "-fsanitize=address,undefined", "-I" + str(TEST),
                    "-I" + str(ROOT / "modules/iopcore/common"),
                    "-I" + str(ROOT / "modules/iopcore/cdvdman"),
                    "-I" + str(build), str(build / "send.c"),
                    "-o", str(build / "test_send")], check=True)
    subprocess.run([str(build / "test_send")], check=True)
