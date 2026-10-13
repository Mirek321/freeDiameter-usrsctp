#include "fdcore-internal.h"
#include "cnxctx.h"

#include <usrsctp.h>

static int fd_usrsctp_create_server(struct cnxctx *conn,
                                    int family,
                                    struct fd_list *endpoints,
                                    uint16_t port)
{
    struct socket *sock = NULL;
    struct sctp_initmsg init;
    int reuse = 1;
    int ret;
    int count = 0;
    size_t max_addresses = 0;
    uint8_t *addresses = NULL;
    uint8_t *cursor;
    struct fd_list *li;

    if (!conn || !port || (family != AF_INET && family != AF_INET6))
        return EINVAL;

    sock = usrsctp_socket(family, SOCK_STREAM, IPPROTO_SCTP,
                          NULL, NULL, 0, NULL);
    if (!sock)
        return errno ? errno : EIO;

    conn->cc_transport.type = FD_CNX_TRANSPORT_USRSCTP;
    conn->cc_transport.endpoint.usrsctp = sock;

    if (usrsctp_setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                           &reuse, sizeof(reuse)) < 0) {
        ret = errno;
        goto fail;
    }

    memset(&init, 0, sizeof(init));
    init.sinit_num_ostreams = fd_g_config->cnf_sctp_str;
    init.sinit_max_init_timeo = CNX_TIMEOUT * 1000;
    if (usrsctp_setsockopt(sock, IPPROTO_SCTP, SCTP_INITMSG,
                           &init, sizeof(init)) < 0) {
        ret = errno;
        goto fail;
    }

    /* Count configured endpoints to size the packed bindx address list. */
    if (endpoints) {
        for (li = endpoints->next; li != endpoints; li = li->next) {
            struct fd_endpoint *ep = (struct fd_endpoint *)li;
            if ((ep->flags & EP_FL_CONF) == EP_FL_CONF)
                max_addresses++;
        }
    }

    if (max_addresses == 0) {
        if (family == AF_INET) {
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            ret = usrsctp_bind(sock, (struct sockaddr *)&addr, sizeof(addr));
        } else {
            struct sockaddr_in6 addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin6_family = AF_INET6;
            addr.sin6_port = htons(port);
            ret = usrsctp_bind(sock, (struct sockaddr *)&addr, sizeof(addr));
        }

        if (ret < 0) {
            ret = errno;
            goto fail;
        }
    } else {
        if (max_addresses > SIZE_MAX / sizeof(struct sockaddr_in6) ||
            max_addresses > INT_MAX) {
            ret = EOVERFLOW;
            goto fail;
        }

        /* bindx expects sockaddr records packed consecutively, not an array
         * of sockaddr_storage objects. */
        addresses = malloc(max_addresses * sizeof(struct sockaddr_in6));
        if (!addresses) {
            ret = ENOMEM;
            goto fail;
        }
        cursor = addresses;

        for (li = endpoints->next; li != endpoints; li = li->next) {
            struct fd_endpoint *ep = (struct fd_endpoint *)li;

            if ((ep->flags & EP_FL_CONF) != EP_FL_CONF)
                continue;

            if (family == AF_INET) {
                struct sockaddr_in addr;
                if (ep->sa.sa_family != AF_INET)
                    continue;

                addr = ep->sin;
                addr.sin_port = htons(port);
                memcpy(cursor, &addr, sizeof(addr));
                cursor += sizeof(addr);
                count++;
            } else if (ep->sa.sa_family == AF_INET6) {
                struct sockaddr_in6 addr = ep->sin6;
                addr.sin6_port = htons(port);
                memcpy(cursor, &addr, sizeof(addr));
                cursor += sizeof(addr);
                count++;
            } else if (ep->sa.sa_family == AF_INET) {
#ifndef SCTP_USE_MAPPED_ADDRESSES
                struct sockaddr_in addr = ep->sin;
                addr.sin_port = htons(port);
                memcpy(cursor, &addr, sizeof(addr));
                cursor += sizeof(addr);
#else
                struct sockaddr_in6 addr;
                memset(&addr, 0, sizeof(addr));
                addr.sin6_family = AF_INET6;
                addr.sin6_port = htons(port);
                addr.sin6_addr.s6_addr[10] = 0xff;
                addr.sin6_addr.s6_addr[11] = 0xff;
                memcpy(&addr.sin6_addr.s6_addr[12],
                       &ep->sin.sin_addr, sizeof(ep->sin.sin_addr));
                memcpy(cursor, &addr, sizeof(addr));
                cursor += sizeof(addr);
#endif
                count++;
            }
        }

        if (count == 0) {
            /* No configured endpoint matched this socket family. */
            ret = EAFNOSUPPORT;
            goto fail;
        }

        if (usrsctp_bindx(sock, (struct sockaddr *)addresses,
                          count, SCTP_BINDX_ADD_ADDR) < 0) {
            ret = errno;
            goto fail;
        }
    }

    free(addresses);
    return 0;

fail:
    free(addresses);
    usrsctp_close(sock);
    conn->cc_transport.endpoint.usrsctp = NULL;
    return ret;
}

static int fd_usrsctp_listen(struct cnxctx *conn, int backlog)
{
    TRACE_ENTRY("%p %d", conn, backlog);

    CHECK_PARAMS(conn);
    CHECK_PARAMS(conn->cc_transport.endpoint.usrsctp);

    CHECK_SYS(usrsctp_listen(
        conn->cc_transport.endpoint.usrsctp,
        backlog
    ));

    return 0;
}

static int fd_usrsctp_accept(struct cnxctx *listener, struct cnxctx *accepted)
{
	struct socket *new_socket = NULL;
    struct sockaddr_storage peer_addr;
    socklen_t peer_len = sizeof(peer_addr);

    CHECK_PARAMS(listener != NULL);
    CHECK_PARAMS(accepted != NULL);
    CHECK_PARAMS(listener->cc_transport.type ==
                 FD_CNX_TRANSPORT_USRSCTP);

    memset(&peer_addr, 0, sizeof(peer_addr));

    new_socket = usrsctp_accept(
        listener->cc_transport.endpoint.usrsctp,
        (struct sockaddr *)&peer_addr,
        &peer_len);

    if (new_socket == NULL) {
        /* Prispôsob error handlingu projektu. */
        return errno ? errno : EIO;
    }

    accepted->cc_transport.type = FD_CNX_TRANSPORT_USRSCTP;
    accepted->cc_transport.endpoint.usrsctp = new_socket;
    accepted->cc_transport.ops = listener->cc_transport.ops;

    accepted->cc_family = listener->cc_family;
    accepted->cc_proto = IPPROTO_SCTP;

    int ret;
	char peer_host[NI_MAXHOST];
	char peer_service[NI_MAXSERV];

	ret = getnameinfo(
		(struct sockaddr *)&peer_addr,
		peer_len,
		peer_host, sizeof(peer_host),
		peer_service, sizeof(peer_service),
		NI_NUMERICHOST | NI_NUMERICSERV);

	if (ret != 0) {
		usrsctp_close(new_socket);
		accepted->cc_transport.endpoint.usrsctp = NULL;
		return EINVAL;
	}

	/* Zistenie počtu SCTP streamov. */
	ret = fd_usrsctp_get_str_info(
		new_socket,
		&accepted->cc_sctp_para.str_in,
		&accepted->cc_sctp_para.str_out);

	if (ret != 0) {
		usrsctp_close(new_socket);
		accepted->cc_transport.endpoint.usrsctp = NULL;
		return ret;
	}

	if (accepted->cc_sctp_para.str_out <
		accepted->cc_sctp_para.str_in) {
		accepted->cc_sctp_para.pairs =
			accepted->cc_sctp_para.str_out;
	} else {
		accepted->cc_sctp_para.pairs =
			accepted->cc_sctp_para.str_in;
	}
    return 0;
}

static int fd_usrsctp_connect(struct cnxctx *conn,
                              int no_ip6,
                              uint16_t port,
                              struct fd_list *remote,
                              struct fd_list *local)
{
   int family = no_ip6 ? AF_INET : AF_INET6;
    struct socket *sock = NULL;
    struct sctp_initmsg init;
    uint8_t *local_addresses = NULL;
    uint8_t *remote_addresses = NULL;
    size_t local_length = 0;
    size_t remote_length = 0;
    int local_count = 0;
    int remote_count = 0;
    int reuse = 1;
    int ret;
    struct sockaddr *peer_addresses = NULL;

    if (!conn || !port || !remote || FD_IS_LIST_EMPTY(remote))
        return EINVAL;

    conn->cc_transport.type = FD_CNX_TRANSPORT_USRSCTP;
    conn->cc_transport.ops = &fd_usrsctp_ops;
    conn->cc_transport.endpoint.usrsctp = NULL;
    conn->cc_family = family;
    conn->cc_proto = IPPROTO_SCTP;

    sock = usrsctp_socket(family, SOCK_STREAM, IPPROTO_SCTP,
                          NULL, NULL, 0, NULL);
    if (!sock)
        return errno ? errno : EIO;

    conn->cc_transport.endpoint.usrsctp = sock;

    if (usrsctp_setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                           &reuse, sizeof(reuse)) < 0) {
        ret = errno;
        goto fail;
    }

    memset(&init, 0, sizeof(init));
    init.sinit_num_ostreams = fd_g_config->cnf_sctp_str;
    init.sinit_max_init_timeo = CNX_TIMEOUT * 1000;
    if (usrsctp_setsockopt(sock, IPPROTO_SCTP, SCTP_INITMSG,
                           &init, sizeof(init)) < 0) {
        ret = errno;
        goto fail;
    }

    /* Bind only configured source addresses; port 0 lets usrsctp choose a port. */
    if (local && !FD_IS_LIST_EMPTY(local)) {
        ret = fd_usrsctp_append_endpoint_pass(
            &local_addresses, &local_length, family, local, 0,
            EP_FL_CONF, EP_FL_CONF, &local_count);
        if (ret)
            goto fail;

        if (local_count &&
            usrsctp_bindx(sock, (struct sockaddr *)local_addresses,
                          local_count, SCTP_BINDX_ADD_ADDR) < 0) {
            ret = errno;
            goto fail;
        }
    }

    /* Try configured, discovered, then other remote addresses, as fd_sctp_client does. */
    ret = fd_usrsctp_append_endpoint_pass(
        &remote_addresses, &remote_length, family, remote, htons(port),
        EP_FL_CONF, EP_FL_CONF, &remote_count);
    if (ret)
        goto fail;

    ret = fd_usrsctp_append_endpoint_pass(
        &remote_addresses, &remote_length, family, remote, htons(port),
        EP_FL_CONF | EP_FL_DISC, EP_FL_DISC, &remote_count);
    if (ret)
        goto fail;

    ret = fd_usrsctp_append_endpoint_pass(
        &remote_addresses, &remote_length, family, remote, htons(port),
        EP_FL_CONF | EP_FL_DISC, 0, &remote_count);
    if (ret)
        goto fail;

    if (!remote_count) {
        ret = EAFNOSUPPORT;
        goto fail;
    }

    if (usrsctp_connectx(sock, (const struct sockaddr *)remote_addresses,
                         remote_count, NULL) < 0) {
        ret = errno;
        goto fail;
    }

    ret = fd_usrsctp_get_str_info(sock,
                                  &conn->cc_sctp_para.str_in,
                                  &conn->cc_sctp_para.str_out);
    if (ret)
        goto fail;

    conn->cc_sctp_para.pairs =
        conn->cc_sctp_para.str_in < conn->cc_sctp_para.str_out
            ? conn->cc_sctp_para.str_in
            : conn->cc_sctp_para.str_out;

    /* Use the first peer address to populate the remote ID. */
    if (usrsctp_getpaddrs(sock, 0, &peer_addresses) < 0) {
        ret = errno;
        goto fail;
    }
    if (!peer_addresses) {
        ret = ENOTCONN;
        goto fail;
    }

    ret = getnameinfo(peer_addresses,
                      peer_addresses->sa_family == AF_INET
                          ? sizeof(struct sockaddr_in)
                          : sizeof(struct sockaddr_in6),
                      conn->cc_remid, sizeof(conn->cc_remid),
                      NULL, 0, 0);
    usrsctp_freepaddrs(peer_addresses);
    peer_addresses = NULL;
    if (ret) {
        ret = EINVAL;
        goto fail;
    }

    free(local_addresses);
    free(remote_addresses);
    return 0;

fail:
    if (peer_addresses)
        usrsctp_freepaddrs(peer_addresses);
    free(local_addresses);
    free(remote_addresses);
    if (sock)
        usrsctp_close(sock);
    conn->cc_transport.endpoint.usrsctp = NULL;
    return ret;
}

static int fd_usrsctp_start_clear(struct cnxctx *conn, int loop)
{
	(void)conn;
	(void)loop;
	return ENOTSUP;
}

static ssize_t fd_usrsctp_recv(struct cnxctx *conn, void *buffer, size_t length)
{
	(void)conn;
	(void)buffer;
	(void)length;
	errno = ENOTSUP;
	return -1;
}

static int fd_usrsctp_recv_meta(struct cnxctx *conn,
                                uint16_t *stream,
                                uint8_t **buffer,
                                size_t *length,
                                int *event)
{
	(void)conn;
	(void)stream;
	(void)buffer;
	(void)length;
	(void)event;
	return ENOTSUP;
}

static ssize_t fd_usrsctp_send(struct cnxctx *conn,
                               uint16_t stream,
                               const void *buffer,
                               size_t length,
                               int unordered)
{
    struct sctp_sndinfo sndinfo;
    struct socket *sock;
    ssize_t ret;

    if (conn == NULL || buffer == NULL || length == 0) {
        errno = EINVAL;
        return -1;
    }

    if (conn->cc_transport.type != FD_CNX_TRANSPORT_USRSCTP) {
        errno = EINVAL;
        return -1;
    }

    sock = conn->cc_transport.endpoint.usrsctp;
    if (sock == NULL) {
        errno = EBADF;
        return -1;
    }

    memset(&sndinfo, 0, sizeof(sndinfo));

    sndinfo.snd_sid = stream;
    sndinfo.snd_flags = unordered ? SCTP_UNORDERED : 0;

    ret = usrsctp_sendv(sock,
                        buffer,
                        length,
                        NULL,
                        0,
                        &sndinfo,
                        sizeof(sndinfo),
                        SCTP_SENDV_SNDINFO,
                        0);

    if (ret < 0)
        return -1;

    return ret;
}

static int fd_usrsctp_get_streams(struct cnxctx *conn,
                                  uint16_t *in,
                                  uint16_t *out)
{
	(void)conn;
	(void)in;
	(void)out;
	return ENOTSUP;
}

static int fd_usrsctp_get_remote_eps(struct cnxctx *conn, struct fd_list *eps)
{
	(void)conn;
	(void)eps;
	return ENOTSUP;
}

static void fd_usrsctp_close(struct cnxctx *conn)
{
	(void)conn;
}

static int fd_usrsctp_get_str_info(
    struct socket *sock,
    uint16_t *in,
    uint16_t *out)
{
    struct sctp_status status;
    socklen_t status_len = sizeof(status);

    if (!sock || !in || !out)
        return EINVAL;

    memset(&status, 0, sizeof(status));

    if (usrsctp_getsockopt(
            sock,
            IPPROTO_SCTP,
            SCTP_STATUS,
            &status,
            &status_len) < 0) {
        return errno ? errno : EIO;
    }

    if (status_len < sizeof(status))
        return ENOTSUP;

    *in = status.sstat_instrms;
    *out = status.sstat_outstrms;

    return 0;
}

static int fd_usrsctp_append_address(uint8_t **buffer,
                                    size_t *length,
                                    int family,
                                    const struct fd_endpoint *ep,
                                    uint16_t port)
{
    uint8_t *new_buffer;

    if (family == AF_INET) {
        struct sockaddr_in addr;

        if (ep->sa.sa_family != AF_INET)
            return 0;

        addr = ep->sin;
        addr.sin_port = htons(port);
        new_buffer = realloc(*buffer, *length + sizeof(addr));
        if (!new_buffer)
            return ENOMEM;

        *buffer = new_buffer;
        memcpy(*buffer + *length, &addr, sizeof(addr));
        *length += sizeof(addr);
        return 1;
    }

    if (family != AF_INET6)
        return EAFNOSUPPORT;

    if (ep->sa.sa_family == AF_INET6) {
        struct sockaddr_in6 addr = ep->sin6;
        addr.sin6_port = htons(port);
        new_buffer = realloc(*buffer, *length + sizeof(addr));
        if (!new_buffer)
            return ENOMEM;

        *buffer = new_buffer;
        memcpy(*buffer + *length, &addr, sizeof(addr));
        *length += sizeof(addr);
        return 1;
    }

    if (ep->sa.sa_family == AF_INET) {
#ifdef SCTP_USE_MAPPED_ADDRESSES
        struct sockaddr_in6 addr;

        memset(&addr, 0, sizeof(addr));
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(port);
        addr.sin6_addr.s6_addr[10] = 0xff;
        addr.sin6_addr.s6_addr[11] = 0xff;
        memcpy(&addr.sin6_addr.s6_addr[12],
               &ep->sin.sin_addr, sizeof(ep->sin.sin_addr));

        new_buffer = realloc(*buffer, *length + sizeof(addr));
        if (!new_buffer)
            return ENOMEM;

        *buffer = new_buffer;
        memcpy(*buffer + *length, &addr, sizeof(addr));
        *length += sizeof(addr);
#else
        struct sockaddr_in addr = ep->sin;
        addr.sin_port = htons(port);
        new_buffer = realloc(*buffer, *length + sizeof(addr));
        if (!new_buffer)
            return ENOMEM;

        *buffer = new_buffer;
        memcpy(*buffer + *length, &addr, sizeof(addr));
        *length += sizeof(addr);
#endif
        return 1;
    }

    return EAFNOSUPPORT;
}

static int fd_usrsctp_append_endpoint_pass(uint8_t **buffer,
                                           size_t *length,
                                           int family,
                                           struct fd_list *list,
                                           uint16_t port,
                                           uint32_t mask,
                                           uint32_t value,
                                           int *count)
{
    struct fd_list *li;

    for (li = list->next; li != list; li = li->next) {
        struct fd_endpoint *ep = (struct fd_endpoint *)li;
        int ret;

        if ((ep->flags & mask) != value)
            continue;

        ret = fd_usrsctp_append_address(buffer, length, family, ep, port);
        if (ret < 0)
            return ret;
        *count += ret;
    }

    return 0;
}

const struct fd_cnx_transport_ops fd_usrsctp_ops = {
	.create_server = fd_usrsctp_create_server,
	.listen = fd_usrsctp_listen,
	.accept = fd_usrsctp_accept,
	.connect = fd_usrsctp_connect,
	.start_clear = fd_usrsctp_start_clear,
	.recv = fd_usrsctp_recv,
	.recv_meta = fd_usrsctp_recv_meta,
	.send = fd_usrsctp_send,
	.get_streams = fd_usrsctp_get_streams,
	.get_remote_eps = fd_usrsctp_get_remote_eps,
	.close = fd_usrsctp_close
};
