#ifndef SMB2TEST_H
#define SMB2TEST_H

#include <tamtypes.h>

#define SMB2TEST_RPC_ID  0x534D4232
#define SMB2TEST_CMD_RUN 1
#define SMB2TEST_CMD_STATUS 2
#define SMB2TEST_SEGMENT_COUNT 4

enum smb2test_stage {
    SMB2TEST_STAGE_NONE = 0,
    SMB2TEST_STAGE_NETWORK,
    SMB2TEST_STAGE_MEMORY,
    SMB2TEST_STAGE_CONNECT,
    SMB2TEST_STAGE_NEGOTIATE,
    SMB2TEST_STAGE_SESSION,
    SMB2TEST_STAGE_SESSION_WAIT,
    SMB2TEST_STAGE_SESSION_RETRY,
    SMB2TEST_STAGE_TREE,
    SMB2TEST_STAGE_OPEN,
    SMB2TEST_STAGE_READ,
    SMB2TEST_STAGE_CLOSE,
    SMB2TEST_STAGE_DONE
};

enum smb2test_type {
    SMB2TEST_TYPE_SMB1_SEQUENTIAL = 0,
    SMB2TEST_TYPE_SMB1_RANDOM,
    SMB2TEST_TYPE_SMB2_SEQUENTIAL,
    SMB2TEST_TYPE_SMB2_RANDOM,
    SMB2TEST_TYPE_COUNT
};

struct smb2test_request
{
    char server[24];
    char share[32];
    char user[32];
    char password[32];
    char path[160];
    u16 port;
    u16 sector_cache_size;
    u32 offset_low;
    u32 offset_high;
    u32 read_size;
    u32 read_count;
    u32 test_type;
};

struct smb2test_measurement
{
    s32 result;
    u32 bytes_read;
    u32 elapsed_ms;
    u32 checksum;
    u32 segment_speed[SMB2TEST_SEGMENT_COUNT];
    s32 last_read_size;
    char error[64];
};

struct smb2test_result
{
    s32 result;
    u32 stage;
    u32 bytes_read;
    u32 elapsed_ms;
    u32 checksum;
    u32 dialect;
    u32 max_read_size;
    s32 last_read_size;
    char error[96];
    struct smb2test_measurement smb1_sequential;
    struct smb2test_measurement smb1_random;
    struct smb2test_measurement smb2_sequential;
    struct smb2test_measurement smb2_random;
};

#endif
