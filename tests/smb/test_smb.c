#include "transport.h"

struct cdvdman_settings_smb cdvdman_settings = {
    .common = {.NumParts = 1, .media = 0x14},
    .filename = "test.iso",
    .smb_ip = "192.0.2.1",
    .smb_port = 445,
    .smb_share = "PS2SMB"
};
struct irx_export_table _exp_oplsmb;

static int sema_value = 1;
static u64 now_us;
static int echo_requests, tcp_probes, phy_checks, tray_changes;
static u64 echo_sent_at[32];
static unsigned int (*alarm_callback)(void *);
static void *alarm_arg;
static u64 alarm_at, alarm_period;
static int read_wakeup;
static u8 response[16384];
static int response_size, response_pos;
static int rx_prefix = 32768;
static u64 rx_release_us;
static int drop_echo;
static unsigned int poll_overhead_us;
static int require_nonblocking;
static int send_blocked, socket_closed, corrupt_echo, wrong_mid_first, echo_error;
static int link_up = 1;
struct lwip_socket test_socket;

struct lwip_socket *get_socket(int s)
{
    int end = now_us >= rx_release_us ? response_size : rx_prefix;
    (void)s;
    test_socket.rcvevent = response_pos < end;
    if (socket_closed)
        test_socket.rcvevent = 1;
    return &test_socket;
}
void sock_set_errno(struct lwip_socket *s, int error) { s->err = error; }
struct netbuf *netconn_recv(struct netconn *conn)
{
    int end = now_us >= rx_release_us ? response_size : rx_prefix;
    int size = end - response_pos;
    struct netbuf *buf;
    (void)conn;
    if (socket_closed)
        return NULL;
    assert(size > 0);
    buf = calloc(1, sizeof(*buf));
    buf->p = calloc(1, sizeof(*buf->p));
    memcpy(buf->p->data, response + response_pos, size);
    buf->p->size = size;
    response_pos += size;
    get_socket(1);
    return buf;
}
int netconn_type(struct netconn *conn) { (void)conn; return NETCONN_TCP; }
void netbuf_delete(struct netbuf *buf) { free(buf->p); free(buf); }
u16 netbuf_len(struct netbuf *buf) { return buf->p->size; }
void pbuf_cat(struct pbuf *dst, struct pbuf *src)
{
    assert(dst->size + src->size <= sizeof(dst->data));
    memcpy(dst->data + dst->size, src->data, src->size);
    dst->size += src->size;
    free(src);
}
void netbuf_copy_partial(struct netbuf *buf, void *dst, u16 size, u16 offset)
{
    assert(offset + size <= buf->p->size);
    memcpy(dst, buf->p->data + offset, size);
}
struct ip_addr *netbuf_fromaddr(struct netbuf *buf) { static struct ip_addr addr; (void)buf; return &addr; }
u16 netbuf_fromport(struct netbuf *buf) { (void)buf; return 445; }

static void respond(const void *body, int size)
{
    int offset = response_size;
    assert(offset + size + 4 <= sizeof(response));
    response_size += size + 4;
    response[offset] = 0;
    response[offset + 1] = size >> 16;
    response[offset + 2] = size >> 8;
    response[offset + 3] = size;
    memcpy(response + offset + 4, body, size);
}

static int fake_send(int s, void *data, int size, unsigned int flags)
{
    SMBHeader_t *h = (SMBHeader_t *)((u8 *)data + 4);
    u8 body[8448] = {0};
    SMBHeader_t *reply = (SMBHeader_t *)body;
    (void)s;
    (void)flags;
    assert(size >= 36);
    *reply = *h;
    reply->Flags = SMB_FLAGS_SERVER_TO_REDIR;
    switch (h->Cmd) {
        case SMB_COM_NEGOTIATE: {
            NegotiateProtocolResponse_t *n = (void *)body;
            n->smbWordcount = 17;
            n->MaxMpxCount = 2;
            respond(body, sizeof(*n) + 32);
            break;
        }
        case SMB_COM_SESSION_SETUP_ANDX:
            reply->UID = 12;
            respond(body, sizeof(SessionSetupAndXResponse_t));
            break;
        case SMB_COM_TREE_CONNECT_ANDX:
            reply->TID = 34;
            respond(body, sizeof(TreeConnectAndXResponse_t));
            break;
        case SMB_COM_OPEN_ANDX:
            ((OpenAndXResponse_t *)body)->FID = 56;
            respond(body, sizeof(OpenAndXResponse_t));
            break;
        case SMB_COM_ECHO:
            if (require_nonblocking && !(flags & MSG_DONTWAIT)) {
                fprintf(stderr, "FAIL: Echo send can block behind a full TCP send queue while holding the IGR lock\n");
                exit(1);
            }
            assert(echo_requests < 32);
            if (send_blocked)
                return -1;
            echo_sent_at[echo_requests++] = now_us;
            memcpy(body + sizeof(SMBHeader_t), (u8 *)data + 4 + sizeof(SMBHeader_t), size - 4 - sizeof(SMBHeader_t));
            for (int i = 0; i < wrong_mid_first; i++) {
                if (wrong_mid_first > 1) {
                    respond(body, 0);
                    response[response_size - 4] = 0x85;
                }
                reply->MID--;
                respond(body, size - 4);
                reply->MID++;
            }
            if (drop_echo)
                break;
            if (corrupt_echo)
                ((EchoRequest_t *)body)->EchoCount = 2;
            if (echo_error)
                reply->Ecode = (s16)0xc000;
            respond(body, size - 4);
            break;
        case SMB_COM_READ_ANDX: {
            ReadAndXRequest_t *request = (void *)h;
            ReadAndXResponse_t *read_reply = (void *)body;
            int bytes = request->MaxCountLow;
            read_reply->DataOffset = sizeof(*read_reply);
            read_reply->DataLengthLow = bytes;
            memset(body + sizeof(*read_reply), 0x5a, bytes);
            respond(body, sizeof(*read_reply) + bytes);
            break;
        }
        case SMB_COM_CLOSE:
            respond(body, sizeof(CloseResponse_t));
            break;
        default:
            assert(!"unexpected SMB command");
    }
    return size;
}

static int fake_recv(int s, void *buf, int size, unsigned int flags)
{
    return lwip_recvfrom(s, NULL, 0, buf, size, flags, NULL, NULL);
}

static int fake_close(int s) { (void)s; return 0; }
static int fake_connect(int s, struct sockaddr *a, socklen_t n) { (void)s; (void)a; (void)n; return 0; }
static int fake_socket(int a, int b, int c) { (void)a; (void)b; (void)c; return 1; }
static int fake_setsockopt(int s, int l, int o, const void *v, socklen_t n) { (void)s; (void)l; (void)o; (void)v; (void)n; return 0; }
static int fake_probe(int s) { (void)s; tcp_probes++; return 0; }
static int fake_link(void) { phy_checks++; return link_up; }
static void hash_password(server_specs_t *specs) { (void)specs; }

static void advance_idle(u64 until)
{
    while (alarm_callback && alarm_at <= until) {
        unsigned int next;
        now_us = alarm_at;
        next = alarm_callback(alarm_arg);
        if (next)
            alarm_at = now_us + (u64)next * 1000 / 36864;
        else
            alarm_callback = NULL;
        if (read_wakeup) {
            read_wakeup = 0;
            assert(smb_read_thread_on_wake(0) == 1);
        }
    }
    if (now_us < until)
        now_us = until;
}

int getModInfo(const char *name, modinfo_t *info)
{
    memset(info, 0, sizeof(*info));
    if (!strcmp(name, "ps2ip")) {
        info->exports[6] = fake_close;
        info->exports[7] = fake_connect;
        info->exports[9] = fake_recv;
        info->exports[10] = lwip_recvfrom;
        info->exports[11] = fake_send;
        info->exports[13] = fake_socket;
        info->exports[19] = fake_setsockopt;
        info->exports[24] = inet_addr;
        info->exports[42] = fake_probe;
    } else if (!strcmp(name, "netman"))
        info->exports[14] = fake_link;
    else if (!strcmp(name, "smaplink"))
        info->exports[4] = fake_link;
    return 1;
}

int CreateSema(iop_sema_t *s) { sema_value = s->initial; return 1; }
int WaitSema(int s) { (void)s; assert(sema_value == 1); sema_value--; return 0; }
int PollSema(int s) { (void)s; if (!sema_value) return -1; sema_value--; return 0; }
int SignalSema(int s) { (void)s; assert(!sema_value); sema_value++; return 0; }
int DelayThread(unsigned int usec)
{
    u64 until = now_us + usec + poll_overhead_us;
    while (alarm_callback && alarm_at <= until) {
        unsigned int next;
        now_us = alarm_at;
        next = alarm_callback(alarm_arg);
        if (next)
            alarm_at = now_us + (u64)next * 1000 / 36864;
        else
            alarm_callback = NULL;
    }
    now_us = until;
    assert(now_us < 1000000000);
    return 0;
}
int USec2SysClock(u32 usec, iop_sys_clock_t *c) { u64 ticks = (u64)usec * 36864 / 1000; c->lo = ticks; c->hi = ticks >> 32; return 0; }
int SetAlarm(iop_sys_clock_t *c, unsigned int (*cb)(void *), void *arg)
{
    assert(!alarm_callback);
    alarm_callback = cb; alarm_arg = arg;
    alarm_period = (((u64)c->hi << 32) | c->lo) * 1000 / 36864;
    alarm_at = now_us + alarm_period;
    return 0;
}
int CancelAlarm(unsigned int (*cb)(void *), void *arg) { (void)cb; (void)arg; alarm_callback = NULL; return 0; }
int RegisterLibraryEntries(struct irx_export_table *t) { (void)t; return 0; }
int sceCdTrayReq(int mode, u32 *count) { (void)mode; (void)count; tray_changes++; return 1; }
void cdvdman_kick_rthread(int from_intr) { (void)from_intr; read_wakeup = 1; }

int main(int argc, char **argv)
{
    u8 data[2048];
    u64 start;
    assert(argc >= 2);
    smb_NegotiateProt(hash_password);
    assert(DeviceFSInit() == 0);
    if (!strcmp(argv[1], "server_error")) {
        echo_error = 1;
        assert(smb_Echo() == -1);
        assert(now_us == 0 && sema_value == 1);
        puts("PASS: server error is rejected without undefined signed shifts");
        return 0;
    }
    if (!strcmp(argv[1], "lock_busy")) {
        DeviceLock();
        assert(smb_Echo() == 0 && echo_requests == 0);
        SignalSema(1);
        assert(smb_Echo() == 1);
        puts("PASS: Echo never waits for the VMC/IGR I/O lock");
        return 0;
    }
    if (!strcmp(argv[1], "send_blocked")) {
        send_blocked = require_nonblocking = 1;
        assert(smb_Echo() == -1);
        assert(now_us == 0 && sema_value == 1);
        assert(echo_requests == 0 && tray_changes == 0);
        send_blocked = 0;
        assert(smb_Echo() == 1);
        puts("PASS: unsent Echo releases the lock without counting a missing reply");
        return 0;
    }
    if (!strcmp(argv[1], "wrong_mid")) {
        wrong_mid_first = 1;
        assert(smb_Echo() == 1);
        assert(!test_socket.lastdata && response_pos == response_size);
        puts("PASS: old MID reply is drained before accepting the current Echo reply");
        return 0;
    }
    if (!strcmp(argv[1], "bad_echo")) {
        corrupt_echo = 1;
        assert(smb_Echo() == -1);
        assert(now_us == 0 && sema_value == 1);
        assert(DeviceReadSectors(20, data, 1) == SCECdErNO);
        puts("PASS: malformed Echo is a protocol error, not a missing reply");
        return 0;
    }
    if (!strcmp(argv[1], "closed")) {
        socket_closed = 1;
        assert(smb_Echo() == -1);
        assert(now_us == 0 && sema_value == 1 && tray_changes == 0);
        puts("PASS: closed connection does not wait or simulate an open tray during Echo");
        return 0;
    }
    if (!strcmp(argv[1], "shutdown")) {
        advance_idle(60000000);
        DeviceLock();
        DeviceUnmount();
        assert(!alarm_callback);
        assert(smb_read_thread_on_wake(0) == 1);
        assert(echo_requests == 1);
        puts("PASS: shutdown cancels idle work before closing the SMB connection");
        return 0;
    }
    if (!strcmp(argv[1], "read_priority")) {
        advance_idle(58000000);
        now_us = alarm_at;
        assert(alarm_callback(alarm_arg) != 0);
        read_wakeup = 0;
        assert(smb_read_thread_on_wake(1) == 0);
        assert(echo_requests == 0);
        assert(DeviceReadSectors(20, data, 1) == SCECdErNO);
        assert(smb_read_thread_on_wake(0) == 1);
        assert(echo_requests == 0);
        puts("PASS: queued disc reads take precedence over an idle Echo wakeup");
        return 0;
    }
    if (!strcmp(argv[1], "read_after_missing")) {
        drop_echo = 1;
        assert(smb_Echo() < 0);
        start = now_us;
        assert(DeviceReadSectors(20, data, 1) == SCECdErNO);
        assert(now_us == start && sema_value == 1);
        for (int i = 0; i < sizeof(data); i++)
            assert(data[i] == 0x5a);
        puts("PASS: missing Echo reply cannot hold later reads in a stale-packet drain loop");
        return 0;
    }
    if (!strcmp(argv[1], "timeout_cadence")) {
        drop_echo = 1;
        advance_idle(126000000);
        assert(echo_requests == 2);
        assert(echo_sent_at[0] == 60000000);
        assert(echo_sent_at[1] == 120000000);
        puts("PASS: timeout does not extend the one-minute Echo cadence");
        return 0;
    }
    if (!strcmp(argv[1], "send_nonblocking"))
        require_nonblocking = 1;
    if (!strcmp(argv[1], "idle")) {
        assert(DeviceReadSectors(20, data, 1) == SCECdErNO);
        advance_idle(59000000);
        assert(echo_requests == 0);
        advance_idle(60000000);
        assert(echo_requests == 1);
        advance_idle(120000000);
        assert(echo_requests == 2);
        if (phy_checks || tcp_probes || tray_changes) {
            fprintf(stderr, "FAIL: idle must only exchange SMB Echo; phy=%d tcp=%d tray=%d\n", phy_checks, tcp_probes, tray_changes);
            return 1;
        }
        puts("PASS: idle sends one Echo each minute without PHY checks or TCP probes");
        return 0;
    }
    if (!strcmp(argv[1], "timeout") || !strcmp(argv[1], "slow_poll") ||
        !strcmp(argv[1], "ignored_replies")) {
        if (!strcmp(argv[1], "slow_poll"))
            poll_overhead_us = 1000;
        if (!strcmp(argv[1], "ignored_replies"))
            wrong_mid_first = 200;
        drop_echo = 1;
        start = now_us;
        assert(smb_Echo() == SMB_ECHO_TIMEOUT);
        assert(now_us - start == (poll_overhead_us ? 6300000 : 6000000));
        if (!strcmp(argv[1], "ignored_replies"))
            assert(test_socket.lastdata);
        assert(sema_value == 1 && tray_changes == 0);
        puts("PASS: missing Echo releases I/O lock after about 6 seconds without opening tray");
        return 0;
    }
    if (!strcmp(argv[1], "late_partial")) {
        rx_prefix = response_size + (argc > 2 ? atoi(argv[2]) : 2);
        rx_release_us = now_us + 6500000;
        assert(smb_Echo() < 0);
        assert(sema_value == 1);
        memset(data, 0, sizeof(data));
        assert(DeviceReadSectors(20, data, 1) == SCECdErNO);
        for (int i = 0; i < sizeof(data); i++)
            assert(data[i] == 0x5a);
        puts("PASS: partial late Echo cannot corrupt the next disc read");
        return 0;
    }
    if (!strcmp(argv[1], "delayed_success")) {
        rx_prefix = response_size + 2;
        rx_release_us = 5980000;
        assert(smb_Echo() == 1);
        assert(now_us == 5980000 && sema_value == 1);
        puts("PASS: Echo is still accepted at 5.98 seconds");
        return 0;
    }
    assert(smb_Echo() == 1);
    if (echo_requests != 1 || tcp_probes != 0 || response_pos != response_size) {
        fprintf(stderr, "FAIL: Echo must send SMB_COM_ECHO and consume its reply; echo=%d tcp=%d unread=%d\n", echo_requests, tcp_probes, response_size - response_pos);
        return 1;
    }
    puts("PASS: SMB Echo request and reply");
    return 0;
}
