#ifndef SMB_SEND_HOST_H
#define SMB_SEND_HOST_H

#include "host.h"
#include <errno.h>
typedef int err_t;
typedef u8 u8_t;
typedef u16 u16_t;
typedef int sys_mbox_t;
#define ERR_OK 0
#define ERR_MEM -1
#define ERR_CONN -6
#define ERR_VAL -7
#define ERR_ARG -8
#define ERR_RST -4
#define NETCONN_COPY 1
#define NETCONN_EVT_SENDMINUS 4
#define TCP_SNDLOWAT 128
#define ESTABLISHED 4
#define CLOSE_WAIT 5
#define LWIP_TCP 1
#define MEMP_API_MSG 1
#define SOCKETS_DEBUG 0
#define LWIP_DEBUGF(flag, args) ((void)0)
#define TCPIP_APIMSG_ACK(box) ((void)0)
enum netconn_type { NETCONN_RAW, NETCONN_UDP, NETCONN_UDPLITE, NETCONN_UDPNOCHKSUM, NETCONN_TCP };
struct tcp_pcb { int state; u16 snd_buf; };
struct netconn {
    enum netconn_type type;
    union { struct tcp_pcb *tcp; } pcb;
    err_t err;
    sys_mbox_t mbox;
    void (*callback)(struct netconn *, int, u16);
};
struct netbuf { struct pbuf *p; };
struct lwip_socket { struct netconn *conn; int err; };
struct api_msg;
#define api_msg_post(msg) test_api_msg_post(msg)
void test_api_msg_post(struct api_msg *msg);
void *memp_malloc(int kind);
void memp_free(int kind, void *memory);
#define tcp_sndbuf(pcb) ((pcb)->snd_buf)
err_t tcp_write(struct tcp_pcb *pcb, void *data, u16 size, u8 copy);
err_t tcp_output(struct tcp_pcb *pcb);
struct lwip_socket *get_socket(int s);
int netconn_type(struct netconn *conn);
void sock_set_errno(struct lwip_socket *sock, int error);
#define set_errno(error) (errno = (error))
int err_to_errno(err_t error);
struct netbuf *netbuf_new(void);
err_t netbuf_ref(struct netbuf *buf, void *data, int size);
err_t netconn_send(struct netconn *conn, struct netbuf *buf);
void netbuf_delete(struct netbuf *buf);
err_t netconn_write(struct netconn *conn, void *data, u16 size, u8 copy);

#endif
