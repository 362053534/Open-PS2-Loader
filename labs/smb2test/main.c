#include <debug.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <kernel.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <sifrpc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "smb2test.h"
#include "smb2test_config.h"

extern unsigned char ps2dev9_irx[];
extern unsigned int size_ps2dev9_irx;
extern unsigned char smsutils_irx[];
extern unsigned int size_smsutils_irx;
extern unsigned char smstcpip_irx[];
extern unsigned int size_smstcpip_irx;
extern unsigned char smap_irx[];
extern unsigned int size_smap_irx;
extern unsigned char smb2test_irx[];
extern unsigned int size_smb2test_irx;

static SifRpcClientData_t rpcClient;
static struct smb2test_request rpcRequest __attribute__((aligned(64)));
static struct smb2test_result rpcResult __attribute__((aligned(64)));
static char ipConfig[64] __attribute__((aligned(64)));

int main(int argc, char *argv[])
{
    int i, moduleID, moduleResult = 0;
    int ipConfigLength = 0;
    u32 speed = 0;
    const char *stage;

    (void)argc;
    (void)argv;

    init_scr();
    scr_clear();
    scr_printf("SMB2TEST - OPL IOP SMB2 sequential read\n\n");

    SifInitRpc(0);
    scr_printf("Resetting IOP... ");
    while (!SifIopReset("", 0))
        ;
    while (!SifIopSync())
        ;

    SifExitIopHeap();
    SifLoadFileExit();
    SifExitRpc();
    SifExitCmd();

    SifInitRpc(0);
    FlushCache(0);
    FlushCache(2);
    SifLoadFileInit();
    SifInitIopHeap();
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
    scr_printf("OK\n");

    strcpy(&ipConfig[ipConfigLength], SMB2TEST_PS2_IP);
    ipConfigLength += strlen(SMB2TEST_PS2_IP) + 1;
    strcpy(&ipConfig[ipConfigLength], SMB2TEST_NETMASK);
    ipConfigLength += strlen(SMB2TEST_NETMASK) + 1;
    strcpy(&ipConfig[ipConfigLength], SMB2TEST_GATEWAY);
    ipConfigLength += strlen(SMB2TEST_GATEWAY) + 1;

    moduleID = SifExecModuleBuffer(ps2dev9_irx, size_ps2dev9_irx, 0, NULL, &moduleResult);
    scr_printf("DEV9: id=%d result=%d\n", moduleID, moduleResult);
    if (moduleID < 0)
        goto end;

    moduleID = SifExecModuleBuffer(smsutils_irx, size_smsutils_irx, 0, NULL, &moduleResult);
    scr_printf("SMSUTILS: id=%d result=%d\n", moduleID, moduleResult);
    if (moduleID < 0)
        goto end;

    moduleID = SifExecModuleBuffer(smstcpip_irx, size_smstcpip_irx, 0, NULL, &moduleResult);
    scr_printf("PS2IP: id=%d result=%d\n", moduleID, moduleResult);
    if (moduleID < 0)
        goto end;

    moduleID = SifExecModuleBuffer(smap_irx, size_smap_irx, ipConfigLength, ipConfig, &moduleResult);
    scr_printf("SMAP: id=%d result=%d\n", moduleID, moduleResult);
    if (moduleID < 0)
        goto end;
    usleep(2000000);

    moduleID = SifExecModuleBuffer(smb2test_irx, size_smb2test_irx, 0, NULL, &moduleResult);
    scr_printf("SMB2TEST.IRX: id=%d result=%d\n\n", moduleID, moduleResult);
    if (moduleID < 0)
        goto end;

    memset(&rpcClient, 0, sizeof(rpcClient));
    for (i = 0; i < 100; i++) {
        if (SifBindRpc(&rpcClient, SMB2TEST_RPC_ID, 0) >= 0 && rpcClient.server)
            break;
        usleep(10000);
    }
    if (!rpcClient.server) {
        scr_printf("RPC bind failed.\n");
        goto end;
    }

    memset(&rpcRequest, 0, sizeof(rpcRequest));
    strncpy(rpcRequest.server, SMB2TEST_SERVER, sizeof(rpcRequest.server) - 1);
    strncpy(rpcRequest.share, SMB2TEST_SHARE, sizeof(rpcRequest.share) - 1);
    strncpy(rpcRequest.user, SMB2TEST_USER, sizeof(rpcRequest.user) - 1);
    strncpy(rpcRequest.password, SMB2TEST_PASSWORD, sizeof(rpcRequest.password) - 1);
    strncpy(rpcRequest.path, SMB2TEST_FILE, sizeof(rpcRequest.path) - 1);
    rpcRequest.port = SMB2TEST_PORT;
    rpcRequest.offset_low = SMB2TEST_OFFSET_LOW;
    rpcRequest.offset_high = SMB2TEST_OFFSET_HIGH;
    rpcRequest.read_size = SMB2TEST_READ_SIZE;
    rpcRequest.read_count = SMB2TEST_READ_COUNT;

    scr_printf("Server: %s:%u/%s\n", rpcRequest.server, rpcRequest.port, rpcRequest.share);
    scr_printf("File: %s\n", rpcRequest.path);
    scr_printf("Read: %u x %u bytes\n\n", rpcRequest.read_count, rpcRequest.read_size);
    scr_printf("Testing...\n");
    scr_printf("(During test: netstat -an | findstr 192.168.5.100)\n\n");

    memset(&rpcResult, 0, sizeof(rpcResult));
    i = SifCallRpc(&rpcClient, SMB2TEST_CMD_RUN, 0, &rpcRequest, sizeof(rpcRequest), &rpcResult, sizeof(rpcResult), NULL, NULL);
    if (i < 0) {
        scr_printf("SifCallRpc failed: %d\n", i);
        goto end;
    }

    switch (rpcResult.stage) {
        case SMB2TEST_STAGE_NETWORK:
            stage = "NETWORK";
            break;
        case SMB2TEST_STAGE_MEMORY:
            stage = "MEMORY";
            break;
        case SMB2TEST_STAGE_CONNECT:
            stage = "CONNECT";
            break;
        case SMB2TEST_STAGE_OPEN:
            stage = "OPEN";
            break;
        case SMB2TEST_STAGE_READ:
            stage = "READ";
            break;
        case SMB2TEST_STAGE_CLOSE:
            stage = "CLOSE";
            break;
        case SMB2TEST_STAGE_DONE:
            stage = "DONE";
            break;
        default:
            stage = "UNKNOWN";
    }

    if (rpcResult.elapsed_ms)
        speed = (rpcResult.bytes_read / 1024) * 1000 / rpcResult.elapsed_ms;

    scr_printf("\nStage: %s\n", stage);
    scr_printf("Result: %d  Last read: %d\n", rpcResult.result, rpcResult.last_read_size);
    scr_printf("Bytes: %u  Time: %u ms\n", rpcResult.bytes_read, rpcResult.elapsed_ms);
    scr_printf("Speed: %u KiB/s\n", speed);
    scr_printf("Checksum: %08X\n", rpcResult.checksum);
    scr_printf("Dialect: %04X  MaxRead: %u\n", rpcResult.dialect, rpcResult.max_read_size);
    if (rpcResult.error[0])
        scr_printf("Error: %s\n", rpcResult.error);

end:
    scr_printf("\nTest stopped. Reset the console to run again.\n");
    SleepThread();
    return 0;
}
