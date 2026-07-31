# SMB2TEST.ELF

这个独立测试程序绕过 OPL 的游戏二次启动流程，直接在 IOP 上运行当前 `modules/iopcore/cdvdman/smb2.c`，用于确认 DEV9、PS2IP、SMAP、libsmb2 和顺序读取是否正常。

## 配置

修改 `smb2test_config.h`：

- `SMB2TEST_PS2_IP`、`SMB2TEST_NETMASK`、`SMB2TEST_GATEWAY`：PS2 网络参数；
- `SMB2TEST_SERVER`、`SMB2TEST_PORT`：SMB 服务器；
- `SMB2TEST_SHARE`、`SMB2TEST_USER`、`SMB2TEST_PASSWORD`：共享与账号；
- `SMB2TEST_FILE`：共享目录内的文件路径，例如 `DVD/game.iso`；
- `SMB2TEST_READ_SIZE`：每次读取大小，默认 16 KiB；
- `SMB2TEST_READ_COUNT`：读取次数，默认总计 16 MiB。

账号密码会被直接内嵌进 ELF；公开发布测试文件时请使用专用测试账号或无密码只读共享。

## 编译

```sh
make -C labs/smb2test clean all
```

输出文件为 `labs/smb2test/SMB2TEST.ELF`。

## 判断结果

- `Stage: DONE` 且 `Result: 0`：独立 SMB2 顺序读取成功；
- `NETWORK`：没有找到 PS2IP 导出表；
- `CONNECT`：SMB2 服务器、共享、账号或网络连接失败；
- `OPEN`：文件路径或访问权限错误；
- `READ`：读取过程中出现错误或提前到达文件末尾；
- 多次读取同一个文件、偏移和长度时，`Checksum` 应保持一致。
