#!/usr/bin/env python3
"""复查改动前 TCP 保活是否会把旧堆内容当作 TCP 标志位发送。"""

import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
BASE = "1690253da723a58aa12c16eb82862a56c3fdf6bb"


def old(path):
    return subprocess.check_output(["git", "show", f"{BASE}:{path}"], cwd=ROOT, text=True)


header = old("modules/network/SMSTCPIP/include/lwip/tcp.h")
tcp = old("modules/network/SMSTCPIP/tcp_out.c")
declarations = header[header.index("struct tcp_hdr\n"):header.index("#define TCP_TCPLEN")]
declarations = declarations.replace('#ifdef PACK_STRUCT_USE_INCLUDES\n#include "arch/epstruct.h"\n#endif', '')
keepalive = tcp[tcp.index("void tcp_keepalive("):tcp.index("\n#endif", tcp.index("void tcp_keepalive("))]
source = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
typedef uint16_t u16_t;
typedef uint32_t u32_t;
#define PACK_STRUCT_FIELD(x) x
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_END
#define TCP_FLAGS 0x3f
#define TCP_HLEN 20
#define PBUF_IP 0
#define PBUF_RAM 0
#define IP_PROTO_TCP 6
#define LWIP_DEBUGF(flag, args) ((void)0)
#define TCP_STATS_INC(x) ((void)0)
struct tcp_pcb { u16_t local_port, remote_port; u32_t snd_nxt, rcv_nxt; u16_t rcv_wnd; int local_ip, remote_ip, ttl; };
struct pbuf { void *payload; int tot_len; };
static unsigned char poison;
static unsigned sent_flags;
static struct pbuf *pbuf_alloc(int layer, int size, int kind)
{
    struct pbuf *p = malloc(sizeof(*p));
    (void)layer; (void)kind;
    p->payload = malloc(size);
    memset(p->payload, poison, size);
    p->tot_len = size;
    return p;
}
static int inet_chksum_pseudo(struct pbuf *p, int *a, int *b, int proto, int len)
{ (void)p; (void)a; (void)b; (void)proto; (void)len; return 0; }
static void ip_output(struct pbuf *p, int *a, int *b, int ttl, int tos, int proto);
static void pbuf_free(struct pbuf *p) { free(p->payload); free(p); }
''' + declarations + keepalive + r'''
static void ip_output(struct pbuf *p, int *a, int *b, int ttl, int tos, int proto)
{ (void)a; (void)b; (void)ttl; (void)tos; (void)proto; sent_flags = TCPH_FLAGS((struct tcp_hdr *)p->payload); }
int main(void)
{
    struct tcp_pcb pcb = {0};
    poison = 0;
    tcp_keepalive(&pcb);
    printf("payload fill=0x00 -> TCP flags=0x%02x (ACK absent)\n", sent_flags);
    assert(sent_flags == 0);
    poison = 0xff;
    tcp_keepalive(&pcb);
    printf("payload fill=0xff -> TCP flags=0x%02x (FIN|SYN|RST|PSH|ACK|URG)\n", sent_flags);
    assert(sent_flags == 0x3f);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="opl-tcp-probe-") as tmp:
    path = pathlib.Path(tmp)
    (path / "probe.c").write_text(source)
    subprocess.run(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    str(path / "probe.c"), "-o", str(path / "probe")], check=True)
    subprocess.run([str(path / "probe")], check=True)
