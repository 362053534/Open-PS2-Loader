#ifndef SMB2TEST_CONFIG_H
#define SMB2TEST_CONFIG_H

// 修改这些参数后重新编译 SMB2TEST.ELF。
#define SMB2TEST_PS2_IP       "192.168.5.100"
#define SMB2TEST_NETMASK      "255.255.255.0"
#define SMB2TEST_GATEWAY      "192.168.5.4"
/* PCAP 测局域网用 192.168.5.4；PCSX2 Socket 连本机共享请改成 127.0.0.1 */
#define SMB2TEST_SERVER       "127.0.0.1"
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
