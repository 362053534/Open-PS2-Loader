static struct tcp_pcb pcb = {.state = ESTABLISHED, .snd_buf = 128};
static struct netconn conn = {.type = NETCONN_TCP, .pcb.tcp = &pcb};
static struct lwip_socket sock = {.conn = &conn};
static int core_locked, no_memory;
static u8 wire[256];
static int wire_size;

void test_api_msg_post(struct api_msg *msg)
{
    assert(!core_locked);
    core_locked = 1;
    do_trywrite(&msg->msg);
    core_locked = 0;
}
void *memp_malloc(int kind) { (void)kind; return no_memory ? NULL : calloc(1, sizeof(struct api_msg)); }
void memp_free(int kind, void *memory) { (void)kind; free(memory); }
err_t tcp_write(struct tcp_pcb *p, void *data, u16 size, u8 copy)
{
    assert(core_locked && copy == NETCONN_COPY);
    assert(size <= p->snd_buf);
    memcpy(wire + wire_size, data, size);
    wire_size += size;
    p->snd_buf -= size;
    return ERR_OK;
}
err_t tcp_output(struct tcp_pcb *p) { (void)p; assert(core_locked); return ERR_OK; }
struct lwip_socket *get_socket(int s) { return s == 1 ? &sock : NULL; }
int netconn_type(struct netconn *c) { return c->type; }
void sock_set_errno(struct lwip_socket *s, int error) { s->err = error; }
int err_to_errno(err_t error) { return error == ERR_MEM ? ENOMEM : ECONNRESET; }
struct netbuf *netbuf_new(void) { abort(); }
err_t netbuf_ref(struct netbuf *b, void *d, int n) { (void)b; (void)d; (void)n; abort(); }
err_t netconn_send(struct netconn *c, struct netbuf *b) { (void)c; (void)b; abort(); }
void netbuf_delete(struct netbuf *b) { (void)b; abort(); }
err_t netconn_write(struct netconn *c, void *d, u16 n, u8 copy) { (void)c; (void)d; (void)n; (void)copy; abort(); }

int main(void)
{
    u8 data[42];
    memset(data, 0x45, sizeof(data));
    assert(lwip_send(1, data, sizeof(data), MSG_DONTWAIT) == 42);
    assert(wire_size == 42 && memcmp(wire, data, 42) == 0);
    assert(conn.err == ERR_OK && !core_locked);

    pcb.snd_buf = 41;
    assert(lwip_send(1, data, sizeof(data), MSG_DONTWAIT) == -1);
    assert(wire_size == 42 && conn.err == ERR_OK);
    pcb.snd_buf = 0;
    assert(lwip_send(1, data, sizeof(data), MSG_DONTWAIT) == -1);
    assert(wire_size == 42 && conn.err == ERR_OK);

    no_memory = 1;
    assert(lwip_send(1, data, sizeof(data), MSG_DONTWAIT) == -1);
    assert(wire_size == 42 && conn.err == ERR_OK);
    no_memory = 0;
    pcb.snd_buf = 42;
    assert(lwip_send(1, data, sizeof(data), MSG_DONTWAIT) == 42);
    assert(wire_size == 84 && conn.err == ERR_OK);

    assert(lwip_send(1, data, 65536, MSG_DONTWAIT) == -1);
    assert(lwip_send(1, data, -1, MSG_DONTWAIT) == -1);
    assert(wire_size == 84 && conn.err == ERR_OK);

    conn.err = ERR_RST;
    assert(lwip_send(1, data, sizeof(data), MSG_DONTWAIT) == -1);
    assert(wire_size == 84 && conn.err == ERR_RST);
    puts("PASS: actual lwIP nonblocking send is atomic, preserves connection errors and recovers after congestion");
    return 0;
}
