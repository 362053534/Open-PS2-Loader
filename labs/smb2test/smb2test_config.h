#ifndef SMB2TEST_CONFIG_H
#define SMB2TEST_CONFIG_H

// 网络与 SMB 参数自动读取 OPL 的 conf_network.cfg。
// 修改以下测速参数后重新编译 SMB2TEST.ELF（Actions 会编 labs/smb2test）。
#define SMB2TEST_DIRECTORY    "DVD"
#define SMB2TEST_OFFSET_LOW   0
#define SMB2TEST_OFFSET_HIGH  0
#define SMB2TEST_READ_SIZE    2048
#define SMB2TEST_READ_COUNT   4096

#endif
