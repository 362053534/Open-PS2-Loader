#include <debug.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <kernel.h>
#include <loadfile.h>
#include <netman.h>
#include <ps2ips.h>
#include <sbv_patches.h>
#include <sifrpc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "smb2test.h"
#include "smb2test_config.h"

extern unsigned char ps2dev9_irx[];
extern unsigned int size_ps2dev9_irx;
extern unsigned char netman_irx[];
extern unsigned int size_netman_irx;
extern unsigned char smsutils_irx[];
extern unsigned int size_smsutils_irx;
extern unsigned char smap_irx[];
extern unsigned int size_smap_irx;
extern unsigned char ps2ip_irx[];
extern unsigned int size_ps2ip_irx;
extern unsigned char ps2ips_irx[];
extern unsigned int size_ps2ips_irx;
extern unsigned char smb2test_irx[];
extern unsigned int size_smb2test_irx;

static SifRpcClientData_t rpcClient;
static struct smb2test_request rpcRequest __attribute__((aligned(64)));
static struct smb2test_result rpcResult __attribute__((aligned(64)));

static int parse_ipv4(const char *text, unsigned char out[4])
{
    int a, b, c, d;

    if (sscanf(text, "%d.%d.%d.%d", &a, &b, &c, &d) != 4)
        return -1;
    if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255)
        return -1;
    out[0] = (unsigned char)a;
    out[1] = (unsigned char)b;
    out[2] = (unsigned char)c;
    out[3] = (unsigned char)d;
    return 0;
}

static int apply_static_ip(void)
{
    t_ip_info ip_info;
    unsigned char ip[4], mask[4], gw[4];
    int result;

    if (parse_ipv4(SMB2TEST_PS2_IP, ip) < 0 ||
        parse_ipv4(SMB2TEST_NETMASK, mask) < 0 ||
        parse_ipv4(SMB2TEST_GATEWAY, gw) < 0) {
        scr_printf("Bad IP config strings.\n");
        return -1;
    }

    result = ps2ip_getconfig("sm0", &ip_info);
    if (result < 0) {
        scr_printf("ps2ip_getconfig failed: %d\n", result);
        return result;
    }

    memset(&ip_info.ipaddr, 0, sizeof(ip_info.ipaddr));
    memset(&ip_info.netmask, 0, sizeof(ip_info.netmask));
    memset(&ip_info.gw, 0, sizeof(ip_info.gw));
    memcpy(&ip_info.ipaddr, ip, 4);
    memcpy(&ip_info.netmask, mask, 4);
    memcpy(&ip_info.gw, gw, 4);
    ip_info.dhcp_enabled = 0;

    result = ps2ip_setconfig(&ip_info);
    if (result < 0) {
        scr_printf("ps2ip_setconfig failed: %d\n", result);
        return result;
    }

    scr_printf("IP %u.%u.%u.%u gw %u.%u.%u.%u\n",
               ip[0], ip[1], ip[2], ip[3], gw[0], gw[1], gw[2], gw[3]);
    return 0;
}

int main(int argc, char *argv[])
{
    int i, moduleID, moduleResult = 0;
    u32 speed = 0;
    const char *stage;

    (void)argc;
    (void)argv;

    init_scr();
    scr_clear();
    scr_printf("SMB2TEST - OPL IOP SMB2 (netman/ps2ip stack)\n\n");

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

    moduleID = SifExecModuleBuffer(ps2dev9_irx, size_ps2dev9_irx, 0, NULL, &moduleResult);
    scr_printf("DEV9: id=%d result=%d\n", moduleID, moduleResult);
    if (moduleID < 0)
        goto end;

    /* 加载顺序与 OPL ethLoadModules 对齐 */
    moduleID = SifExecModuleBuffer(netman_irx, size_netman_irx, 0, NULL, &moduleResult);
    scr_printf("NETMAN: id=%d result=%d\n", moduleID, moduleResult);
    if (moduleID < 0)
        goto end;
    NetManInit();

    moduleID = SifExecModuleBuffer(smsutils_irx, size_smsutils_irx, 0, NULL, &moduleResult);
    scr_printf("SMSUTILS: id=%d result=%d\n", moduleID, moduleResult);
    if (moduleID < 0)
        goto end;

    moduleID = SifExecModuleBuffer(smap_irx, size_smap_irx, 0, NULL, &moduleResult);
    scr_printf("SMAP: id=%d result=%d\n", moduleID, moduleResult);
    if (moduleID < 0)
        goto end;

    moduleID = SifExecModuleBuffer(ps2ip_irx, size_ps2ip_irx, 0, NULL, &moduleResult);
    scr_printf("PS2IP: id=%d result=%d\n", moduleID, moduleResult);
    if (moduleID < 0)
        goto end;

    moduleID = SifExecModuleBuffer(ps2ips_irx, size_ps2ips_irx, 0, NULL, &moduleResult);
    scr_printf("PS2IPS: id=%d result=%d\n", moduleID, moduleResult);
    if (moduleID < 0)
        goto end;
    ps2ip_init();

    usleep(500000);
    if (apply_static_ip() < 0)
        goto end;
    usleep(1500000);

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
    scr_printf("First 8s: TCP hold - run: netstat -an | findstr \":445\"\n\n");

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
