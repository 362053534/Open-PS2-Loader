#ifndef SMB_TRANSPORT_H
#define SMB_TRANSPORT_H

#include "host.h"
#include <errno.h>
typedef u8 u8_t;
typedef u16 u16_t;
typedef u32 u32_t;
#define SOCKETS_DEBUG 0
#define LWIP_DEBUGF(flag, args) ((void)0)
#define NETCONN_TCP 0
#define O_NONBLOCK 0x800
#define mips_memcpy memcpy
#define mips_memset memset
#define ip_addr_debug_print(flag, addr) ((void)0)

struct pbuf { u8 data[32768]; u16 size; };
struct netbuf { struct pbuf *p, *ptr; };
struct ip_addr { u32 addr; };
struct netconn { int unused; };
struct lwip_socket {
    struct netconn *conn;
    struct netbuf *lastdata;
    u16 lastoffset;
    int rcvevent, flags, err;
};
extern struct lwip_socket test_socket;
struct lwip_socket *get_socket(int s);
void sock_set_errno(struct lwip_socket *s, int error);
#define set_errno(error) (errno = (error))
struct netbuf *netconn_recv(struct netconn *conn);
int netconn_type(struct netconn *conn);
void netbuf_delete(struct netbuf *buf);
u16 netbuf_len(struct netbuf *buf);
void pbuf_cat(struct pbuf *dst, struct pbuf *src);
void netbuf_copy_partial(struct netbuf *buf, void *dst, u16 size, u16 offset);
struct ip_addr *netbuf_fromaddr(struct netbuf *buf);
u16 netbuf_fromport(struct netbuf *buf);
int lwip_recvfrom(int s, void *header, int index, void *payload, int plen,
                  unsigned int flags, struct sockaddr *from, socklen_t *fromlen);

#endif
