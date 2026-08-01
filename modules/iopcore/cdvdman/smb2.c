#include "smstcpip.h"
#include "internal.h"

#include <stdarg.h>
#include <stdint.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#define SMB2_HANDLE_COUNT (ISO_MAX_PARTS + 2)

#define WAITIOSEMA(x)   WaitSema(x)
#define SIGNALIOSEMA(x) SignalSema(x)

#ifndef F_GETFL
#define F_GETFL 3
#endif
#ifndef F_SETFL
#define F_SETFL 4
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 1
#endif
#ifndef SHUT_RDWR
#define SHUT_RDWR 2
#endif

extern int (*plwip_close)(int s);
extern int (*plwip_connect)(int s, struct sockaddr *name, socklen_t namelen);
extern int (*plwip_recv)(int s, void *mem, int len, unsigned int flags);
extern int (*plwip_send)(int s, void *dataptr, int size, unsigned int flags);
extern int (*plwip_socket)(int domain, int type, int protocol);
extern int (*plwip_select)(int maxfdp1, fd_set *readset, fd_set *writeset, fd_set *exceptset, struct timeval *timeout);
extern int (*plwip_ioctl)(int s, long cmd, void *argp);
extern int (*plwip_getsockopt)(int s, int level, int optname, void *optval, socklen_t *optlen);
extern int (*plwip_setsockopt)(int s, int level, int optname, const void *optval, socklen_t optlen);
extern int (*plwip_shutdown)(int s, int how);
extern int (*plwip_fcntl)(int s, int cmd, int val);
extern u32 (*pinet_addr)(const char *cp);

extern struct cdvdman_settings_smb cdvdman_settings;

int smb_io_sema = -1;

static struct smb2_context *smb2Context;
static struct smb2fh *smb2Handles[SMB2_HANDLE_COUNT];
static int smb2Connected;
static char smb2Server[24];
static char smb2Share[32];
static char smb2User[32];
static char smb2Password[32];
static int smb2DiagConnectResult;
static int smb2DiagSocketError;
static int smb2DiagFcntlResult;
static int smb2DiagSelectResult;
static int smb2DiagSendResult;
static int smb2DiagRecvResult;
static int smb2DiagRecvAvailable;
static u8 smb2DiagSendHeader[8];
/* libsmb2 iop_connect 失败后会再读 SO_ERROR；若仍为 EINPROGRESS 会覆盖 errno。记住失败码供 getsockopt 改写。 */
static int smb2ConnectFailedFd = -1;
static int smb2ConnectFailedErrno;
static u32 smb2DiagConnectIP;
static u16 smb2DiagConnectPort;
static u16 smb2DefaultPort = 445;
smb2_diag_t smb2Diag;

struct addrinfo
{
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;
    char *ai_canonname;
    struct addrinfo *ai_next;
};

void *malloc(size_t size)
{
    return AllocSysMemory(ALLOC_FIRST, size, NULL);
}

void free(void *ptr)
{
    if (ptr)
        FreeSysMemory(ptr);
}

void *calloc(size_t nmemb, size_t size)
{
    void *ptr = malloc(nmemb * size);

    if (ptr)
        memset(ptr, 0, nmemb * size);

    return ptr;
}

/*
 * 覆盖 libsmb2/compat.c 的损坏实现：原代码用 sprintf(str, fmt, va_list) 而非 vsprintf，
 * UNC 会变成垃圾，TCP 通了也会在 share 连接阶段瞬间失败。
 * 与 libsmb2.a 可能多重定义，SMB 链接需 -Wl,--allow-multiple-definition。
 */
int asprintf(char **strp, const char *fmt, ...)
{
    va_list args;
    char *str;
    int len;

    if (!strp)
        return -1;

    str = malloc(256);
    if (!str)
        return -1;

    va_start(args, fmt);
    len = vsprintf(str, fmt, args);
    va_end(args);
    *strp = str;
    return len;
}

int lwip_close(int s)
{
    return plwip_close(s);
}

int lwip_socket(int domain, int type, int protocol)
{
    unsigned int nonblocking = 0;
    int s;

    /* 与 TCP probe 一致：SOCK_STREAM 显式用 IPPROTO_TCP（libsmb2 常传 protocol=0） */
    if (type == SOCK_STREAM && protocol == 0)
        protocol = IPPROTO_TCP;

    s = plwip_socket(domain, type, protocol);

    /* 新建套接字立即清非阻塞，避免随后 fcntl/ioctl 竞态 */
    if (s >= 0 && plwip_ioctl)
        plwip_ioctl(s, FIONBIO, &nonblocking);

    return s;
}

int lwip_ioctl(int s, long cmd, void *argp)
{
    /* 忽略开启非阻塞；FIONREAD 等其它命令照常 */
    if (cmd == FIONBIO) {
        unsigned int off = 0;

        if (argp && *(unsigned int *)argp)
            return 0;
        if (plwip_ioctl)
            return plwip_ioctl(s, FIONBIO, &off);
        return 0;
    }

    if (!plwip_ioctl)
        return -1;
    return plwip_ioctl(s, cmd, argp);
}

int lwip_connect(int s, struct sockaddr *name, socklen_t namelen)
{
    int result;
    int so_error = 0;
    socklen_t so_error_len = sizeof(so_error);
    unsigned int nonblocking = 0;
    struct sockaddr_in clean;
    struct sockaddr *connectName = name;
    socklen_t connectLen = namelen;

    smb2DiagConnectIP = 0;
    smb2DiagConnectPort = 0;

    /*
     * 始终重建干净的 sockaddr_in（含 sin_len），与已成功的 TCP probe 完全一致。
     * libsmb2 经 sockaddr_storage 拷贝后偶发缺 sin_len / 脏填充，ps2ip 会报 EHOSTUNREACH(113)。
     */
    if (name && name->sa_family == AF_INET && namelen >= (socklen_t)sizeof(struct sockaddr_in)) {
        struct sockaddr_in *in = (struct sockaddr_in *)name;

        memset(&clean, 0, sizeof(clean));
        clean.sin_len = sizeof(clean);
        clean.sin_family = AF_INET;
        clean.sin_port = in->sin_port;
        clean.sin_addr = in->sin_addr;
        /* libsmb2/getaddrinfo 在 IOP 上常留下 sin_port=0（如 atoi 无效），会变成 D…:0 / E113 */
        if (!clean.sin_port)
            clean.sin_port = htons(smb2DefaultPort ? smb2DefaultPort : 445);
        connectName = (struct sockaddr *)&clean;
        connectLen = sizeof(clean);
        smb2DiagConnectIP = clean.sin_addr.s_addr;
        smb2DiagConnectPort = ntohs(clean.sin_port);
    }

    /* 连接前强制阻塞：与已成功的 TCP probe / smbman 一致 */
    if (plwip_ioctl)
        plwip_ioctl(s, FIONBIO, &nonblocking);

    result = plwip_connect(s, connectName, connectLen);
    smb2DiagConnectResult = result;

    if (result == 0) {
        if (smb2ConnectFailedFd == s)
            smb2ConnectFailedFd = -1;
        return 0;
    }

    if (result == -EINPROGRESS || result == -EALREADY)
        so_error = EINPROGRESS;
    else if (result < 0 && result != -1)
        so_error = -result;
    else if (result == -1 && plwip_getsockopt) {
        plwip_getsockopt(s, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len);
        smb2DiagSocketError = so_error;
    }

    /* 若仍落入“连接中”，轮询 SO_ERROR（不用 select 写就绪——在 ps2ip-nm 上不可靠） */
    if (so_error == EINPROGRESS || so_error == EALREADY || so_error == 0) {
        int waited;

        for (waited = 0; waited < 50; waited++) {
            DelayThread(100000); /* 100ms，最多约 5s */
            so_error = 0;
            so_error_len = sizeof(so_error);
            if (plwip_getsockopt)
                plwip_getsockopt(s, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len);
            smb2DiagSocketError = so_error;
            smb2DiagSelectResult = waited + 1;
            if (so_error == 0 || so_error == EISCONN) {
                smb2DiagConnectResult = 0;
                if (smb2ConnectFailedFd == s)
                    smb2ConnectFailedFd = -1;
                errno = 0;
                return 0;
            }
            if (so_error != EINPROGRESS && so_error != EALREADY)
                break;
        }
        /* 绝不能把 EINPROGRESS 交回 libsmb2，否则会当成连接中继续空等 */
        if (so_error == EINPROGRESS || so_error == EALREADY || !so_error)
            so_error = ETIMEDOUT;
    }

    errno = so_error ? so_error : ECONNREFUSED;
    smb2ConnectFailedFd = s;
    smb2ConnectFailedErrno = errno;
    smb2DiagConnectResult = -1;
    return -1;
}

int lwip_recv(int s, void *mem, int len, unsigned int flags)
{
    smb2DiagRecvResult = plwip_recv(s, mem, len, flags);
    return smb2DiagRecvResult;
}

int lwip_send(int s, void *dataptr, int size, unsigned int flags)
{
    if (size >= (int)sizeof(smb2DiagSendHeader))
        memcpy(smb2DiagSendHeader, dataptr, sizeof(smb2DiagSendHeader));

    smb2DiagSendResult = plwip_send(s, dataptr, size, flags);
    return smb2DiagSendResult;
}

int lwip_bind(int s, struct sockaddr *name, socklen_t namelen)
{
    (void)s;
    (void)name;
    (void)namelen;
    return -1;
}

int lwip_listen(int s, int backlog)
{
    (void)s;
    (void)backlog;
    return -1;
}

int lwip_accept(int s, struct sockaddr *addr, socklen_t *addrlen)
{
    (void)s;
    (void)addr;
    (void)addrlen;
    return -1;
}

int lwip_select(int maxfdp1, fd_set *readset, fd_set *writeset, fd_set *exceptset, struct timeval *timeout)
{
    fd_set pendingReadSet;
    fd_set pendingWriteSet;
    int i;
    int so_error;
    socklen_t so_error_len;
    int writeReady;

    if (readset)
        pendingReadSet = *readset;
    if (writeset)
        pendingWriteSet = *writeset;

    /* 连接已完成时立刻报写就绪，避免再空等完整 timeout */
    if (writeset) {
        writeReady = 0;
        for (i = 0; i < maxfdp1; i++) {
            if (!FD_ISSET(i, &pendingWriteSet))
                continue;
            so_error = 0;
            so_error_len = sizeof(so_error);
            if (plwip_getsockopt)
                plwip_getsockopt(i, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len);
            if (so_error == EINPROGRESS || so_error == EALREADY)
                continue;
            writeReady++;
        }
        if (writeReady && !readset) {
            FD_ZERO(writeset);
            for (i = 0; i < maxfdp1; i++) {
                if (!FD_ISSET(i, &pendingWriteSet))
                    continue;
                so_error = 0;
                so_error_len = sizeof(so_error);
                if (plwip_getsockopt)
                    plwip_getsockopt(i, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len);
                smb2DiagSocketError = so_error;
                if (so_error == EINPROGRESS || so_error == EALREADY)
                    continue;
                FD_SET(i, writeset);
            }
            smb2DiagSelectResult = writeReady;
            return writeReady;
        }
    }

    smb2DiagSelectResult = plwip_select(maxfdp1, readset, writeset, exceptset, timeout);

    /*
     * ps2ip-nm 上已完成的 TCP 连接常不触发写就绪，libsmb2 会一直卡在 connecting。
     * 超时后用 SO_ERROR 补判：已连接或已失败都唤醒，仍 EINPROGRESS 则保持超时。
     */
    if (smb2DiagSelectResult == 0 && writeset) {
        writeReady = 0;
        FD_ZERO(writeset);
        for (i = 0; i < maxfdp1; i++) {
            if (!FD_ISSET(i, &pendingWriteSet))
                continue;
            so_error = 0;
            so_error_len = sizeof(so_error);
            if (plwip_getsockopt)
                plwip_getsockopt(i, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len);
            smb2DiagSocketError = so_error;
            if (so_error == EINPROGRESS || so_error == EALREADY)
                continue;
            FD_SET(i, writeset);
            writeReady++;
        }
        if (writeReady) {
            smb2DiagSelectResult = writeReady;
            return writeReady;
        }
    }

    if (smb2DiagSelectResult == 0 && readset) {
        for (i = 0; i < maxfdp1; i++) {
            if (FD_ISSET(i, &pendingReadSet)) {
                u16 available = 0;

                if (plwip_ioctl && plwip_ioctl(i, FIONREAD, &available) == 0)
                    smb2DiagRecvAvailable = available;
                break;
            }
        }
    }

    return smb2DiagSelectResult;
}

int lwip_getsockopt(int s, int level, int optname, void *optval, socklen_t *optlen)
{
    int result = plwip_getsockopt(s, level, optname, optval, optlen);

    /*
     * 覆盖 libsmb2 iop_connect 的二次 SO_ERROR 读取：若仍为 EINPROGRESS，
     * 会把我们已设好的 ETIMEDOUT 盖掉。勿再定义 iop_connect（与 libsmb2.a 多重定义）。
     */
    if (result == 0 && level == SOL_SOCKET && optname == SO_ERROR && optval && s == smb2ConnectFailedFd) {
        *(int *)optval = smb2ConnectFailedErrno;
        smb2DiagSocketError = smb2ConnectFailedErrno;
        smb2ConnectFailedFd = -1;
        return 0;
    }

    if (result == 0 && level == SOL_SOCKET && optname == SO_ERROR)
        smb2DiagSocketError = *(int *)optval;

    return result;
}

int lwip_setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen)
{
    return plwip_setsockopt(s, level, optname, optval, optlen);
}

int lwip_shutdown(int s, int how)
{
    return plwip_shutdown(s, how);
}

int lwip_fcntl(int s, int cmd, int val)
{
    (void)s;
    (void)val;

    /*
     * 阻塞 TCP probe 已能连上 :445；libsmb2 默认会 F_SETFL|O_NONBLOCK，
     * 非阻塞 connect 会得到 CFFFFFFF/S0。强制保持阻塞。
     */
    if (cmd == F_GETFL)
        return 0;

    if (cmd == F_SETFL) {
        smb2DiagFcntlResult = 0;
        return 0;
    }

    return -1;
}

/* libsmb2 set_nonblocking() 通过 fcntl 调进来；勿再导出 ioctl（与 ioman.h 冲突） */
int fcntl(int s, int cmd, int val)
{
    return lwip_fcntl(s, cmd, val);
}

u32 inet_addr(const char *cp)
{
    return pinet_addr(cp);
}

int lwip_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
    struct sockaddr_in *address;

    (void)hints;

    *res = calloc(1, sizeof(struct addrinfo));
    if (!*res)
        return -1;

    address = calloc(1, sizeof(struct sockaddr_in));
    if (!address) {
        free(*res);
        *res = NULL;
        return -1;
    }

    address->sin_len = sizeof(struct sockaddr_in);
    address->sin_family = AF_INET;
    address->sin_addr.s_addr = pinet_addr(node);
    {
        u16 port = 0;

        if (service && service[0])
            port = (u16)strtol(service, NULL, 10);
        if (!port)
            port = smb2DefaultPort ? smb2DefaultPort : 445;
        address->sin_port = htons(port);
    }
    (*res)->ai_family = AF_INET;
    (*res)->ai_socktype = SOCK_STREAM;
    (*res)->ai_protocol = IPPROTO_TCP;
    (*res)->ai_addrlen = sizeof(struct sockaddr_in);
    (*res)->ai_addr = (struct sockaddr *)address;

    return 0;
}

void lwip_freeaddrinfo(struct addrinfo *res)
{
    if (res) {
        free(res->ai_addr);
        free(res);
    }
}

int smb_NegotiateProtocol(char *SMBServerIP, int SMBServerPort, char *Username, char *Password, u32 *capabilities, OplSmbPwHashFunc_t hash_callback)
{
    iop_sema_t smp;

    (void)hash_callback;

    if (smb_io_sema < 0) {
        smp.initial = 1;
        smp.max = 1;
        smp.option = 0;
        smp.attr = 1;
        smb_io_sema = CreateSema(&smp);
        if (smb_io_sema < 0)
            return smb_io_sema;
    }

    if (!smb2Server[0]) {
        /* server 只能是 IP/主机名：带 :port 会把 UNC 弄成 \\ip:port\share */
        if (SMBServerPort == 445)
            strcpy(smb2Server, SMBServerIP);
        else
            sprintf(smb2Server, "%s:%d", SMBServerIP, SMBServerPort);
        smb2DefaultPort = (u16)(SMBServerPort ? SMBServerPort : 445);

        strncpy(smb2User, Username, sizeof(smb2User));
        smb2User[sizeof(smb2User) - 1] = '\0';
        strncpy(smb2Password, Password, sizeof(smb2Password));
        smb2Password[sizeof(smb2Password) - 1] = '\0';
        strncpy(smb2Share, cdvdman_settings.smb_share, sizeof(smb2Share));
        smb2Share[sizeof(smb2Share) - 1] = '\0';
    }

    if (smb2Context) {
        smb2_destroy_context(smb2Context);
        smb2Connected = 0;
    }

    memset(smb2Handles, 0, sizeof(smb2Handles));
    smb2Context = smb2_init_context();
    if (!smb2Context)
        return -ENOMEM;

#ifdef SMB2TEST_BUILD
    smb2_set_timeout(smb2Context, 5);
#else
    smb2_set_timeout(smb2Context, 30);
#endif
    smb2_set_user(smb2Context, smb2User);
    smb2_set_password(smb2Context, smb2Password);
    *capabilities = 0;
    smb2DiagConnectResult = 0;
    smb2DiagSocketError = 0;
    smb2DiagFcntlResult = 0;
    smb2DiagSelectResult = 0;
    smb2DiagSendResult = 0;
    smb2DiagRecvResult = 0;
    smb2DiagRecvAvailable = 0;
    memset(smb2DiagSendHeader, 0, sizeof(smb2DiagSendHeader));
    smb2Diag.error[0] = '\0';

    if (smb2_connect_share(smb2Context, smb2Server, smb2Share, smb2User) != 0) {
        const char *error = smb2_get_error(smb2Context);
        if (error && error[0]) {
            strncpy(smb2Diag.error, error, sizeof(smb2Diag.error));
            smb2Diag.error[sizeof(smb2Diag.error) - 1] = '\0';
        } else
            sprintf(smb2Diag.error, "C%d E%d S%d T%d R%d D%08lX:%u", smb2DiagConnectResult, smb2DiagSocketError, smb2DiagSelectResult, smb2DiagSendResult, smb2DiagRecvResult, (unsigned long)smb2DiagConnectIP, smb2DiagConnectPort);
        smb2_destroy_context(smb2Context);
        smb2Context = NULL;
        return -1;
    }

    smb2Connected = 1;

    return 1;
}

int smb_SessionSetupAndX(u32 capabilities)
{
    (void)capabilities;
    return smb2Context ? 1 : -1;
}

int smb_TreeConnectAndX(char *ShareName)
{
    char *share;

    if (!smb2Context)
        return -1;

    if (smb2Connected)
        return 1;

    if (!smb2Share[0]) {
        share = strrchr(ShareName, '\\');
        if (!share || !share[1])
            return -1;

        strncpy(smb2Share, share + 1, sizeof(smb2Share));
        smb2Share[sizeof(smb2Share) - 1] = '\0';
    }

    if (smb2_connect_share(smb2Context, smb2Server, smb2Share, smb2User) != 0)
        return -1;

    smb2Connected = 1;
    return 1;
}

int smb_OpenAndX(char *filename, u8 *FID, int Write)
{
    char path[256];
    char *separator;
    struct smb2fh *fh;
    int i, result = 0;

    WAITIOSEMA(smb_io_sema);

    for (i = 0; i < SMB2_HANDLE_COUNT; i++) {
        if (!smb2Handles[i])
            break;
    }

    if (i == SMB2_HANDLE_COUNT)
        goto cleanup;

    strncpy(path, filename, sizeof(path));
    path[sizeof(path) - 1] = '\0';
    for (separator = path; *separator; separator++) {
        if (*separator == '\\')
            *separator = '/';
    }

    fh = smb2_open(smb2Context, path[0] == '/' ? path + 1 : path, Write ? O_RDWR : O_RDONLY);
    if (!fh)
        goto cleanup;

    smb2Handles[i] = fh;
    memcpy(FID, &i, sizeof(u16));
    result = 1;

cleanup:
    SIGNALIOSEMA(smb_io_sema);
    return result;
}

int smb_Close(int FID)
{
    int result;

    if (!smb2Context || FID < 0 || FID >= SMB2_HANDLE_COUNT || !smb2Handles[FID])
        return -EIO;

    result = smb2_close(smb2Context, smb2Handles[FID]);
    if (result == 0)
        smb2Handles[FID] = NULL;

    return result;
}

int smb_ReadFile(u16 FID, u32 offsetlow, u32 offsethigh, void *readbuf, int nbytes)
{
    int remaining = nbytes;
    u8 *ptr = readbuf;
    int diagPending = 0;
    int result;

    if (!smb2Context || FID >= SMB2_HANDLE_COUNT || !smb2Handles[FID]) {
        if (!smb2Diag.valid) {
            smb2Diag.valid = 1;
            smb2Diag.result = -EIO;
            smb2Diag.fid = FID;
            smb2Diag.offset_low = offsetlow;
            smb2Diag.offset_high = offsethigh;
            smb2Diag.request_size = nbytes;
            smb2Diag.dialect = smb2Context ? smb2_get_dialect(smb2Context) : 0;
            smb2Diag.max_read_size = smb2Context ? smb2_get_max_read_size(smb2Context) : 0;
            strcpy(smb2Diag.error, "Invalid SMB2 context or handle");
        }
        return -EIO;
    }

    WAITIOSEMA(smb_io_sema);
    if (!smb2Diag.valid) {
        smb2Diag.valid = 1;
        smb2Diag.result = SMB2_DIAG_RESULT_PENDING;
        smb2Diag.fid = FID;
        smb2Diag.offset_low = offsetlow;
        smb2Diag.offset_high = offsethigh;
        smb2Diag.request_size = nbytes;
        smb2Diag.dialect = smb2_get_dialect(smb2Context);
        smb2Diag.max_read_size = smb2_get_max_read_size(smb2Context);
        smb2Diag.error[0] = '\0';
        diagPending = 1;
    }

    while (remaining > 0) {
        if (diagPending) {
            smb2Diag.result = SMB2_DIAG_RESULT_PENDING;
            smb2Diag.offset_low = offsetlow;
            smb2Diag.offset_high = offsethigh;
            smb2Diag.request_size = remaining;
        }

        result = smb2_pread(smb2Context, smb2Handles[FID], ptr, remaining, ((u64)offsethigh << 32) | offsetlow);
        if (result <= 0)
            break;

        if (offsetlow + result < offsetlow)
            offsethigh++;
        offsetlow += result;
        ptr += result;
        remaining -= result;
    }

    if (diagPending && remaining == 0) {
        smb2Diag.valid = 0;
    } else if (diagPending) {
        const char *error = smb2_get_error(smb2Context);

        smb2Diag.result = result;
        if (error) {
            strncpy(smb2Diag.error, error, sizeof(smb2Diag.error));
            smb2Diag.error[sizeof(smb2Diag.error) - 1] = '\0';
        }
    }
    SIGNALIOSEMA(smb_io_sema);

    return remaining > 0 ? result : nbytes;
}

int smb_WriteFile(u16 FID, u32 offsetlow, u32 offsethigh, void *writebuf, int nbytes)
{
    int result;

    if (!smb2Context || FID >= SMB2_HANDLE_COUNT || !smb2Handles[FID])
        return -EIO;

    WAITIOSEMA(smb_io_sema);
    result = smb2_pwrite(smb2Context, smb2Handles[FID], writebuf, nbytes, ((u64)offsethigh << 32) | offsetlow);
    SIGNALIOSEMA(smb_io_sema);

    return result;
}

int smb_ReadCD(unsigned int lsn, unsigned int nsectors, void *buf, int part_num)
{
    return smb_ReadFile(cdvdman_settings.FIDs[part_num], lsn * 2048, lsn >> 21, buf, nsectors * 2048);
}

int smb_Echo(void)
{
    int result;

    if (!smb2Context || smb_io_sema < 0 || PollSema(smb_io_sema) != 0)
        return 0;

    result = smb2_echo(smb2Context) == 0 ? 1 : -1;
    SIGNALIOSEMA(smb_io_sema);

    return result;
}

void smb_CloseAll(void)
{
    int i, fd;

    for (i = 0; i < cdvdman_settings.common.NumParts; i++) {
        fd = cdvdman_settings.FIDs[i];
        if (fd >= 0) {
            smb_Close(fd);
            cdvdman_settings.FIDs[i] = -1;
        }
    }
}

int smb_Disconnect(void)
{
    if (smb2Context) {
        smb2_destroy_context(smb2Context);
        smb2Context = NULL;
        smb2Connected = 0;
        memset(smb2Handles, 0, sizeof(smb2Handles));
    }

    return 1;
}

int smb_AbortConnection(void)
{
    int fd;

    if (!smb2Context || !plwip_shutdown)
        return -1;

    fd = smb2_get_fd(smb2Context);
    return fd >= 0 ? plwip_shutdown(fd, SHUT_RDWR) : -1;
}
