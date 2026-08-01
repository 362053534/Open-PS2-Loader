# SMB2TEST.ELF

独立测试：在 IOP 上跑 `modules/iopcore/cdvdman/smb2.c`（libsmb2），验证顺序读取。

当前 EE 侧网络栈与 **OPL 菜单 ETH 相同**：`netman` + `smap` + `ps2ip-nm` + `ps2ips`（不再使用 `smap-ingame` / `SMSTCPIP`）。

## 配置

改 `smb2test_config.h` 后由 Actions / `make -C labs/smb2test` 重新编译。

PCSX2「端口/Sockets」常用：

- PS2 / 网关：`192.0.2.100` / `192.0.2.1`
- SMB 服务器：本机局域网 IP（如 `192.168.5.4`）

PCSX2 调试 SMB2 时请关闭 IOP 动态重编译（`EnableIOP = false`），否则在 NEGOTIATE 收包后易宿主崩溃（`c0000005` / `IP_ON_HEAP`）。

## 编译

```sh
make -C labs/smb2test clean all
```

产物：`labs/smb2test/SMB2TEST.ELF`（CI artifact 名 `SMB2TEST`）。

## 结果

- `Stage: DONE` 且 `Result: 0`：成功
- `CONNECT`：TCP/SMB2 协商失败（看 `Error`）
- `OPEN` / `READ`：路径或读取失败
