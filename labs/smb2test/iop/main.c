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

extern smb2_diag_t smb2Diag;

static SifRpcDataQueue_t rpcQueue;
static SifRpcServerData_t rpcServer;
static unsigned char rpcBuffer[sizeof(struct smb2test_request)] __attribute__((aligned(16)));
static struct smb2test_result rpcResult __attribute__((aligned(16)));
static int smbWorkerSema = -1;

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

static void runReadTest(const struct smb2test_request *request, u16 fid, unsigned char *readBuffer, int random, smb_read_func_t readFunction, struct smb2test_measurement *measurement)
{
    iop_sys_clock_t start, end, elapsed;
    u32 seconds, microseconds;
    u32 offsetLow, offsetHigh;
    u32 block, delta;
    u32 i, j;

    memset(measurement, 0, sizeof(*measurement));
    measurement->result = -EIO;
    measurement->checksum = 2166136261u;

    GetSystemTime(&start);
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
    }
    if (i == request->read_count)
        measurement->result = 0;

    GetSystemTime(&end);
    elapsed.lo = end.lo - start.lo;
    elapsed.hi = end.hi - start.hi - (start.lo > end.lo);
    SysClock2USec(&elapsed, &seconds, &microseconds);
    measurement->elapsed_ms = seconds * 1000 + microseconds / 1000;
}

static void runSMB1Test(const struct smb2test_request *request, unsigned char *readBuffer, int random, struct smb2test_measurement *measurement)
{
    u32 capabilities = 0;
    u16 fid = 0xFFFF;
    char share[64];
    char path[sizeof(request->path) + 2];
    int i;

    memset(measurement, 0, sizeof(*measurement));
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

    path[0] = '\\';
    strncpy(&path[1], request->path, sizeof(path) - 2);
    path[sizeof(path) - 1] = '\0';
    for (i = 1; path[i]; i++) {
        if (path[i] == '/')
            path[i] = '\\';
    }

    measurement->result = smb1_OpenAndX(path, (u8 *)&fid, 0);
    if (measurement->result <= 0) {
        strcpy(measurement->error, "SMB1 file open failed");
        goto cleanup;
    }

    runReadTest(request, fid, readBuffer, random, smb1_ReadFile, measurement);

cleanup:
    if (fid != 0xFFFF)
        smb1_Close(fid);
    smb1_Disconnect();
}

static void runSMB2Test(const struct smb2test_request *request, unsigned char *readBuffer, int random, struct smb2test_measurement *measurement)
{
    u32 capabilities = 0;
    u16 fid = 0xFFFF;

    memset(measurement, 0, sizeof(*measurement));
    measurement->result = smb_NegotiateProtocol((char *)request->server, request->port, (char *)request->user, (char *)request->password, &capabilities, NULL);
    if (measurement->result <= 0) {
        if (smb2Diag.error[0]) {
            strncpy(measurement->error, smb2Diag.error, sizeof(measurement->error));
            measurement->error[sizeof(measurement->error) - 1] = '\0';
        } else
            strcpy(measurement->error, "SMB2 connection failed");
        goto cleanup;
    }

    rpcResult.dialect = smb2Diag.dialect;
    rpcResult.max_read_size = smb2Diag.max_read_size;
    measurement->result = smb_OpenAndX((char *)request->path, (u8 *)&fid, 0);
    if (measurement->result <= 0) {
        strcpy(measurement->error, "SMB2 file open failed");
        goto cleanup;
    }

    runReadTest(request, fid, readBuffer, random, smb_ReadFile, measurement);
    if (measurement->result < 0 && smb2Diag.error[0]) {
        strncpy(measurement->error, smb2Diag.error, sizeof(measurement->error));
        measurement->error[sizeof(measurement->error) - 1] = '\0';
    }

cleanup:
    if (fid != 0xFFFF)
        smb_Close(fid);
    smb_Disconnect();
}

static void runTest(const struct smb2test_request *request)
{
    int socketID;
    int socketError;
    int opt;
    socklen_t socketErrorLength;
    struct sockaddr_in serverAddress;
    u32 serverIP;
    unsigned char *readBuffer = NULL;

    memset(&rpcResult, 0, sizeof(rpcResult));
    rpcResult.result = -EIO;

    rpcResult.stage = SMB2TEST_STAGE_NETWORK;
    if (initPS2IP() < 0) {
        strcpy(rpcResult.error, "PS2IP export table not found");
        goto cleanup;
    }

    rpcResult.stage = SMB2TEST_STAGE_MEMORY;
    if (!request->read_size || request->read_size > 65536 || !request->read_count) {
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

    rpcResult.stage = SMB2TEST_STAGE_CONNECT;
    serverIP = pinet_addr(request->server);
    socketID = plwip_socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketID < 0) {
        rpcResult.result = socketID;
        sprintf(rpcResult.error, "TCP socket failed: %d", socketID);
        goto cleanup;
    }

    opt = 1;
    plwip_setsockopt(socketID, IPPROTO_TCP, TCP_NODELAY, (char *)&opt, sizeof(opt));
    memset(&serverAddress, 0, sizeof(serverAddress));
    serverAddress.sin_len = sizeof(serverAddress);
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(request->port);
    serverAddress.sin_addr.s_addr = serverIP;
    rpcResult.result = plwip_connect(socketID, (struct sockaddr *)&serverAddress, sizeof(serverAddress));
    if (rpcResult.result < 0) {
        socketError = 0;
        socketErrorLength = sizeof(socketError);
        plwip_getsockopt(socketID, SOL_SOCKET, SO_ERROR, &socketError, &socketErrorLength);
        plwip_close(socketID);
        sprintf(rpcResult.error, "TCP fail r=%ld so=%d", rpcResult.result, socketError);
        goto cleanup;
    }
    plwip_close(socketID);

    rpcResult.stage = SMB2TEST_STAGE_READ;
    runSMB1Test(request, readBuffer, 0, &rpcResult.smb1_sequential);
    runSMB2Test(request, readBuffer, 0, &rpcResult.smb2_sequential);
    runSMB2Test(request, readBuffer, 1, &rpcResult.smb2_random);
    runSMB1Test(request, readBuffer, 1, &rpcResult.smb1_random);

    printf("SMB2TEST: SMB1 SEQ r=%ld bytes=%lu ms=%lu sum=%08lx\n", rpcResult.smb1_sequential.result, rpcResult.smb1_sequential.bytes_read, rpcResult.smb1_sequential.elapsed_ms, rpcResult.smb1_sequential.checksum);
    printf("SMB2TEST: SMB2 SEQ r=%ld bytes=%lu ms=%lu sum=%08lx\n", rpcResult.smb2_sequential.result, rpcResult.smb2_sequential.bytes_read, rpcResult.smb2_sequential.elapsed_ms, rpcResult.smb2_sequential.checksum);
    printf("SMB2TEST: SMB1 RND r=%ld bytes=%lu ms=%lu sum=%08lx\n", rpcResult.smb1_random.result, rpcResult.smb1_random.bytes_read, rpcResult.smb1_random.elapsed_ms, rpcResult.smb1_random.checksum);
    printf("SMB2TEST: SMB2 RND r=%ld bytes=%lu ms=%lu sum=%08lx\n", rpcResult.smb2_random.result, rpcResult.smb2_random.bytes_read, rpcResult.smb2_random.elapsed_ms, rpcResult.smb2_random.checksum);

    rpcResult.result = rpcResult.smb1_sequential.result || rpcResult.smb1_random.result ||
                       rpcResult.smb2_sequential.result || rpcResult.smb2_random.result ? -EIO : 0;
    rpcResult.stage = SMB2TEST_STAGE_DONE;

cleanup:
    if (readBuffer)
        FreeSysMemory(readBuffer);
}

/* libsmb2 在 smb2_write/read_to_socket 里有 iovec[256] 栈数组；RPC 线程栈不够会
 * 在 send 返回后跑飞（PCSX2：宿主崩溃 / 解释器 Unimplemented op f0000102）。 */
static void smbWorkerThread(void *arg)
{
    iop_thread_info_t info;

    printf("SMB2TEST: worker begin\n");
    memset(&info, 0, sizeof(info));
    if (ReferThreadStatus(TH_SELF, &info) == 0)
        printf("SMB2TEST: worker stack=%d remain=%d\n", info.stackSize, CheckThreadStack());
    runTest((const struct smb2test_request *)arg);
    printf("SMB2TEST: worker end\n");
    SignalSema(smbWorkerSema);
    ExitDeleteThread();
}

static void *rpcHandler(int function, void *buffer, int size)
{
    iop_thread_t thread;
    int threadID;

    if (function == SMB2TEST_CMD_RUN && (unsigned int)size >= sizeof(struct smb2test_request)) {
        thread.attr = TH_C;
        thread.option = SMB2TEST_RPC_ID;
        thread.thread = smbWorkerThread;
        thread.priority = 0x21;
        thread.stacksize = 0x40000; /* 256KB：避开 libsmb2 iovec[256] 栈溢出 */
        threadID = CreateThread(&thread);
        if (threadID < 0) {
            memset(&rpcResult, 0, sizeof(rpcResult));
            rpcResult.result = threadID;
            strcpy(rpcResult.error, "SMB worker thread create failed");
            return &rpcResult;
        }
        StartThread(threadID, buffer);
        WaitSema(smbWorkerSema);
    } else {
        memset(&rpcResult, 0, sizeof(rpcResult));
        rpcResult.result = -EINVAL;
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
    smbWorkerSema = CreateSema(&sema);
    if (smbWorkerSema < 0)
        return MODULE_NO_RESIDENT_END;

    thread.attr = TH_C;
    thread.option = SMB2TEST_RPC_ID;
    thread.thread = rpcThread;
    thread.priority = 0x20;
    thread.stacksize = 0x4000;
    threadID = CreateThread(&thread);
    if (threadID < 0)
        return MODULE_NO_RESIDENT_END;

    StartThread(threadID, NULL);
    return MODULE_RESIDENT_END;
}
