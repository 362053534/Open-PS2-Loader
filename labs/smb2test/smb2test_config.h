#ifndef SMB2TEST_CONFIG_H

#define SMB2TEST_CONFIG_H



// 修改这些参数后重新编译 SMB2TEST.ELF（Actions 会编 labs/smb2test）。

//

// PCSX2「Switched / PCAP」桥接：访客与 SMB 同网段。

#define SMB2TEST_PS2_IP       "192.168.200.125"

#define SMB2TEST_NETMASK      "255.255.255.0"

#define SMB2TEST_GATEWAY      "192.168.200.124"

#define SMB2TEST_SERVER       "192.168.200.124"

#define SMB2TEST_PORT         445

#define SMB2TEST_SHARE        "ps2smb"

#define SMB2TEST_USER         "1"

#define SMB2TEST_PASSWORD     ""

#define SMB2TEST_FILE         "DVD/test.iso"

#define SMB2TEST_OFFSET_LOW   0

#define SMB2TEST_OFFSET_HIGH  0

#define SMB2TEST_READ_SIZE    16384

#define SMB2TEST_READ_COUNT   512


#endif

