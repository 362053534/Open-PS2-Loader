#!/usr/bin/env python3
"""只在隔离副本中破坏关键规则，检查回归测试是否会失败。"""

import hashlib
import json
import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
MUTANTS = [
    ("超时缩短为五秒", "modules/iopcore/cdvdman/smb.c", "SMB_ECHO_TIMEOUT_MS    6000", "SMB_ECHO_TIMEOUT_MS    5000"),
    ("六秒边界多等一拍", "modules/iopcore/cdvdman/smb.c", ">= timeout.lo", "> timeout.lo"),
    ("提前发送分钟 Echo", "modules/iopcore/cdvdman/device-smb.c", "SMB_ECHO_IDLE_TICKS  30", "SMB_ECHO_IDLE_TICKS  29"),
    ("恢复空闲 PHY 查询", "modules/iopcore/cdvdman/device-smb.c", "(void)arg;\n    if (!smb_echo_enabled)", "(void)arg;\n    smb_link_up();\n    if (!smb_echo_enabled)"),
    ("不跳过迟到 Echo", "modules/iopcore/cdvdman/smb.c", "if (!smb_is_echo_reply(rcv_size))", "if (1)"),
    ("不核对 Echo MID", "modules/iopcore/cdvdman/smb.c", "if (!echo_mid || SMB_buf.smb.echoRequest.smbH.MID != echo_mid)", "if (!echo_mid)"),
    ("Echo 等待 IGR 锁", "modules/iopcore/cdvdman/smb.c", "PollSema(smb_io_sema)", "WaitSema(smb_io_sema)"),
    ("Echo 走阻塞发送", "modules/iopcore/cdvdman/smb.c", "sizeof(*echo) + 5, MSG_DONTWAIT", "sizeof(*echo) + 5, 0"),
    ("拒绝恰好够用的发送空间", "modules/network/SMSTCPIP/api_msg.c", "msg->msg.w.len > tcp_sndbuf(pcb)", "msg->msg.w.len >= tcp_sndbuf(pcb)"),
    ("非阻塞发送重新忽略长度截断", "modules/network/SMSTCPIP/sockets.c", "size < 0 || size > 0xffff ? ERR_ARG : netconn_trywrite(sock->conn, data, size)", "netconn_trywrite(sock->conn, data, size)"),
]
FILES = [
    "modules/iopcore/cdvdman/smb.c", "modules/iopcore/cdvdman/smb.h",
    "modules/iopcore/cdvdman/device-smb.c", "modules/iopcore/cdvdman/device.h",
    "modules/iopcore/cdvdman/internal.h", "modules/iopcore/common/cdvd_config.h",
    "modules/iopcore/common/oplsmb.h", "modules/network/SMSTCPIP/sockets.c",
    "modules/network/SMSTCPIP/api_lib.c", "modules/network/SMSTCPIP/api_msg.c",
    "modules/network/SMSTCPIP/include/lwip/api.h",
    "modules/network/SMSTCPIP/include/lwip/api_msg.h",
]
before = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in FILES}
results = []
for label, name, old, new in MUTANTS:
    with tempfile.TemporaryDirectory(prefix="opl-smb-mutant-") as tmp:
        copy = pathlib.Path(tmp)
        for relative in FILES:
            target = copy / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, target)
        shutil.copytree(ROOT / "tests/smb", copy / "tests/smb")
        path = copy / name
        source = path.read_text()
        assert source.count(old) == 1, label
        path.write_text(source.replace(old, new))
        result = subprocess.run(["python3", str(copy / "tests/smb/run.py")],
                                capture_output=True, text=True, timeout=20)
        # 编译失败不能证明测试会抓住业务错误。
        killed = result.returncode != 0 and ("Assertion" in result.stderr or "FAIL:" in result.stderr)
        results.append({"mutation": label, "killed": killed})
        print(json.dumps(results[-1], ensure_ascii=False), flush=True)
        assert killed, result.stdout + result.stderr
after = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in FILES}
assert before == after
print(f"PASS: {len(results)} mutations rejected; workspace source hashes unchanged")
