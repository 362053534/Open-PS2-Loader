#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sysclib.h>
#include <thbase.h>

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#include "slist.h"
#include "portable-endian.h"
#include "libsmb2-private.h"

#ifndef _U_
#define _U_
#endif

/* 与 libsmb2 IOP compat 对齐；强制有符号，避免 EAGAIN 被 size_t 吃掉 */
typedef int ssize_t;
struct iovec {
    void *iov_base;
    size_t iov_len;
};

int readv(int fd, const struct iovec *vector, int count);

void smb2_free_pdu(struct smb2_context *smb2, struct smb2_pdu *pdu);
int smb2_which_events(struct smb2_context *smb2);
int smb3_decrypt_pdu(struct smb2_context *smb2);

/* 避免 libsmb2 smb2_read_data 在栈上分配 iovec[256]（PCSX2 IOP 上 POLLIN 会跑飞） */
static struct iovec iov[SMB2_MAX_VECTORS];

typedef ssize_t (*read_func)(struct smb2_context *smb2,
                             const struct iovec *iov, int iovcnt);

static int smb2_read_data_safe(struct smb2_context *smb2, read_func func,
                          int has_xfrmhdr)
{
        /* iov moved to file-scope static */
        struct iovec *tmpiov;
        int i, niov, is_chained;
        size_t num_done;
        size_t iov_offset = 0;
        static char smb3tfrm[4] = {0xFD, 'S', 'M', 'B'};
        struct smb2_pdu *pdu = smb2->pdu;
        ssize_t count;
        int len;

read_more_data:
        num_done = smb2->in.num_done;

        /* Copy all the current vectors to our work vector */
        niov = smb2->in.niov;
        for (i = 0; i < niov; i++) {
                iov[i].iov_base = smb2->in.iov[i].buf;
#if defined(_WIN32) || defined(_XBOX)
                iov[i].iov_len = (unsigned long)smb2->in.iov[i].len;
#else
                iov[i].iov_len = (size_t)smb2->in.iov[i].len;
#endif
        }
        tmpiov = iov;

        /* Skip the vectors we have already read */
        while (num_done >= tmpiov->iov_len) {
                num_done -= tmpiov->iov_len;
                tmpiov++;
                niov--;
        }

        /* Adjust the first vector to read */
        tmpiov->iov_base = (char *)tmpiov->iov_base + num_done;
#if defined(_WIN32) || defined(_XBOX)
        tmpiov->iov_len -= (unsigned long)num_done;
#else
        tmpiov->iov_len -= (size_t)num_done;
#endif
        /* Read into our trimmed iovectors */
        count = func(smb2, tmpiov, niov);
        if (count < 0) {
#if defined(_WIN32) || defined(_XBOX)
                int err = WSAGetLastError();
                if (err == WSAEINTR || err == WSAEWOULDBLOCK) {
#else
                int err = errno;
                if (err == EINTR || err == EAGAIN || err == EWOULDBLOCK) {
#endif
                        return -EAGAIN;
                }
                smb2_set_error(smb2, "Read from socket failed, "
                               "errno:%d. Closing socket.", err);
                return -1;
        }
        if (count == 0) {
                /* remote side has closed the socket. */
                smb2_set_error(smb2, "Read from socket failed, "
                               "remote closed connection.");
                return -1;
        }
        smb2->in.num_done += (size_t)count;

        if (smb2->in.num_done < smb2->in.total_size) {
                goto read_more_data;
        }

        /* At this point we have all the data we need for the current phase */
        switch (smb2->recv_state) {
        case SMB2_RECV_SPL:
                smb2->spl = be32toh(smb2->spl);
                smb2->recv_state = SMB2_RECV_HEADER;
                if (smb2_add_iovector(smb2, &smb2->in, &smb2->header[0],
                                  SMB2_HEADER_SIZE, NULL) == NULL) {
                        smb2_set_error(smb2, "Too many I/O vectors when adding header");
                        return -1;
                }
                goto read_more_data;
        case SMB2_RECV_HEADER:
                if (!memcmp(smb2->in.iov[smb2->in.niov - 1].buf, smb3tfrm, 4)) {
                        smb2->in.iov[smb2->in.niov - 1].len = 52;
                        len = smb2->spl - 52;
                        smb2->in.total_size -= 12;
                        {
                                uint8_t *tmp = malloc(len);
                                if (tmp == NULL) {
                                        smb2_set_error(smb2, "malloc failed while adding TRFM payload");
                                        return -1;
                                }
                                if (smb2_add_iovector(smb2, &smb2->in,tmp,len, free) == NULL) {
                                        free(tmp);
                                        smb2_set_error(smb2, "Failed to add iovector for TRFM payload");
                                        return -1;
                                }
                        }
                        memcpy(smb2->in.iov[smb2->in.niov - 1].buf,
                               &smb2->in.iov[smb2->in.niov - 2].buf[52], 12);
                        smb2->recv_state = SMB2_RECV_TRFM;
                        goto read_more_data;
                }
                if (smb2_decode_header(smb2, &smb2->in.iov[smb2->in.niov - 1],
                                       &smb2->hdr) != 0) {
                        smb2_set_error(smb2, "Failed to decode smb2 "
                                       "header: %s", smb2_get_error(smb2));
                        return -1;
                }
                /* if serving, and this is an smb1 negotiate, just short-circuit and flush
                 * any remaining data on input and call the callback */
                if (smb2_is_server(smb2) && smb2->hdr.command == SMB1_NEGOTIATE) {
                        uint8_t flusher[32];
                        struct iovec fiov;
                        fiov.iov_base = (char *)flusher;
                        fiov.iov_len = sizeof(flusher);
                        do {
                                count = func(smb2, &fiov, 1);
                                if (count < 0) {
#if defined(_WIN32) || defined(_XBOX)
                                        int err = WSAGetLastError();
                                        if (err == WSAEINTR || err == WSAEWOULDBLOCK) {
#else
                                        int err = errno;
                                        if (err == EINTR || err == EAGAIN || err == EWOULDBLOCK) {
#endif
                                                count = 0;
                                        }
                                }
                        }
                        while (count > 0);

                        /* put on wait queue so queue_pdu doesn't complain */
                        SMB2_LIST_ADD_END(&smb2->waitqueue, pdu);

                        smb2->in.num_done = 0;
                        pdu->cb(smb2, smb2->hdr.status, pdu->payload, pdu->cb_data);
                        smb2->pdu = NULL;
                        smb2->pdu = smb2->next_pdu;
                        smb2->next_pdu = NULL;
                        return 0;
                }
                /* Record the offset for the start of payload data. */
                smb2->payload_offset = smb2->in.num_done;

                if (!smb2_is_server(smb2)) {
                        smb2->credits += smb2->hdr.credit_request_response;
                        /* Got credit, recheck if there are pending pdu to be sent. */
                        smb2_change_events(smb2, smb2->fd, smb2_which_events(smb2));
                }

                if (!smb2_is_server(smb2) && !(smb2->hdr.flags & SMB2_FLAGS_SERVER_TO_REDIR)) {
                        smb2_set_error(smb2, "received non-reply");
                        return -1;
                }
                else if (smb2_is_server(smb2) && (smb2->hdr.flags & SMB2_FLAGS_SERVER_TO_REDIR)) {
                        smb2_set_error(smb2, "received non-request");
                        return -1;
                }
                if (smb2->hdr.status == SMB2_STATUS_PENDING) {
                        /* Pending. Just treat the rest of the data as
                         * padding then check for and skip processing below.
                         * We will eventually receive a proper reply for this
                         * request sometime later.
                         */

                        len = smb2->spl - smb2->in.num_done;
                        /* If we don't have a transform header we are reading
                         * straight from the socket, and not a buffer,
                         * so we need to take the SPL size into account.
                         */
                        if (!has_xfrmhdr) {
                                len += SMB2_SPL_SIZE;
                        }
                        /* Add padding before the next PDU */
                        smb2->recv_state = SMB2_RECV_PAD;
                        {
                                uint8_t *tmp = malloc(len);
                                if (tmp == NULL) {
                                        smb2_set_error(smb2, "malloc failed while adding PENDING padding");
                                        return -1;
                                }
                                if (smb2_add_iovector(smb2, &smb2->in,tmp, len, free) == NULL) {
                                        free(tmp);
                                        return -1;
                                }
                        }
                        goto read_more_data;
                }

                if (smb2_is_server(smb2)) {
                        pdu = smb2->pdu;
                        if (!pdu) {
                                smb2_set_error(smb2, "no pdu for request");
                                return -1;
                        }
                        /* set the pdu header's message id to the request's id and
                        *  the tree id to the request's tree id
                        */
                        pdu->header.message_id = smb2->hdr.message_id;
                        if (!(smb2->hdr.flags & SMB2_FLAGS_ASYNC_COMMAND)) {
                                pdu->header.sync.tree_id = smb2->hdr.sync.tree_id;
                        }
                        /* if the session is properly opened then we could get
                         * any request from the client, so use the header's command
                         * not the pdu's command for the rest of input
                         */
                        if (smb2->hdr.command > SMB2_SESSION_SETUP) {
                                pdu->header.command = smb2->hdr.command;
                        }
                        pdu->header.credit_charge = smb2->hdr.credit_charge;
                        pdu->header.credit_request_response = smb2->hdr.credit_request_response;
                } else {
                        if ((smb2->hdr.command != SMB2_OPLOCK_BREAK) ||
                                        (smb2->hdr.message_id != 0xffffffffffffffffULL)) {
                                if (smb2->pdu) {
                                        smb2_free_pdu(smb2, smb2->pdu);
                                        smb2->pdu = NULL;
                                }
                                pdu = smb2->pdu = smb2_find_pdu(smb2, smb2->hdr.message_id);
                                if (pdu == NULL) {
                                        len = smb2->spl - smb2->in.num_done;
                                        if (!has_xfrmhdr) {
                                                len += SMB2_SPL_SIZE;
                                        }
                                        if (len > SMB2_MAX_PDU_SIZE) {
                                                smb2_set_error(smb2, "no matching PDU found");
                                                return -1;
                                        }
                                        smb2->recv_state = SMB2_RECV_UNKNOWN;
                                        {
                                                uint8_t *tmp = malloc(len);
                                                if (tmp == NULL) {
                                                        smb2_set_error(smb2, "malloc failed while adding UNKNOWN padding");
                                                        return -1;
                                                }
                                                if (smb2_add_iovector(smb2, &smb2->in, tmp, len, free) == NULL) {
                                                        free(tmp);
                                                        return -1;
                                                }
                                        }
                                        goto read_more_data;
                                }
                                /*
                                 * If part of a compound chain, verify that
                                 * the previous command has been processed.
                                 */
                                if (pdu->prev_compound_mid &&
                                    smb2_find_pdu(smb2, pdu->prev_compound_mid)) {
                                        smb2_set_error(smb2, "compound reply received out of order");
                                        return -1;
                                }

                                SMB2_LIST_REMOVE(&smb2->waitqueue, pdu);
                        } else {
                                /* oplock and lease break notifications won't have a pdu so make one
                                 * oplock replies (that are NOT notifications, i.e. have a valid message_id)
                                 * are normal replies handled above */
                                pdu = smb2->pdu;
                                if (!pdu) {
                                        pdu = smb2->pdu = smb2_allocate_pdu(smb2, SMB2_OPLOCK_BREAK,
                                                smb2_oplock_break_notify,  NULL);
                                }
                                if (pdu == NULL) {
                                       smb2_set_error(smb2, "can not alloc pdu");
                                       return -1;
                                }
                        }
                }

                len = smb2_get_fixed_size(smb2, pdu);
                if (((int)len) < 0) {
                        smb2_set_error(smb2, "can not determine fixed size");
                        return -1;
                }

                smb2->recv_state = SMB2_RECV_FIXED;
                {
                        size_t alen = len & 0xfffe;
                        uint8_t *tmp = malloc(alen);
                        if (tmp == NULL) {
                                smb2_set_error(smb2, "malloc failed while adding FIXED payload");
                                return -1;
                        }
                        if (smb2_add_iovector(smb2, &smb2->in,
                                  tmp,
                                  alen, free) == NULL) {
                                free(tmp);
                                return -1;
                        }
                }
                goto read_more_data;
        case SMB2_RECV_FIXED:
                len = smb2_process_payload_fixed(smb2, pdu);
                if (len < 0) {
                        smb2_set_error(smb2, "Failed to parse fixed part of "
                                       "command payload. %s",
                                       smb2_get_error(smb2));
                        return -1;
                }

                /* Add application provided iovectors */
                if (len) {
                        for (i = 0; i < pdu->in.niov; i++) {
                                size_t num = pdu->in.iov[i].len;

                                if (num > (size_t)len) {
                                        num = (size_t)len;
                                }
                                if (smb2_add_iovector(smb2, &smb2->in,
                                                  pdu->in.iov[i].buf,
                                                  num, NULL) == NULL) {
                                        return -1;
                                }
                                len -= num;

                                if (len == 0) {
                                        smb2->recv_state = SMB2_RECV_VARIABLE;
                                        goto read_more_data;
                                }
                        }
                        if (len > 0) {
                                smb2->recv_state = SMB2_RECV_VARIABLE;
                                {
                                        uint8_t *tmp = malloc(len);
                                        if (tmp == NULL) {
                                                smb2_set_error(smb2, "malloc failed while adding VARIABLE tail");
                                                return -1;
                                        }
                                        if (smb2_add_iovector(smb2, &smb2->in,
                                                  tmp,
                                                  len, free) == NULL) {
                                                free(tmp);
                                                return -1;
                                        }
                                }
                                goto read_more_data;
                        }
                }

                /* Check for padding */
                if (smb2->hdr.next_command) {
                        len = smb2->hdr.next_command - (SMB2_HEADER_SIZE +
                                  smb2->in.num_done - smb2->payload_offset);
                } else {
                        len = smb2->spl + SMB2_SPL_SIZE - smb2->in.num_done;
                        /*
                         * We never read the SPL when handling decrypted
                         * payloads.
                         */
                        if (smb2->enc) {
                                len -= SMB2_SPL_SIZE;
                        }
                }
                if (len < 0) {
                        smb2_set_error(smb2, "Negative number of PAD bytes "
                                       "encountered during PDU decode of"
                                       "fixed payload");
                        return -1;
                }
                if (len > 0) {
                        /* Add padding before the next PDU */
                        smb2->recv_state = SMB2_RECV_PAD;
                        {
                                uint8_t *tmp = malloc(len);
                                if (tmp == NULL) {
                                        smb2_set_error(smb2, "malloc failed while adding PAD");
                                        return -1;
                                }
                                if (smb2_add_iovector(smb2, &smb2->in,
                                          tmp,
                                          len, free) == NULL) {
                                        free(tmp);
                                        return -1;
                                }
                        }
                        goto read_more_data;
                }

                /* If len == 0 it means there is no padding and we are finished
                 * reading this PDU */
                break;
        case SMB2_RECV_VARIABLE:
                if (smb2_process_payload_variable(smb2, pdu) < 0) {
                        smb2_set_error(smb2, "Failed to parse variable part of "
                                       "command payload. %s",
                                       smb2_get_error(smb2));
                        return -1;
                }

                /* Check for padding */
                if (smb2->hdr.next_command) {
                        len = smb2->hdr.next_command - (SMB2_HEADER_SIZE +
                                  smb2->in.num_done - smb2->payload_offset);
                } else {
                        len = smb2->spl + SMB2_SPL_SIZE - smb2->in.num_done;
                        /*
                         * We never read the SPL when handling decrypted
                         * payloads.
                         */
                        if (smb2->enc) {
                                len -= SMB2_SPL_SIZE;
                        }
                }
                if (len < 0) {
                        smb2_set_error(smb2, "Negative number of PAD bytes "
                                       "encountered during PDU decode of"
                                       "variable payload");
                        return -1;
                }
                if (len > 0) {
                        /* Add padding before the next PDU */
                        smb2->recv_state = SMB2_RECV_PAD;
                        uint8_t * tmp = malloc(len);
                        if (tmp == NULL) {
                            smb2_set_error(smb2, "malloc failed while adding PAD");
                            return -1;
                        }
                        if (smb2_add_iovector(smb2, &smb2->in,
                                              tmp,
                                              len, free) == NULL) {
                                free(tmp);
                                smb2_set_error(smb2, "Failed to add iovector for PAD");
                                return -1;
                        }
                        goto read_more_data;
                }

                /* If len == 0 it means there is no padding and we are finished
                 * reading this PDU */
                break;
        case SMB2_RECV_PAD:
                /* We are finished reading all the data and padding for this
                 * PDU. Break out of the switch and invoke the callback.
                 */
                break;
        case SMB2_RECV_TRFM:
                /* We are finished reading the full payload for the
                 * encrypted packet.
                 */
                smb2->in.num_done = 0;
                if (smb3_decrypt_pdu(smb2)) {
                        smb2_set_error(smb2, "Failed to decrypyt pdu");
                        return -1;
                }
                /* We are all done now with this PDU. Reset num_done to 0
                 * and restart with a new SPL for the next chain.
                 */
                return 0;
        case SMB2_RECV_UNKNOWN:
                /* We have finished reading the payload the the unknown reply we
                 * just received. As it is not matching anything we are waiting on
                 * there is no PDU associated with this and thus nothing else we need
                 * to do.
                 */
                smb2->in.num_done = 0;
                return 0;
        }

        if (smb2->in.niov < 2) {
                smb2_set_error(smb2, "Too few io vectors in received PDU.");
                return -1;
        }

        if (smb2->hdr.status == SMB2_STATUS_PENDING) {
                /* This was a pending command. Just ignore it and proceed
                 * to read the next chain.
                 */
                if (smb2->passthrough) {
                        pdu = smb2_find_pdu(smb2, smb2->hdr.message_id);
                        if (pdu == NULL) {
                                smb2_set_error(smb2, "no matching PDU found");
                                /* ignore this error for now, it might be OK
                                 * to not pass the pending reply along */
                                /*return -1;*/
                        }
                        else
                        {
                                /* need to pass all pdus through note we do not free
                                * the pdu or delist the request */
                                pdu->cb(smb2, smb2->hdr.status, pdu->payload, pdu->cb_data);
                        }
                }
                smb2->in.num_done = 0;
                return 0;
        }

        /* We don't yet have the signing key until later, once session
         * setup has completed, so we can not yet verify the signature
         * of the final leg of session setup.
         */
        if (smb2->sign &&
            (smb2->hdr.flags & SMB2_FLAGS_SIGNED) &&
            (smb2->hdr.command != SMB2_SESSION_SETUP) ) {
                uint8_t signature[16] _U_;
                memcpy(&signature[0], &smb2->in.iov[1 + iov_offset].buf[48], 16);
                if (smb2_calc_signature(smb2, &smb2->in.iov[1 + iov_offset].buf[48],
                                        &smb2->in.iov[1 + iov_offset],
                                        smb2->in.niov - 1 - iov_offset) < 0) {
                        smb2_set_error(smb2, "Signature calc failed.");
                        return -1;
                }
                if (memcmp(&signature[0], &smb2->in.iov[1 + iov_offset].buf[48], 16)) {
                        smb2_set_error(smb2, "Wrong signature in received "
                                       "PDU");
                        return -1;
                }
        }

        is_chained = smb2->hdr.next_command;

        if (smb2_is_server(smb2)) {
                /* queue requests to correlate our replies we send back later */
                SMB2_LIST_ADD_END(&smb2->waitqueue, pdu);
                pdu->cb(smb2, smb2->hdr.status, pdu->payload, pdu->cb_data);
                smb2->pdu = smb2->next_pdu;
                smb2->next_pdu = NULL;
        } else {
                pdu->cb(smb2, smb2->hdr.status, pdu->payload, pdu->cb_data);
                if (!pdu->caller_frees_pdu) {
                        smb2_free_pdu(smb2, pdu);
                }
                smb2->pdu = NULL;
        }

        if (is_chained) {
                /* Record at which iov we ended in this loop so we know where to start in the next */
                iov_offset = smb2->in.niov - 1;
                smb2->recv_state = SMB2_RECV_HEADER;
                if (smb2_add_iovector(smb2, &smb2->in, &smb2->header[0],
                                  SMB2_HEADER_SIZE, NULL) == NULL) {
                        smb2_set_error(smb2, "Too many I/O vectors when adding chained header");
                        return -1;
                }
                goto read_more_data;
        }

        /* We are all done now with this chain. Reset num_done to 0
         * and restart with a new SPL for the next chain.
         */
        smb2->in.num_done = 0;

        return 0;
}

static ssize_t smb2_readv_from_socket_safe(struct smb2_context *smb2,
                                      const struct iovec *iov, int iovcnt)
{
        ssize_t rc = readv(smb2->fd, (struct iovec*) iov, iovcnt);
        return rc;
}

static int smb2_read_from_socket_safe(struct smb2_context *smb2)
{
        int count;

        while(1) {
                /* initialize the input vectors to the spl and the header
                 * which are both static data in the smb2 context.
                 * additional vectors will be added when we can map this to
                 * the corresponding pdu.
                 */
                if (smb2->in.num_done == 0) {
                        smb2->recv_state = SMB2_RECV_SPL;
                        smb2->spl = 0;

                        smb2_free_iovector(smb2, &smb2->in);
                        if (smb2_add_iovector(smb2, &smb2->in, (uint8_t *)&smb2->spl,
                                          SMB2_SPL_SIZE, NULL) == NULL) {
                                smb2_set_error(smb2, "Too many I/O vectors when adding SPL");
                                return -1;
                        }
                }

                count = smb2_read_data_safe(smb2, smb2_readv_from_socket_safe, 0);
                if (count == -EAGAIN) {
                        return 0;
                }
                if (count) {
                        return count;
                }
        }
}

int smb2_polllin_safe(struct smb2_context *smb2)
{
        int count;

        printf("SMB2TEST: polllin_safe begin stack=%d\n", CheckThreadStack());
        count = smb2_read_from_socket_safe(smb2);
        printf("SMB2TEST: polllin_safe done r=%d\n", count);
        return count;
}
