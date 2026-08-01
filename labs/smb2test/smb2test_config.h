#ifndef SMB2TEST_CONFIG_H
#define SMB2TEST_CONFIG_H

// 修改这些参数后重新编译 SMB2TEST.ELF（Actions 会编 labs/smb2test）。
//
// PCSX2「端口/Sockets」实测：
//   访客 IP/网关用 192.0.2.100 / 192.0.2.1（拦截 DHCP）
//   SMB 服务器填本机真实局域网地址 192.168.5.4
// 本 ELF 现与 OPL 菜单相同：netman + smap + ps2ip-nm（不再用 smap-ingame）。
#define SMB2TEST_PS2_IP       "192.0.2.100"
#define SMB2TEST_NETMASK      "255.255.255.0"
#define SMB2TEST_GATEWAY      "192.0.2.1"
#define SMB2TEST_SERVER       "192.168.5.4"
#define SMB2TEST_PORT         445
#define SMB2TEST_SHARE        "ps2smb"
#define SMB2TEST_USER         "1"
#define SMB2TEST_PASSWORD     ""
#define SMB2TEST_FILE         "DVD/test.iso"
#define SMB2TEST_OFFSET_LOW   0
#define SMB2TEST_OFFSET_HIGH  0
#define SMB2TEST_READ_SIZE    16384
#define SMB2TEST_READ_COUNT   1024

#endif
