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
int (*plwip_send)(int s, void *dataptr, int size, unsigned int flags);
int (*plwip_socket)(int domain, int type, int protocol);
int (*plwip_select)(int maxfdp1, fd_set *readset, fd_set *writeset, fd_set *exceptset, struct timeval *timeout);
int (*plwip_ioctl)(int s, long cmd, void *argp);
int (*plwip_getsockopt)(int s, int level, int optname, void *optval, socklen_t *optlen);
int (*plwip_setsockopt)(int s, int level, int optname, const void *optval, socklen_t optlen);
int (*plwip_shutdown)(int s, int how);
u32 (*pinet_addr)(const char *cp);

struct cdvdman_settings_smb cdvdman_settings;

extern smb2_diag_t smb2Diag;

static SifRpcDataQueue_t rpcQueue;
static SifRpcServerData_t rpcServer;
static unsigned char rpcBuffer[sizeof(struct smb2test_request)] __attribute__((aligned(16)));
static struct smb2test_result rpcResult __attribute__((aligned(16)));

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

    plwip_close = info.exports[6];
    plwip_connect = info.exports[7];
    plwip_recv = info.exports[9];
    plwip_send = info.exports[11];
    plwip_socket = info.exports[13];
    plwip_select = info.exports[14];
    plwip_ioctl = info.exports[15];
    plwip_getsockopt = info.exports[18];
    plwip_setsockopt = info.exports[19];
    pinet_addr = info.exports[24];
    plwip_shutdown = info.exports[46];
    return 0;
}

static void runTest(const struct smb2test_request *request)
{
    iop_sys_clock_t start, end, elapsed;
    u32 capabilities = 0;
    u32 seconds, microseconds;
    u32 offsetLow = request->offset_low;
    u32 offsetHigh = request->offset_high;
    u32 i, j;
    u16 fid = 0xFFFF;
    int closeResult;
    unsigned char *readBuffer = NULL;

    memset(&rpcResult, 0, sizeof(rpcResult));
    rpcResult.result = -EIO;
    rpcResult.checksum = 2166136261u;

    rpcResult.stage = SMB2TEST_STAGE_NETWORK;
    if (initPS2IP() < 0) {
        strcpy(rpcResult.error, "PS2IP export table not found");
        goto cleanup;
    }

    rpcResult.stage = SMB2TEST_STAGE_MEMORY;
    if (!request->read_size || request->read_size > 65536) {
        rpcResult.result = -EINVAL;
        strcpy(rpcResult.error, "Invalid read size");
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
    rpcResult.result = smb_NegotiateProtocol((char *)request->server, request->port, (char *)request->user, (char *)request->password, &capabilities, NULL);
    if (rpcResult.result <= 0) {
        strcpy(rpcResult.error, "SMB2 share connection failed");
        goto cleanup;
    }

    rpcResult.stage = SMB2TEST_STAGE_OPEN;
    rpcResult.result = smb_OpenAndX((char *)request->path, (u8 *)&fid, 0);
    if (rpcResult.result <= 0) {
        strcpy(rpcResult.error, "SMB2 file open failed");
        goto cleanup;
    }

    rpcResult.stage = SMB2TEST_STAGE_READ;
    GetSystemTime(&start);
    for (i = 0; i < request->read_count; i++) {
        rpcResult.last_read_size = smb_ReadFile(fid, offsetLow, offsetHigh, readBuffer, request->read_size);
        if (rpcResult.last_read_size != (s32)request->read_size) {
            rpcResult.result = rpcResult.last_read_size;
            if (smb2Diag.error[0]) {
                strncpy(rpcResult.error, smb2Diag.error, sizeof(rpcResult.error));
                rpcResult.error[sizeof(rpcResult.error) - 1] = '\0';
            } else
                strcpy(rpcResult.error, "SMB2 sequential read failed");
            goto read_finished;
        }

        for (j = 0; j < request->read_size; j++) {
            rpcResult.checksum ^= readBuffer[j];
            rpcResult.checksum *= 16777619u;
        }

        rpcResult.bytes_read += request->read_size;
        if (offsetLow + request->read_size < offsetLow)
            offsetHigh++;
        offsetLow += request->read_size;
    }
    rpcResult.result = 0;

read_finished:
    GetSystemTime(&end);
    elapsed.lo = end.lo - start.lo;
    elapsed.hi = end.hi - start.hi - (start.lo > end.lo);
    SysClock2USec(&elapsed, &seconds, &microseconds);
    rpcResult.elapsed_ms = seconds * 1000 + microseconds / 1000;
    rpcResult.dialect = smb2Diag.dialect;
    rpcResult.max_read_size = smb2Diag.max_read_size;
    if (rpcResult.result < 0 || rpcResult.last_read_size != (s32)request->read_size)
        goto cleanup;

    rpcResult.stage = SMB2TEST_STAGE_DONE;

cleanup:
    if (fid != 0xFFFF) {
        closeResult = smb_Close(fid);
        if (rpcResult.stage == SMB2TEST_STAGE_DONE && closeResult < 0) {
            rpcResult.stage = SMB2TEST_STAGE_CLOSE;
            rpcResult.result = closeResult;
            strcpy(rpcResult.error, "SMB2 file close failed");
        }
    }
    smb_Disconnect();
    if (readBuffer)
        FreeSysMemory(readBuffer);
}

static void *rpcHandler(int function, void *buffer, int size)
{
    if (function == SMB2TEST_CMD_RUN && (unsigned int)size >= sizeof(struct smb2test_request))
        runTest((const struct smb2test_request *)buffer);
    else {
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
    int threadID;

    (void)argc;
    (void)argv;

    thread.attr = TH_C;
    thread.option = SMB2TEST_RPC_ID;
    thread.thread = rpcThread;
    thread.priority = 0x20;
    thread.stacksize = 0x6000;
    threadID = CreateThread(&thread);
    if (threadID < 0)
        return MODULE_NO_RESIDENT_END;

    StartThread(threadID, NULL);
    return MODULE_RESIDENT_END;
}
