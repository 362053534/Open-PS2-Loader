#include <debug.h>
#include <fcntl.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <kernel.h>
#include <loadfile.h>
#include <netman.h>
#include <ps2ips.h>
#include <sbv_patches.h>
#include <sifcmd.h>
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
static char configPS2IP[16];
static char configNetmask[16];
static char configGateway[16];
static char configSMBPrefix[32];
static int configUseDHCP;

static int load_opl_network_config(void)
{
    static const char *paths[] = {
        "mc0:OPL/conf_network.cfg",
        "mc1:OPL/conf_network.cfg",
    };
    char buffer[2049], oplPath[32];
    const char *configPath = NULL;
    int fd = -1, size, i;

    memset(&rpcRequest, 0, sizeof(rpcRequest));
    rpcRequest.port = 445;

    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        fd = open(paths[i], O_RDONLY);
        if (fd >= 0) {
            configPath = paths[i];
            break;
        }
    }

    if (fd < 0) {
        scr_printf("OPL conf_network.cfg not found.\n");
        return -1;
    }

    size = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (size <= 0) {
        scr_printf("Unable to read %s.\n", configPath);
        return -1;
    }
    buffer[size] = '\0';

    for (char *line = buffer; *line;) {
        char *next = strpbrk(line, "\r\n");
        char *separator;

        if (next) {
            *next++ = '\0';
            while (*next == '\r' || *next == '\n')
                next++;
        } else
            next = line + strlen(line);

        separator = strchr(line, '=');
        if (separator) {
            char *value = separator + 1;
            int number;

            *separator = '\0';
            if (!strcmp(line, "ps2_ip_use_dhcp"))
                sscanf(value, "%d", &configUseDHCP);
            else if (!strcmp(line, "ps2_ip_addr")) {
                strncpy(configPS2IP, value, sizeof(configPS2IP) - 1);
                configPS2IP[sizeof(configPS2IP) - 1] = '\0';
            } else if (!strcmp(line, "ps2_netmask")) {
                strncpy(configNetmask, value, sizeof(configNetmask) - 1);
                configNetmask[sizeof(configNetmask) - 1] = '\0';
            } else if (!strcmp(line, "ps2_gateway")) {
                strncpy(configGateway, value, sizeof(configGateway) - 1);
                configGateway[sizeof(configGateway) - 1] = '\0';
            } else if (!strcmp(line, "smb_ip")) {
                strncpy(rpcRequest.server, value, sizeof(rpcRequest.server) - 1);
                rpcRequest.server[sizeof(rpcRequest.server) - 1] = '\0';
            } else if (!strcmp(line, "smb_share")) {
                strncpy(rpcRequest.share, value, sizeof(rpcRequest.share) - 1);
                rpcRequest.share[sizeof(rpcRequest.share) - 1] = '\0';
            } else if (!strcmp(line, "smb_user")) {
                strncpy(rpcRequest.user, value, sizeof(rpcRequest.user) - 1);
                rpcRequest.user[sizeof(rpcRequest.user) - 1] = '\0';
            } else if (!strcmp(line, "smb_pass")) {
                strncpy(rpcRequest.password, value, sizeof(rpcRequest.password) - 1);
                rpcRequest.password[sizeof(rpcRequest.password) - 1] = '\0';
            } else if (!strcmp(line, "smb_port") && sscanf(value, "%d", &number) == 1)
                rpcRequest.port = number;
        }

        line = next;
    }

    snprintf(oplPath, sizeof(oplPath), "mc%d:OPL/conf_opl.cfg", i);
    fd = open(oplPath, O_RDONLY);
    if (fd >= 0) {
        size = read(fd, buffer, sizeof(buffer) - 1);
        close(fd);
        if (size > 0) {
            buffer[size] = '\0';
            for (char *line = buffer; *line;) {
                char *next = strpbrk(line, "\r\n");
                char *separator;

                if (next) {
                    *next++ = '\0';
                    while (*next == '\r' || *next == '\n')
                        next++;
                } else
                    next = line + strlen(line);

                separator = strchr(line, '=');
                if (separator) {
                    *separator = '\0';
                    if (!strcmp(line, "eth_prefix")) {
                        strncpy(configSMBPrefix, separator + 1, sizeof(configSMBPrefix) - 1);
                        configSMBPrefix[sizeof(configSMBPrefix) - 1] = '\0';
                        break;
                    }
                }

                line = next;
            }
        }
    }

    if (!rpcRequest.server[0] || !rpcRequest.share[0]) {
        scr_printf("SMB server or share is missing in %s.\n", configPath);
        return -1;
    }

    scr_printf("Config: %s\n", configPath);
    return 0;
}

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

static int link_is_up(void)
{
    return NetManIoctl(NETMAN_NETIF_IOCTL_GET_LINK_STATUS, NULL, 0, NULL, 0) == NETMAN_NETIF_ETH_LINK_STATE_UP;
}

/* 与 OPL ethWaitValidNetIFLinkState 同思路：最多等约 30 秒 */
static int wait_for_link(void)
{
    int i;

    scr_printf("Waiting link... ");
    for (i = 0; i < 300; i++) {
        if (link_is_up()) {
            scr_printf("UP\n");
            return 0;
        }
        usleep(100000);
    }

    scr_printf("FAIL\n");
    return -1;
}

static int apply_ip_config(void)
{
    t_ip_info ip_info;
    unsigned char ip[4], mask[4], gw[4];
    struct ip4_addr ipaddr, netmask, gateway;
    int result;

    result = ps2ip_getconfig("sm0", &ip_info);
    if (result < 0) {
        scr_printf("ps2ip_getconfig failed: %d\n", result);
        return result;
    }

    if (configUseDHCP) {
        ip_info.dhcp_enabled = 1;
    } else {
        if (parse_ipv4(configPS2IP, ip) < 0 ||
            parse_ipv4(configNetmask, mask) < 0 ||
            parse_ipv4(configGateway, gw) < 0) {
            scr_printf("Bad IP config strings.\n");
            return -1;
        }

        IP4_ADDR(&ipaddr, ip[0], ip[1], ip[2], ip[3]);
        IP4_ADDR(&netmask, mask[0], mask[1], mask[2], mask[3]);
        IP4_ADDR(&gateway, gw[0], gw[1], gw[2], gw[3]);
        ip_addr_set((struct ip4_addr *)&ip_info.ipaddr, &ipaddr);
        ip_addr_set((struct ip4_addr *)&ip_info.netmask, &netmask);
        ip_addr_set((struct ip4_addr *)&ip_info.gw, &gateway);
        ip_info.dhcp_enabled = 0;
    }

    result = ps2ip_setconfig(&ip_info);
    if (result < 0) {
        scr_printf("ps2ip_setconfig failed: %d\n", result);
        return result;
    }

    if (configUseDHCP) {
        int i;

        for (i = 0; i < 300; i++) {
            usleep(100000);
            if (ps2ip_getconfig("sm0", &ip_info) >= 0 && ip_info.dhcp_enabled &&
                (ip_info.dhcp_status == DHCP_STATE_BOUND || ip_info.dhcp_status == DHCP_STATE_OFF))
                break;
        }
        if (i == 300) {
            scr_printf("DHCP timeout.\n");
            return -1;
        }

        scr_printf("DHCP IP %u.%u.%u.%u\n",
                   ip4_addr1((struct ip4_addr *)&ip_info.ipaddr), ip4_addr2((struct ip4_addr *)&ip_info.ipaddr),
                   ip4_addr3((struct ip4_addr *)&ip_info.ipaddr), ip4_addr4((struct ip4_addr *)&ip_info.ipaddr));
    } else {
        scr_printf("IP %u.%u.%u.%u gw %u.%u.%u.%u\n",
                   ip[0], ip[1], ip[2], ip[3], gw[0], gw[1], gw[2], gw[3]);
    }
    return 0;
}

static void printMeasurement(const char *name, const struct smb2test_measurement *measurement)
{
    u32 speed = 0;

    if (measurement->elapsed_ms)
        speed = (measurement->bytes_read / 1024) * 1000 / measurement->elapsed_ms;

    scr_printf("%-9s %4u KiB/s  %5u ms  %08X\n", name, speed, measurement->elapsed_ms, measurement->checksum);
    if (measurement->result)
        scr_printf("  Error: %s (%d)\n", measurement->error[0] ? measurement->error : "Unknown", measurement->result);
}

int main(int argc, char *argv[])
{
    int i, moduleID, moduleResult = 0, waited, statusWaited;
    int testType;
    u32 lastStage;
    static const char *testNames[] = {"SMB1 SEQ", "SMB1 RND", "SMB2 SEQ", "SMB2 RND"};
    static const u8 sectorCacheSizes[] = {0, 6, 12, 24, 48};
    struct smb2test_measurement *measurement;

    (void)argc;
    (void)argv;

    init_scr();
    scr_clear();
    scr_printf("SMB2TEST - OPL IOP SMB1/SMB2 (netman/ps2ip stack)\n\n");

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

    // 主动加载记忆卡模块，不依赖启动 SMB2TEST 的上级程序保留驱动。
    moduleID = SifLoadModule("rom0:SIO2MAN", 0, NULL);
    if (moduleID < 0) {
        scr_printf("SIO2MAN load failed: %d\n", moduleID);
        goto end;
    }
    moduleID = SifLoadModule("rom0:MCMAN", 0, NULL);
    if (moduleID < 0) {
        scr_printf("MCMAN load failed: %d\n", moduleID);
        goto end;
    }
    moduleID = SifLoadModule("rom0:MCSERV", 0, NULL);
    if (moduleID < 0) {
        scr_printf("MCSERV load failed: %d\n", moduleID);
        goto end;
    }
    if (load_opl_network_config() < 0)
        goto end;

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

    /* 与 OPL ethApplyNetIFConfig：先设链路模式，再装 TCP/IP */
    NetManSetLinkMode(NETMAN_NETIF_ETH_LINK_MODE_AUTO);
    if (wait_for_link() < 0)
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

    if (wait_for_link() < 0)
        goto end;
    if (apply_ip_config() < 0)
        goto end;
    usleep(500000);

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

    if (configSMBPrefix[0]) {
        const char *prefix = configSMBPrefix;
        size_t prefixLength;

        while (*prefix == '/' || *prefix == '\\')
            prefix++;
        prefixLength = strlen(prefix);
        while (prefixLength > 0 && (prefix[prefixLength - 1] == '/' || prefix[prefixLength - 1] == '\\'))
            prefixLength--;

        if (prefixLength > 0)
            snprintf(rpcRequest.path, sizeof(rpcRequest.path), "%.*s/%s", (int)prefixLength, prefix, SMB2TEST_DIRECTORY);
        else
            strncpy(rpcRequest.path, SMB2TEST_DIRECTORY, sizeof(rpcRequest.path) - 1);
    } else
        strncpy(rpcRequest.path, SMB2TEST_DIRECTORY, sizeof(rpcRequest.path) - 1);
    rpcRequest.path[sizeof(rpcRequest.path) - 1] = '\0';
    for (i = 0; rpcRequest.path[i]; i++) {
        if (rpcRequest.path[i] == '\\')
            rpcRequest.path[i] = '/';
    }
    rpcRequest.offset_low = SMB2TEST_OFFSET_LOW;
    rpcRequest.offset_high = SMB2TEST_OFFSET_HIGH;
    rpcRequest.read_size = SMB2TEST_READ_SIZE;
    rpcRequest.read_count = SMB2TEST_READ_COUNT;

    scr_printf("Server: %s:%u/%s\n", rpcRequest.server, rpcRequest.port, rpcRequest.share);
    scr_printf("File: first ISO in %s\n", rpcRequest.path);
    scr_printf("Read: %u x %u bytes\n\n", rpcRequest.read_count, rpcRequest.read_size);
    scr_clear();
    scr_printf("Testing...\n\n");
    scr_printf("Protocol  Speed       Time       Checksum\n");
    for (i = 0; i < (int)(sizeof(sectorCacheSizes) / sizeof(sectorCacheSizes[0])) * 2 + 2; i++) {
        testType = i & 1 ? SMB2TEST_TYPE_SMB2_SEQUENTIAL : SMB2TEST_TYPE_SMB1_SEQUENTIAL;
        if (i && !(i & 1))
            scr_printf("\n");
        rpcRequest.test_type = testType;
        if (i < (int)(sizeof(sectorCacheSizes) / sizeof(sectorCacheSizes[0])) * 2) {
            if (!(i & 1))
                scr_printf("Sector cache: %u\n", sectorCacheSizes[i / 2]);
            rpcRequest.sector_cache_size = sectorCacheSizes[i / 2];
            rpcRequest.read_size = SMB2TEST_READ_SIZE;
            rpcRequest.read_count = SMB2TEST_READ_COUNT;
        } else {
            if (!(i & 1))
                scr_printf("Read size: 96 KiB\n");
            rpcRequest.sector_cache_size = 0;
            rpcRequest.read_size = 96 * 1024;
            rpcRequest.read_count = SMB2TEST_READ_SIZE * SMB2TEST_READ_COUNT / rpcRequest.read_size;
        }

        memset(&rpcResult, 0, sizeof(rpcResult));
        moduleResult = SifCallRpc(&rpcClient, SMB2TEST_CMD_RUN, 0, &rpcRequest, sizeof(rpcRequest), &rpcResult, sizeof(rpcResult), NULL, NULL);
        if (moduleResult < 0) {
            scr_printf("SifCallRpc failed: %d\n", moduleResult);
            goto end;
        }

        lastStage = SMB2TEST_STAGE_NONE;
        for (waited = 0; waited < 1200; waited++) {
            if (rpcResult.stage == SMB2TEST_STAGE_DONE)
                break;

            usleep(100000);
            memset(&rpcResult, 0, sizeof(rpcResult));
            moduleResult = SifCallRpc(&rpcClient, SMB2TEST_CMD_STATUS, SIF_RPC_M_NOWAIT, NULL, 0, &rpcResult, sizeof(rpcResult), NULL, NULL);
            if (moduleResult < 0) {
                scr_printf("Status RPC failed: %d\n", moduleResult);
                goto end;
            }

            for (statusWaited = 0; statusWaited < 100 && SifCheckStatRpc(&rpcClient); statusWaited++) {
                usleep(10000);
            }
            if (statusWaited == 100) {
                if (lastStage == SMB2TEST_STAGE_CONNECT)
                    scr_printf("TCP connection timed out\n");
                else if (lastStage == SMB2TEST_STAGE_NEGOTIATE)
                    scr_printf("SMB2 support unknown, negotiate timed out\n");
                else if (lastStage == SMB2TEST_STAGE_SESSION)
                    scr_printf("SESSION send blocked\n");
                else if (lastStage == SMB2TEST_STAGE_SESSION_WAIT)
                    scr_printf("SESSION response timed out\n");
                else if (lastStage == SMB2TEST_STAGE_SESSION_RETRY)
                    scr_printf("SESSION send retry stalled\n");
                else if (lastStage == SMB2TEST_STAGE_TREE)
                    scr_printf("SMB2 supported, share timed out\n");
                scr_printf("Status RPC timeout at stage: %u\n", lastStage);
                goto end;
            }

            if (rpcRequest.test_type >= SMB2TEST_TYPE_SMB2_SEQUENTIAL && rpcResult.stage != lastStage && rpcResult.stage != SMB2TEST_STAGE_DONE)
                lastStage = rpcResult.stage;
        }

        if (waited == 1200) {
            if (rpcResult.stage == SMB2TEST_STAGE_CONNECT)
                scr_printf("TCP connection timed out\n");
            else if (rpcResult.stage == SMB2TEST_STAGE_NEGOTIATE)
                scr_printf("SMB2 support unknown, negotiate timed out\n");
            else if (rpcResult.stage == SMB2TEST_STAGE_SESSION)
                scr_printf("SESSION send blocked\n");
            else if (rpcResult.stage == SMB2TEST_STAGE_SESSION_WAIT)
                scr_printf("SESSION response timed out\n");
            else if (rpcResult.stage == SMB2TEST_STAGE_SESSION_RETRY)
                scr_printf("SESSION send retry stalled\n");
            else if (rpcResult.stage == SMB2TEST_STAGE_TREE)
                scr_printf("SMB2 supported, share timed out\n");
            scr_printf("Test timeout at stage: %u\n", rpcResult.stage);
            goto end;
        }

        if (rpcResult.error[0]) {
            scr_printf("Test failed: %s (%d)\n", rpcResult.error, rpcResult.result);
            goto end;
        }

        if (rpcRequest.test_type == SMB2TEST_TYPE_SMB1_SEQUENTIAL)
            measurement = &rpcResult.smb1_sequential;
        else if (rpcRequest.test_type == SMB2TEST_TYPE_SMB1_RANDOM)
            measurement = &rpcResult.smb1_random;
        else if (rpcRequest.test_type == SMB2TEST_TYPE_SMB2_SEQUENTIAL)
            measurement = &rpcResult.smb2_sequential;
        else
            measurement = &rpcResult.smb2_random;

        printMeasurement(testNames[testType], measurement);
        if (i + 1 < (int)(sizeof(sectorCacheSizes) / sizeof(sectorCacheSizes[0])) * 2 + 2)
            usleep(1000000);
    }

    scr_printf("\nSMB2 dialect: %04X\n", rpcResult.dialect);

end:
    scr_printf("\nTest stopped. Reset the console to run again.\n");
    SleepThread();
    return 0;
}
