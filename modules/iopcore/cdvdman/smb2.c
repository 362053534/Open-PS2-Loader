#include "smstcpip.h"
#include "internal.h"

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

int lwip_close(int s)
{
    return plwip_close(s);
}

int lwip_connect(int s, struct sockaddr *name, socklen_t namelen)
{
    return plwip_connect(s, name, namelen);
}

int lwip_recv(int s, void *mem, int len, unsigned int flags)
{
    return plwip_recv(s, mem, len, flags);
}

int lwip_send(int s, void *dataptr, int size, unsigned int flags)
{
    return plwip_send(s, dataptr, size, flags);
}

int lwip_socket(int domain, int type, int protocol)
{
    return plwip_socket(domain, type, protocol);
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
    return plwip_select(maxfdp1, readset, writeset, exceptset, timeout);
}

int lwip_getsockopt(int s, int level, int optname, void *optval, socklen_t *optlen)
{
    return plwip_getsockopt(s, level, optname, optval, optlen);
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
    unsigned int nonblocking;

    if (cmd == F_GETFL)
        return 0;

    if (cmd != F_SETFL || !plwip_ioctl)
        return -1;

    nonblocking = val & O_NONBLOCK;
    return plwip_ioctl(s, FIONBIO, &nonblocking);
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

    address->sin_family = AF_INET;
    address->sin_addr.s_addr = pinet_addr(node);
    address->sin_port = service ? htons(strtol(service, NULL, 10)) : 0;
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
        if (SMBServerPort == 445)
            strcpy(smb2Server, SMBServerIP);
        else
            sprintf(smb2Server, "%s:%d", SMBServerIP, SMBServerPort);

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

    smb2_set_timeout(smb2Context, 30);
    smb2_set_user(smb2Context, smb2User);
    smb2_set_password(smb2Context, smb2Password);
    *capabilities = 0;

    if (smb2_connect_share(smb2Context, smb2Server, smb2Share, smb2User) != 0) {
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
    int result;

    if (!smb2Context || FID >= SMB2_HANDLE_COUNT || !smb2Handles[FID])
        return -EIO;

    WAITIOSEMA(smb_io_sema);
    result = smb2_pread(smb2Context, smb2Handles[FID], readbuf, nbytes, ((u64)offsethigh << 32) | offsetlow);
    SIGNALIOSEMA(smb_io_sema);

    return result;
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
