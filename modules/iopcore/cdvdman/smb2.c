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

#ifdef SMB2TEST_BUILD
/* select 返回前先把可读数据收到 bounce，避免随后在 libsmb2 收包路径上踩 IOP 异常 */
static u8 smb2RxBounce[2048];
static int smb2RxBounceLen;
static int smb2RxBouncePos;
static int smb2RxBounceFd = -1;
#endif

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
 * 覆盖 libsmb2 的 SHA512 preauth（USHA*）。
 * negotiate_cb 在收到 NEGOTIATE 响应后总会算 preauthhash；抓包显示此前在
 * 3.1.1 响应后、SESSION_SETUP 前崩溃。SMB 2.0.2 不依赖该哈希即可建会话。
 * 需与 -Wl,--allow-multiple-definition 联用。
 */
int USHAReset(void *ctx, int whichSha)
{
    (void)ctx;
    (void)whichSha;
    return 0;
}

int USHAInput(void *ctx, const unsigned char *bytes, unsigned int bytecount)
{
    (void)ctx;
    (void)bytes;
    (void)bytecount;
    return 0;
}

int USHAResult(void *ctx, unsigned char *digest)
{
    (void)ctx;
    if (digest)
        memset(digest, 0, 64);
    return 0;
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
#ifdef SMB2TEST_BUILD
    printf("SMB2TEST: recv enter want=%d fl=%u\n", len, flags);
    if (s == smb2RxBounceFd && smb2RxBouncePos < smb2RxBounceLen && mem && len > 0) {
        int left = smb2RxBounceLen - smb2RxBouncePos;
        int n = (len < left) ? len : left;

        memcpy(mem, smb2RxBounce + smb2RxBouncePos, n);
        smb2RxBouncePos += n;
        if (smb2RxBouncePos >= smb2RxBounceLen) {
            smb2RxBounceFd = -1;
            smb2RxBounceLen = 0;
            smb2RxBouncePos = 0;
        }
        smb2DiagRecvResult = n;
        printf("SMB2TEST: recv bounce r=%d want=%d\n", n, len);
        return n;
    }
#endif
    /* libsmb2 按非阻塞语义使用 recv；无数据时返回 EAGAIN，避免阻塞死等 */
    if (plwip_ioctl) {
        int available = 0;

        if (plwip_ioctl(s, FIONREAD, &available) == 0 && available <= 0) {
            errno = EAGAIN;
#ifdef SMB2TEST_BUILD
            printf("SMB2TEST: recv EAGAIN\n");
#endif
            smb2DiagRecvResult = -1;
            return -1;
        }
    }
    smb2DiagRecvResult = plwip_recv(s, mem, len, flags);
#ifdef SMB2TEST_BUILD
    printf("SMB2TEST: recv r=%d want=%d\n", smb2DiagRecvResult, len);
    if (smb2DiagRecvResult >= 12 && ((u8 *)mem)[0] == 0xFE && ((u8 *)mem)[1] == 'S' && ((u8 *)mem)[2] == 'M' && ((u8 *)mem)[3] == 'B')
        printf("SMB2TEST: status=%02X%02X%02X%02X\n", ((u8 *)mem)[11], ((u8 *)mem)[10], ((u8 *)mem)[9], ((u8 *)mem)[8]);
#endif
    return smb2DiagRecvResult;
}

int lwip_send(int s, void *dataptr, int size, unsigned int flags)
{
    if (size >= (int)sizeof(smb2DiagSendHeader))
        memcpy(smb2DiagSendHeader, dataptr, sizeof(smb2DiagSendHeader));

    smb2DiagSendResult = plwip_send(s, dataptr, size, flags);
#ifdef SMB2TEST_BUILD
    printf("SMB2TEST: send r=%d size=%d hdr=%02X%02X%02X%02X\n",
           smb2DiagSendResult, size,
           smb2DiagSendHeader[0], smb2DiagSendHeader[1],
           smb2DiagSendHeader[2], smb2DiagSendHeader[3]);
#endif
    return smb2DiagSendResult;
}

/*
 * 覆盖 libsmb2 NEED_READV/WRITEV。
 * 库内实现经 read()/write() 宏变成 lwip_recv/send(..., MSG_DONTWAIT)；
 * 在 ps2ip-nm 上收包刚就绪时 DONTWAIT 易踩 PCSX2 IOP JIT。
 * 这里改为直接、阻塞地走我们已挂钩的 lwip_recv/lwip_send。
 */
struct iovec {
    void *iov_base;
    size_t iov_len;
};

int readv(int fd, const struct iovec *vector, int count)
{
    int i;
    int total = 0;

#ifdef SMB2TEST_BUILD
    printf("SMB2TEST: readv enter count=%d\n", count);
#endif
    if (!vector || count <= 0)
        return -1;

    for (i = 0; i < count; i++) {
        int n;

        if (!vector[i].iov_base || !vector[i].iov_len)
            continue;
        n = lwip_recv(fd, vector[i].iov_base, (int)vector[i].iov_len, 0);
        if (n < 0) {
            if (total > 0)
                return total;
            return n;
        }
        if (n == 0)
            break;
        total += n;
        if ((size_t)n < vector[i].iov_len)
            break;
    }
#ifdef SMB2TEST_BUILD
    printf("SMB2TEST: readv r=%d\n", total);
#endif
    return total;
}

int writev(int fd, const struct iovec *vector, int count)
{
    static u8 combined[512];
    int i;
    int total = 0;
    int offset = 0;

    if (!vector || count <= 0)
        return -1;

    /* 小包合并成一次 send，避免把 NEGOTIATE 拆成多段 TCP */
    for (i = 0; i < count; i++) {
        if (!vector[i].iov_base || !vector[i].iov_len)
            continue;
        if (offset + (int)vector[i].iov_len > (int)sizeof(combined)) {
            offset = -1;
            break;
        }
        memcpy(combined + offset, vector[i].iov_base, vector[i].iov_len);
        offset += (int)vector[i].iov_len;
    }
    if (offset > 0)
        return lwip_send(fd, combined, offset, 0);

    for (i = 0; i < count; i++) {
        int n;

        if (!vector[i].iov_base || !vector[i].iov_len)
            continue;
        n = lwip_send(fd, vector[i].iov_base, (int)vector[i].iov_len, 0);
        if (n < 0) {
            if (total > 0)
                return total;
            return n;
        }
        if (n == 0)
            break;
        total += n;
        if ((size_t)n < vector[i].iov_len)
            break;
    }
    return total;
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

/*
 * 切勿对调用方的 fd_set 做结构体赋值 / FD_ZERO：
 * 我们编进的 sizeof(fd_set)（smstcpip FD_SETSIZE=16）可能与 libsmb2/ps2ip
 * 不一致；整结构写回会砸坏 poll() 栈 → IOP PC 跑飞（ffffffff / f0000102）。
 * 只按 bit 操作 0 .. maxfdp1-1。
 */
static void smb2FdClearRange(fd_set *set, int maxfdp1)
{
    int i;

    if (!set)
        return;
    for (i = 0; i < maxfdp1; i++)
        FD_CLR(i, set);
}

int lwip_select(int maxfdp1, fd_set *readset, fd_set *writeset, fd_set *exceptset, struct timeval *timeout)
{
    u32 readInterest = 0;
    u32 writeInterest = 0;
    int i;
    int so_error;
    socklen_t so_error_len;
    int writeReady;
    int wait_ms;
    int waited;

#ifdef SMB2TEST_BUILD
    printf("SMB2TEST: select enter max=%d r=%d w=%d e=%d\n",
           maxfdp1, readset ? 1 : 0, writeset ? 1 : 0, exceptset ? 1 : 0);
#endif

    if (maxfdp1 > 32)
        maxfdp1 = 32;

    if (readset) {
        for (i = 0; i < maxfdp1; i++) {
            if (FD_ISSET(i, readset))
                readInterest |= (1u << i);
        }
    }
    if (writeset) {
        for (i = 0; i < maxfdp1; i++) {
            if (FD_ISSET(i, writeset))
                writeInterest |= (1u << i);
        }
    }

    /* libsmb2 NEED_POLL 会预置 exceptset；ps2ip 上易误报，一律忽略 */
    smb2FdClearRange(exceptset, maxfdp1);

    /* 连接已完成时立刻报写就绪，避免再空等完整 timeout */
    if (writeset) {
        writeReady = 0;
        for (i = 0; i < maxfdp1; i++) {
            if (!(writeInterest & (1u << i)))
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
            smb2FdClearRange(writeset, maxfdp1);
            writeReady = 0;
            for (i = 0; i < maxfdp1; i++) {
                if (!(writeInterest & (1u << i)))
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
            smb2DiagSelectResult = writeReady;
#ifdef SMB2TEST_BUILD
            printf("SMB2TEST: select early-write %d\n", writeReady);
#endif
            return writeReady;
        }
    }

    /*
     * 纯读等待：避免长时间阻塞在 select 内（PCSX2 IOP JIT 易崩）。
     * 但必须调用 plwip_select 才能让 lwIP/SMAP 把收包泵进套接字；
     * 仅 FIONREAD+DelayThread 不会处理入站报文，会一直 R0 超时。
     */
    if (readset && !writeset) {
        struct timeval slice;

        wait_ms = 5000;
        if (timeout)
            wait_ms = (int)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000);
        if (wait_ms < 0)
            wait_ms = 5000;
        if (wait_ms > 30000)
            wait_ms = 30000;

        for (waited = 0; waited <= wait_ms; waited += 100) {
            smb2FdClearRange(readset, maxfdp1);
            for (i = 0; i < maxfdp1; i++) {
                if (readInterest & (1u << i))
                    FD_SET(i, readset);
            }

            slice.tv_sec = 0;
            slice.tv_usec = 100000; /* 100ms，泵送协议栈并等待可读 */
            writeReady = plwip_select(maxfdp1, readset, NULL, NULL, &slice);
            if (writeReady > 0) {
                smb2DiagSelectResult = writeReady;
#ifdef SMB2TEST_BUILD
#if 0 // 禁用提前收包，直接让libsmb2读取socket。
                {
                    int fd;

                    smb2RxBounceFd = -1;
                    smb2RxBounceLen = 0;
                    smb2RxBouncePos = 0;
                    for (fd = 0; fd < maxfdp1; fd++) {
                        int available = 0;
                        int n;

                        if (!FD_ISSET(fd, readset))
                            continue;
                        if (plwip_ioctl)
                            plwip_ioctl(fd, FIONREAD, &available);
                        if (available <= 0)
                            available = (int)sizeof(smb2RxBounce);
                        if (available > (int)sizeof(smb2RxBounce))
                            available = (int)sizeof(smb2RxBounce);
                        printf("SMB2TEST: pre-recv fd=%d avail=%d stack=%d\n",
                               fd, available, CheckThreadStack());
                        n = plwip_recv(fd, smb2RxBounce, available, 0);
                        printf("SMB2TEST: pre-recv r=%d hdr=%02X%02X%02X%02X\n",
                               n,
                               n > 0 ? smb2RxBounce[0] : 0,
                               n > 1 ? smb2RxBounce[1] : 0,
                               n > 2 ? smb2RxBounce[2] : 0,
                               n > 3 ? smb2RxBounce[3] : 0);
                        if (n > 0) {
                            smb2RxBounceFd = fd;
                            smb2RxBounceLen = n;
                            smb2RxBouncePos = 0;
                        }
                        break;
                    }
                }
#endif
                printf("SMB2TEST: select slice-read ready=%d waited=%d stack=%d\n",
                       writeReady, waited, CheckThreadStack());
                printf("SMB2TEST: select return to libsmb2\n");
#endif
                return writeReady;
            }
        }

        smb2FdClearRange(readset, maxfdp1);
        smb2DiagSelectResult = 0;
#ifdef SMB2TEST_BUILD
        printf("SMB2TEST: select slice-read timeout %d ms\n", wait_ms);
#endif
        return 0;
    }

    smb2DiagSelectResult = plwip_select(maxfdp1, readset, writeset, NULL, timeout);
    smb2FdClearRange(exceptset, maxfdp1);

#ifdef SMB2TEST_BUILD
    printf("SMB2TEST: select leave r=%d\n", smb2DiagSelectResult);
#endif

    /*
     * ps2ip-nm 上已完成的 TCP 连接常不触发写就绪，libsmb2 会一直卡在 connecting。
     * 超时后用 SO_ERROR 补判：已连接或已失败都唤醒，仍 EINPROGRESS 则保持超时。
     */
    if (smb2DiagSelectResult == 0 && writeset) {
        writeReady = 0;
        smb2FdClearRange(writeset, maxfdp1);
        for (i = 0; i < maxfdp1; i++) {
            if (!(writeInterest & (1u << i)))
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

    if (smb2DiagSelectResult == 0 && readInterest) {
        for (i = 0; i < maxfdp1; i++) {
            if (readInterest & (1u << i)) {
                int available = 0;

                if (plwip_ioctl && plwip_ioctl(i, FIONREAD, &available) == 0)
                    smb2DiagRecvAvailable = available;
                break;
            }
        }
    }

    return smb2DiagSelectResult;
}

#ifndef POLLIN
#define POLLIN  0x0001
#define POLLPRI 0x0002
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#endif

#ifndef SMB2_POLLFD_DEFINED
#define SMB2_POLLFD_DEFINED
struct pollfd {
    int fd;
    short events;
    short revents;
};
#endif

/* 覆盖 libsmb2 NEED_POLL：不使用 exceptset（ps2ip 易误报 POLLHUP），并打点定位卡死位置 */
int poll(struct pollfd *fds, unsigned int nfds, int timo)
{
    struct timeval timeout, *toptr;
    fd_set ifds, ofds;
    fd_set *ip, *op;
    unsigned int i;
    int maxfd = 0;
    int rc;

#ifdef SMB2TEST_BUILD
    printf("SMB2TEST: poll enter nfds=%u ev=%x timo=%d\n",
           nfds, (nfds && fds) ? (unsigned)fds[0].events : 0, timo);
#endif

    FD_ZERO(&ifds);
    FD_ZERO(&ofds);
    ip = NULL;
    op = NULL;
    for (i = 0; i < nfds; ++i) {
        fds[i].revents = 0;
        if (fds[i].fd < 0)
            continue;
        if (fds[i].events & (POLLIN | POLLPRI)) {
            ip = &ifds;
            FD_SET(fds[i].fd, ip);
        }
        if (fds[i].events & POLLOUT) {
            op = &ofds;
            FD_SET(fds[i].fd, op);
        }
        if (fds[i].fd > maxfd)
            maxfd = fds[i].fd;
    }

    if (timo < 0) {
        toptr = NULL;
    } else {
        toptr = &timeout;
        timeout.tv_sec = timo / 1000;
        timeout.tv_usec = (timo % 1000) * 1000;
    }

    rc = lwip_select(maxfd + 1, ip, op, NULL, toptr);
#ifdef SMB2TEST_BUILD
    printf("SMB2TEST: poll after-select rc=%d\n", rc);
#endif
    if (rc <= 0)
        return rc;

    rc = 0;
    for (i = 0; i < nfds; ++i) {
        int fd = fds[i].fd;
        short revents = 0;

        if (fd < 0)
            continue;
        if ((fds[i].events & (POLLIN | POLLPRI)) && ip && FD_ISSET(fd, ip))
            revents |= POLLIN;
        if ((fds[i].events & POLLOUT) && op && FD_ISSET(fd, op))
            revents |= POLLOUT;
        if (revents) {
            fds[i].revents = revents;
            rc++;
        }
    }
#ifdef SMB2TEST_BUILD
    printf("SMB2TEST: poll leave rc=%d rev=%x stack=%d\n",
           rc, (nfds && fds) ? (unsigned)fds[0].revents : 0, CheckThreadStack());
#endif
    return rc;
}

#ifdef SMB2TEST_BUILD
int __real_smb2_service(struct smb2_context *smb2, int revents);
struct smb2_io_vectors {
    size_t num_done;
    size_t total_size;
    int niov;
    struct smb2_iovec iov[256];
};
void __real_smb2_free_iovector(struct smb2_context *smb2, struct smb2_io_vectors *v);
struct smb2_iovec *__real_smb2_add_iovector(struct smb2_context *smb2, struct smb2_io_vectors *v, u8 *buf, size_t len, void (*free_fn)(void *));

void __wrap_smb2_free_iovector(struct smb2_context *smb2, struct smb2_io_vectors *v)
{
    printf("SMB2TEST: free_iov enter n=%d d=%u t=%u f=%u\n", v->niov, (unsigned int)v->num_done, (unsigned int)v->total_size, v->niov > 0 ? (unsigned int)v->iov[0].len : 0);
    __real_smb2_free_iovector(smb2, v);
    printf("SMB2TEST: free_iov leave n=%d d=%u t=%u\n", v->niov, (unsigned int)v->num_done, (unsigned int)v->total_size);
}

struct smb2_iovec *__wrap_smb2_add_iovector(struct smb2_context *smb2, struct smb2_io_vectors *v, u8 *buf, size_t len, void (*free_fn)(void *))
{
    struct smb2_iovec *result;

    printf("SMB2TEST: add_iov enter len=%u n=%d d=%u t=%u\n", (unsigned int)len, v->niov, (unsigned int)v->num_done, (unsigned int)v->total_size);
    result = __real_smb2_add_iovector(smb2, v, buf, len, free_fn);
    printf("SMB2TEST: add_iov leave n=%d d=%u t=%u f=%u\n", v->niov, (unsigned int)v->num_done, (unsigned int)v->total_size, v->niov > 0 ? (unsigned int)v->iov[0].len : 0);
    return result;
}

int __wrap_smb2_service(struct smb2_context *smb2, int revents)
{
    printf("SMB2TEST: smb2_service rev=0x%x stack=%d\n", revents, CheckThreadStack());
    return __real_smb2_service(smb2, revents);
}
#endif

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
    /*
     * 抓包：服务器曾回 SMB 3.1.1，IOP 在 NEGOTIATE 响应后、SESSION_SETUP 前崩溃。
     * 先锁死 2.0.2，关闭 seal，降低密码学/上下文解析复杂度。
     * （IOP 无 KRB5 时 libsmb2 会自然走 NTLMSSP，勿依赖 SMB2_SEC_* 枚举——旧 ports 可能没有）
     */
    smb2_set_version(smb2Context, SMB2_VERSION_0202);
    smb2_set_seal(smb2Context, 0);
    smb2_set_security_mode(smb2Context, 0);
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

#ifdef SMB2TEST_BUILD
    printf("SMB2TEST: dialect lock 0202, connect %s/%s\n", smb2Server, smb2Share);
#endif
    if (smb2_connect_share(smb2Context, smb2Server, smb2Share, smb2User) != 0) {
#ifdef SMB2TEST_BUILD
        printf("SMB2TEST: smb2_connect_share failed\n");
#endif
        const char *error = smb2_get_error(smb2Context);
        if (error && error[0]) {
            strncpy(smb2Diag.error, error, sizeof(smb2Diag.error));
            smb2Diag.error[sizeof(smb2Diag.error) - 1] = '\0';
        } else
            sprintf(smb2Diag.error, "C%d E%d S%d T%d R%d D%08lX:%u", smb2DiagConnectResult, smb2DiagSocketError, smb2DiagSelectResult, smb2DiagSendResult, smb2DiagRecvResult, (unsigned long)smb2DiagConnectIP, smb2DiagConnectPort);
#ifdef SMB2TEST_BUILD
        printf("SMB2TEST: error=%s\n", smb2Diag.error);
#endif
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
