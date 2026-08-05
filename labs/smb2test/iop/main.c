#include <irx.h>
#include <sifcmd.h>
#include <smstcpip.h>

#include "../smb2test.h"
#include "smb2test_internal.h"

IRX_ID("smb2test", 1, 1);

typedef struct
{
    int version;
    void **exports;
} modinfo_t;

int (*plwip_close)(int s);
int (*plwip_connect)(int s, struct sockaddr *name, socklen_t namelen);
int (*plwip_recv)(int s, void *mem, int len, unsigned int flags);
int (*plwip_recvfrom)(int s, void *mem, int hlen, void *payload, int plen, unsigned int flags, struct sockaddr *from, socklen_t *fromlen);
int (*plwip_send)(int s, void *dataptr, int size, unsigned int flags);
int (*plwip_socket)(int domain, int type, int protocol);
int (*plwip_select)(int maxfdp1, fd_set *readset, fd_set *writeset, fd_set *exceptset, struct timeval *timeout);
int (*plwip_ioctl)(int s, long cmd, void *argp);
int (*plwip_getsockopt)(int s, int level, int optname, void *optval, socklen_t *optlen);
int (*plwip_setsockopt)(int s, int level, int optname, const void *optval, socklen_t optlen);
int (*plwip_shutdown)(int s, int how);
int (*plwip_fcntl)(int s, int cmd, int val);
u32 (*pinet_addr)(const char *cp);

struct cdvdman_settings_smb cdvdman_settings;

extern char smb2TestError[64];
extern u32 smb2TestDialect;
extern u32 smb2TestMaxRead;

static SifRpcDataQueue_t rpcQueue;
static SifRpcServerData_t rpcServer;
static unsigned char rpcBuffer[sizeof(struct smb2test_request)] __attribute__((aligned(16)));
static struct smb2test_result rpcResult __attribute__((aligned(16)));
static struct smb2test_request smbWorkerRequest;
static int smbWorkerStartSema = -1;
static int smbWorkerThreadID = -1;
static volatile int smbWorkerBusy;
static char smbTestPath[sizeof(smbWorkerRequest.path)];

void smb2TestSetStage(u32 stage)
{
    rpcResult.stage = stage;
    if (stage == SMB2TEST_STAGE_SESSION)
        rpcResult.dialect = smb2TestDialect;
}

static int getModInfo(char *modname, modinfo_t *info)
{
    iop_library_t *libptr;
    int i;

    libptr = GetLoadcoreInternalData()->let_next;
    while (libptr) {
        for (i = 0; i < 8; i++) {
            if (libptr->name[i] != modname[i])
                break;
        }
        if (i == 8)
            break;
        libptr = libptr->prev;
    }

    if (!libptr)
        return 0;

    info->version = libptr->version;
    info->exports = (void **)(((struct irx_export_table *)libptr)->fptrs);
    return 1;
}

static int initPS2IP(void)
{
    modinfo_t info;

    if (!getModInfo("ps2ip\0\0\0", &info))
        return -ENXIO;

    /* 与 PS2SDK ps2ip-nm / SMSTCPIP 的 socket 导出口径一致 */
    plwip_close = info.exports[6];
    plwip_connect = info.exports[7];
    plwip_recv = info.exports[9];
    plwip_recvfrom = info.exports[10];
    plwip_send = info.exports[11];
    plwip_socket = info.exports[13];
    plwip_select = info.exports[14];
    plwip_ioctl = info.exports[15];
    plwip_getsockopt = info.exports[18];
    plwip_setsockopt = info.exports[19];
    pinet_addr = info.exports[24]; /* SMSTCPIP: inet_addr；ps2ip-nm: ipaddr_addr */
    plwip_shutdown = info.exports[46];
    plwip_fcntl = info.exports[47]; /* ps2ip-nm: lwip_fcntl */
    return 0;
}

typedef int (*smb_read_func_t)(u16 FID, u32 offsetlow, u32 offsethigh, void *readbuf, int nbytes);

static void runReadTest(const struct smb2test_request *request, u16 fid, unsigned char *readBuffer, int random, smb_read_func_t readFunction, struct smb2test_measurement *measurement, u16 sectorCacheSize)
{
    iop_sys_clock_t start, end, elapsed, segmentStart;
    unsigned char *sectorCache = NULL;
    u32 seconds, microseconds;
    u32 offsetLow, offsetHigh;
    u32 cacheOffsetLow = 0, cacheOffsetHigh = 0xFFFFFFFF;
    u32 cacheSize = sectorCacheSize * 2048;
    u32 block, delta;
    u32 i, j, segmentReadCount;

    memset(measurement, 0, sizeof(*measurement));
    measurement->result = -EIO;
    measurement->checksum = 2166136261u;
    segmentReadCount = request->read_count / SMB2TEST_SEGMENT_COUNT;
    if (sectorCacheSize) {
        sectorCache = AllocSysMemory(ALLOC_FIRST, cacheSize, NULL);
        if (!sectorCache) {
            measurement->result = -ENOMEM;
            strcpy(measurement->error, "Sector cache allocation failed");
            return;
        }
    }

    GetSystemTime(&start);
    segmentStart = start;
    for (i = 0; i < request->read_count; i++) {
        if (random) {
            block = (i * 2654435761u) % request->read_count;
            delta = block * request->read_size;
            offsetLow = request->offset_low + delta;
            offsetHigh = request->offset_high + (offsetLow < request->offset_low);
        } else {
            offsetLow = request->offset_low + i * request->read_size;
            offsetHigh = request->offset_high + (offsetLow < request->offset_low);
        }

        if (sectorCache && request->read_size < cacheSize) {
            if (cacheOffsetHigh == 0xFFFFFFFF || offsetHigh != cacheOffsetHigh || offsetLow < cacheOffsetLow || offsetLow - cacheOffsetLow + request->read_size > cacheSize) {
                measurement->last_read_size = readFunction(fid, offsetLow, offsetHigh, sectorCache, cacheSize);
                if (measurement->last_read_size == (s32)cacheSize) {
                    cacheOffsetLow = offsetLow;
                    cacheOffsetHigh = offsetHigh;
                }
            }
            if (cacheOffsetHigh != 0xFFFFFFFF && offsetHigh == cacheOffsetHigh && offsetLow >= cacheOffsetLow && offsetLow - cacheOffsetLow + request->read_size <= cacheSize) {
                memcpy(readBuffer, &sectorCache[offsetLow - cacheOffsetLow], request->read_size);
                measurement->last_read_size = request->read_size;
            }
        } else
            measurement->last_read_size = readFunction(fid, offsetLow, offsetHigh, readBuffer, request->read_size);
        if (measurement->last_read_size != (s32)request->read_size) {
            measurement->result = measurement->last_read_size;
            strcpy(measurement->error, random ? "Random read failed" : "Sequential read failed");
            break;
        }

        for (j = 0; j < request->read_size; j++) {
            measurement->checksum ^= readBuffer[j];
            measurement->checksum *= 16777619u;
        }
        measurement->bytes_read += request->read_size;

        if ((i + 1) % segmentReadCount == 0) {
            GetSystemTime(&end);
            elapsed.lo = end.lo - segmentStart.lo;
            elapsed.hi = end.hi - segmentStart.hi - (segmentStart.lo > end.lo);
            SysClock2USec(&elapsed, &seconds, &microseconds);
            measurement->segment_speed[i / segmentReadCount] = (request->read_size / 1024) * segmentReadCount * 1000 / (seconds * 1000 + microseconds / 1000);
            segmentStart = end;
        }
    }
    if (i == request->read_count)
        measurement->result = 0;

    GetSystemTime(&end);
    elapsed.lo = end.lo - start.lo;
    elapsed.hi = end.hi - start.hi - (start.lo > end.lo);
    SysClock2USec(&elapsed, &seconds, &microseconds);
    measurement->elapsed_ms = seconds * 1000 + microseconds / 1000;
    if (sectorCache)
        FreeSysMemory(sectorCache);
}

static void runSMB1Test(const struct smb2test_request *request, unsigned char *readBuffer, int random, struct smb2test_measurement *measurement)
{
    u32 capabilities = 0;
    u16 fid = 0xFFFF;
    char share[64];
    char path[sizeof(request->path) + 2];
    int i;

    memset(measurement, 0, sizeof(*measurement));
    rpcResult.stage = SMB2TEST_STAGE_CONNECT;
    measurement->result = smb1_NegotiateProtocol((char *)request->server, request->port, (char *)request->user, (char *)request->password, &capabilities, SmbInitHashPassword);
    if (measurement->result <= 0) {
        strcpy(measurement->error, "SMB1 negotiate failed");
        goto cleanup;
    }

    measurement->result = smb1_SessionSetupAndX(capabilities);
    if (measurement->result <= 0) {
        strcpy(measurement->error, "SMB1 session failed");
        goto cleanup;
    }

    sprintf(share, "\\\\%s\\%s", request->server, request->share);
    measurement->result = smb1_TreeConnectAndX(share);
    if (measurement->result <= 0) {
        strcpy(measurement->error, "SMB1 tree connect failed");
        goto cleanup;
    }

    if (!smbTestPath[0]) {
        measurement->result = smb1_FindFirstISO(request->path, smbTestPath, sizeof(smbTestPath));
        if (measurement->result < 0) {
            strcpy(measurement->error, "No ISO found in DVD folder");
            goto cleanup;
        }
    }

    path[0] = '\\';
    strncpy(&path[1], smbTestPath, sizeof(path) - 2);
    path[sizeof(path) - 1] = '\0';
    for (i = 1; path[i]; i++) {
        if (path[i] == '/')
            path[i] = '\\';
    }

    rpcResult.stage = SMB2TEST_STAGE_OPEN;
    measurement->result = smb1_OpenAndX(path, (u8 *)&fid, 0);
    if (measurement->result <= 0) {
        strcpy(measurement->error, "SMB1 file open failed");
        goto cleanup;
    }

    rpcResult.stage = SMB2TEST_STAGE_READ;
    runReadTest(request, fid, readBuffer, random, smb1_ReadFile, measurement, request->sector_cache_size);

cleanup:
    rpcResult.stage = SMB2TEST_STAGE_CLOSE;
    if (fid != 0xFFFF)
        smb1_Close(fid);
    smb1_Disconnect();
}

static void runSMB2Test(const struct smb2test_request *request, unsigned char *readBuffer, int random, struct smb2test_measurement *measurement)
{
    u32 capabilities = 0;
    u16 fid = 0xFFFF;

    memset(measurement, 0, sizeof(*measurement));
    rpcResult.stage = SMB2TEST_STAGE_CONNECT;
    measurement->result = smb_NegotiateProtocol((char *)request->server, request->port, (char *)request->user, (char *)request->password, &capabilities, NULL);
    if (measurement->result <= 0) {
        rpcResult.dialect = smb2TestDialect;
        if (rpcResult.stage == SMB2TEST_STAGE_CONNECT)
            strcpy(measurement->error, "TCP connection failed");
        else if (rpcResult.stage == SMB2TEST_STAGE_NEGOTIATE && !smb2TestDialect)
            strcpy(measurement->error, "SMB2 dialect not supported");
        else if (rpcResult.stage == SMB2TEST_STAGE_SESSION)
            strcpy(measurement->error, "SMB2 supported, login failed");
        else if (rpcResult.stage == SMB2TEST_STAGE_TREE)
            strcpy(measurement->error, "SMB2 supported, share failed");
        else if (smb2TestDialect)
            strcpy(measurement->error, "SMB2 supported, connection failed");
        else if (smb2TestError[0]) {
            strncpy(measurement->error, smb2TestError, sizeof(measurement->error));
            measurement->error[sizeof(measurement->error) - 1] = '\0';
        } else
            strcpy(measurement->error, "SMB2 connection failed");
        goto cleanup;
    }

    rpcResult.dialect = smb2TestDialect;
    rpcResult.max_read_size = smb2TestMaxRead;
    rpcResult.stage = SMB2TEST_STAGE_OPEN;
    if (!smbTestPath[0]) {
        measurement->result = smb_FindFirstISO(request->path, smbTestPath, sizeof(smbTestPath));
        if (measurement->result < 0) {
            strcpy(measurement->error, "No ISO found in DVD folder");
            goto cleanup;
        }
    }
    measurement->result = smb_OpenAndX(smbTestPath, (u8 *)&fid, 0);
    if (measurement->result <= 0) {
        strcpy(measurement->error, "SMB2 file open failed");
        goto cleanup;
    }

    smb_SetReadAhead(48);
    rpcResult.stage = SMB2TEST_STAGE_READ;
    runReadTest(request, fid, readBuffer, random, smb_ReadFile, measurement, request->sector_cache_size);

cleanup:
    rpcResult.stage = SMB2TEST_STAGE_CLOSE;
    if (fid != 0xFFFF)
        smb_Close(fid);
    smb_Disconnect();
}

static void runTest(const struct smb2test_request *request)
{
    unsigned char *readBuffer = NULL;
    struct smb2test_measurement *measurement;

    memset(&rpcResult, 0, sizeof(rpcResult));
    rpcResult.result = -EIO;

    rpcResult.stage = SMB2TEST_STAGE_NETWORK;
    if (initPS2IP() < 0) {
        strcpy(rpcResult.error, "PS2IP export table not found");
        goto cleanup;
    }

    rpcResult.stage = SMB2TEST_STAGE_MEMORY;
    if (!request->read_size || request->read_size > 96 * 1024 || !request->read_count || request->test_type >= SMB2TEST_TYPE_COUNT) {
        rpcResult.result = -EINVAL;
        strcpy(rpcResult.error, "Invalid read parameters");
        goto cleanup;
    }

    readBuffer = AllocSysMemory(ALLOC_FIRST, request->read_size, NULL);
    if (!readBuffer) {
        rpcResult.result = -ENOMEM;
        strcpy(rpcResult.error, "IOP read buffer allocation failed");
        goto cleanup;
    }

    strncpy(cdvdman_settings.smb_share, request->share, sizeof(cdvdman_settings.smb_share));
    cdvdman_settings.smb_share[sizeof(cdvdman_settings.smb_share) - 1] = '\0';

    if (request->test_type == SMB2TEST_TYPE_SMB1_SEQUENTIAL) {
        measurement = &rpcResult.smb1_sequential;
        runSMB1Test(request, readBuffer, 0, measurement);
    } else if (request->test_type == SMB2TEST_TYPE_SMB1_RANDOM) {
        measurement = &rpcResult.smb1_random;
        runSMB1Test(request, readBuffer, 1, measurement);
    } else if (request->test_type == SMB2TEST_TYPE_SMB2_SEQUENTIAL) {
        measurement = &rpcResult.smb2_sequential;
        runSMB2Test(request, readBuffer, 0, measurement);
    } else {
        measurement = &rpcResult.smb2_random;
        runSMB2Test(request, readBuffer, 1, measurement);
    }

    rpcResult.result = measurement->result;

cleanup:
    if (readBuffer)
        FreeSysMemory(readBuffer);
    rpcResult.stage = SMB2TEST_STAGE_DONE;
}

/* libsmb2 在 smb2_write/read_to_socket 里有 iovec[256] 栈数组；RPC 线程栈不够会
 * 在 send 返回后跑飞（PCSX2：宿主崩溃 / 解释器 Unimplemented op f0000102）。 */
static void smbWorkerThread(void *arg)
{
    (void)arg;

    // 常驻并复用同一个大栈线程，避免重复创建造成IOP内存碎片。
    while (1) {
        WaitSema(smbWorkerStartSema);
        runTest(&smbWorkerRequest);
        smbWorkerBusy = 0;
    }
}

static void *rpcHandler(int function, void *buffer, int size)
{
    iop_thread_t thread;
    int result;

    if (function == SMB2TEST_CMD_RUN && (unsigned int)size >= sizeof(struct smb2test_request)) {
        if (smbWorkerBusy)
            return &rpcResult;

        if (smbWorkerThreadID < 0) {
            thread.attr = TH_C;
            thread.option = SMB2TEST_RPC_ID;
            thread.thread = smbWorkerThread;
            thread.priority = 0x21;
            thread.stacksize = 0x40000; /* 256KB：避开 libsmb2 iovec[256] 栈溢出 */
            smbWorkerThreadID = CreateThread(&thread);
            if (smbWorkerThreadID < 0) {
                memset(&rpcResult, 0, sizeof(rpcResult));
                rpcResult.result = smbWorkerThreadID;
                rpcResult.stage = SMB2TEST_STAGE_DONE;
                strcpy(rpcResult.error, "SMB worker thread create failed");
                return &rpcResult;
            }
            result = StartThread(smbWorkerThreadID, NULL);
            if (result < 0) {
                DeleteThread(smbWorkerThreadID);
                smbWorkerThreadID = -1;
                memset(&rpcResult, 0, sizeof(rpcResult));
                rpcResult.result = result;
                rpcResult.stage = SMB2TEST_STAGE_DONE;
                strcpy(rpcResult.error, "SMB worker thread start failed");
                return &rpcResult;
            }
        }

        memcpy(&smbWorkerRequest, buffer, sizeof(smbWorkerRequest));
        memset(&rpcResult, 0, sizeof(rpcResult));
        rpcResult.result = -EINPROGRESS;
        smbWorkerBusy = 1;
        SignalSema(smbWorkerStartSema);
    } else if (function != SMB2TEST_CMD_STATUS) {
        memset(&rpcResult, 0, sizeof(rpcResult));
        rpcResult.result = -EINVAL;
        rpcResult.stage = SMB2TEST_STAGE_DONE;
        strcpy(rpcResult.error, "Invalid SMB2TEST RPC request");
    }

    return &rpcResult;
}

static void rpcThread(void *arg)
{
    (void)arg;

    sceSifSetRpcQueue(&rpcQueue, GetThreadId());
    sceSifRegisterRpc(&rpcServer, SMB2TEST_RPC_ID, rpcHandler, rpcBuffer, NULL, NULL, &rpcQueue);
    sceSifRpcLoop(&rpcQueue);
}

int _start(int argc, char *argv[])
{
    iop_thread_t thread;
    iop_sema_t sema;
    int threadID;

    (void)argc;
    (void)argv;

    sema.attr = 1;
    sema.option = 0;
    sema.initial = 0;
    sema.max = 1;
    smbWorkerStartSema = CreateSema(&sema);
    if (smbWorkerStartSema < 0)
        return MODULE_NO_RESIDENT_END;

    thread.attr = TH_C;
    thread.option = SMB2TEST_RPC_ID;
    thread.thread = rpcThread;
    thread.priority = 0x20;
    thread.stacksize = 0x4000;
    threadID = CreateThread(&thread);
    if (threadID < 0) {
        DeleteSema(smbWorkerStartSema);
        return MODULE_NO_RESIDENT_END;
    }

    StartThread(threadID, NULL);
    return MODULE_RESIDENT_END;
}
