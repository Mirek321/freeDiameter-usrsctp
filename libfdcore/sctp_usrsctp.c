#include "fdcore-internal.h"
#include "cnxctx.h"

#include <usrsctp.h>
#include <stdint.h>
#include <limits.h>

#define FD_USRSCTP_MAX_MESSAGE 65535

static struct fd_usrsctp_cbctx *fd_usrsctp_cbctx_new(struct cnxctx *conn);
static void fd_usrsctp_cbctx_free(struct fd_usrsctp_cbctx *ctx);
static int fd_usrsctp_get_str_info(struct socket *sock,
                                   uint16_t *in,
                                   uint16_t *out);
static int fd_usrsctp_append_endpoint_pass(uint8_t **buffer,
                                           size_t *length,
                                           int family,
                                           struct fd_list *list,
                                           uint16_t port,
                                           uint32_t mask,
                                           uint32_t value,
                                           int *count);

static void fd_usrsctp_free_partial(struct fd_usrsctp_partial *partial)
{
	if (!partial)
		return;
	free(partial->buffer);
	free(partial);
}



static struct fd_usrsctp_cbctx *fd_usrsctp_cbctx_new(struct cnxctx *conn)
{
	struct fd_usrsctp_cbctx *ctx = calloc(1, sizeof(*ctx));
	int ret;

	if (!ctx)
		return NULL;
	ret = pthread_mutex_init(&ctx->lock, NULL);
	if (ret) {
		free(ctx);
		errno = ret;
		return NULL;
	}
	ret = pthread_cond_init(&ctx->idle, NULL);
	if (ret) {
		pthread_mutex_destroy(&ctx->lock);
		free(ctx);
		errno = ret;
		return NULL;
	}
	ctx->conn = conn;
	return ctx;
}

static void fd_usrsctp_cbctx_free(struct fd_usrsctp_cbctx *ctx)
{
	struct fd_usrsctp_partial *partial;
	struct fd_usrsctp_data *queued;

	if (!ctx)
		return;
	pthread_mutex_lock(&ctx->lock);
	ctx->closing = 1;
	ctx->conn = NULL;
	pthread_cond_broadcast(&ctx->idle);
	while (ctx->callback_refs)
		pthread_cond_wait(&ctx->idle, &ctx->lock);
	pthread_mutex_unlock(&ctx->lock);

	while ((partial = ctx->partials) != NULL) {
		ctx->partials = partial->next;
		fd_usrsctp_free_partial(partial);
	}
	while ((queued = ctx->data_head) != NULL) {
		ctx->data_head = queued->next;
		free(queued->buffer);
		free(queued);
	}
	pthread_cond_destroy(&ctx->idle);
	pthread_mutex_destroy(&ctx->lock);
	free(ctx);
}

static int fd_usrsctp_cbctx_acquire(struct fd_usrsctp_cbctx *ctx,
                                    struct cnxctx **conn)
{
	int ret = 0;
	if (!ctx)
		return 0;
	pthread_mutex_lock(&ctx->lock);
	if (ctx->closing || !ctx->conn) {
		ret = 0;
	} else {
		ctx->callback_refs++;
		*conn = ctx->conn;
		ret = 1;
	}
	pthread_mutex_unlock(&ctx->lock);
	return ret;
}

static void fd_usrsctp_cbctx_release(struct fd_usrsctp_cbctx *ctx)
{
	pthread_mutex_lock(&ctx->lock);
	if (--ctx->callback_refs == 0)
		pthread_cond_broadcast(&ctx->idle);
	pthread_mutex_unlock(&ctx->lock);
}





static int fd_usrsctp_handle_message(struct fd_usrsctp_cbctx *ctx,
                                     struct socket *sock,
                                     void *data,
                                     size_t datalen,
                                     struct sctp_rcvinfo rcv,
                                     int flags)
{
	struct fd_usrsctp_partial **link;
	struct fd_usrsctp_partial *partial;
	struct cnxctx *conn = NULL;
	uint8_t *complete = NULL;
	size_t complete_len = 0;
	int notification = !!(flags & MSG_NOTIFICATION);
	int event = 0;
	int ret = 0;

	if (!fd_usrsctp_cbctx_acquire(ctx, &conn)) {
		free(data);
		LOG_D("[usrsctp] handle_message: connection closed");
		return 1;
	}

	pthread_mutex_lock(&ctx->lock);
	for (link = &ctx->partials; *link; link = &(*link)->next) {
		partial = *link;
		if (partial->socket == sock &&
		    partial->assoc_id == rcv.rcv_assoc_id &&
		    partial->stream == rcv.rcv_sid &&
		    partial->ssn == rcv.rcv_ssn &&
		    partial->notification == notification)
			break;
	}
	partial = *link;
	if (!partial) {
		partial = calloc(1, sizeof(*partial));
		if (!partial) {
			ret = ENOMEM;
			LOG_E("[usrsctp] failed to allocate partial message");
			goto unlock;
		}
		partial->socket = sock;
		partial->assoc_id = rcv.rcv_assoc_id;
		partial->stream = rcv.rcv_sid;
		partial->ssn = rcv.rcv_ssn;
		partial->notification = notification;
		partial->next = ctx->partials;
		ctx->partials = partial;
		link = &ctx->partials;
		LOG_D("[usrsctp] new partial message: assoc=%u stream=%u ssn=%u",
		      rcv.rcv_assoc_id, rcv.rcv_sid, rcv.rcv_ssn);
	}
	if (datalen > FD_USRSCTP_MAX_MESSAGE - partial->length) {
		*link = partial->next;
		fd_usrsctp_free_partial(partial);
		ret = EMSGSIZE;
		LOG_E("[usrsctp] message too large: %zu + %zu > %u",
		      partial->length, datalen, FD_USRSCTP_MAX_MESSAGE);
		goto unlock;
	}
	if (partial->length + datalen > partial->capacity) {
		size_t capacity = partial->capacity ? partial->capacity : 4096;
		uint8_t *new_buffer;
		while (capacity < partial->length + datalen) {
			if (capacity > FD_USRSCTP_MAX_MESSAGE / 2) {
				capacity = FD_USRSCTP_MAX_MESSAGE;
				break;
			}
			capacity *= 2;
		}
		new_buffer = realloc(partial->buffer, capacity);
		if (!new_buffer) {
			ret = ENOMEM;
			goto unlock;
		}
		partial->buffer = new_buffer;
		partial->capacity = capacity;
	}
	memcpy(partial->buffer + partial->length, data, datalen);
	partial->length += datalen;
	if (flags & MSG_EOR) {
		*link = partial->next;
		complete = partial->buffer;
		complete_len = partial->length;
		notification = partial->notification;
		LOG_D("[usrsctp] message complete: %zu bytes, notification=%d", complete_len, notification);
		free(partial);
	}

unlock:
	pthread_mutex_unlock(&ctx->lock);
	free(data);

	if (ret == 0 && complete) {
		if (notification) {
			if (complete_len < sizeof(((union sctp_notification *)0)->sn_header)) {
				ret = EPROTO;
				LOG_E("[usrsctp] notification too short: %zu bytes", complete_len);
			} else {
				union sctp_notification *notif = (union sctp_notification *)complete;
				LOG_D("[usrsctp] processing notification type %u", notif->sn_header.sn_type);
				switch (notif->sn_header.sn_type) {
				case SCTP_PEER_ADDR_CHANGE:
					event = FDEVP_CNX_EP_CHANGE;
					break;
				case SCTP_SHUTDOWN_EVENT:
					event = FDEVP_CNX_SHUTDOWN;
					break;
				case SCTP_ASSOC_CHANGE:
					event = FDEVP_CNX_EP_CHANGE;
					break;
				default:
					event = FDEVP_CNX_ERROR;
					LOG_E("[usrsctp] unknown notification type");
					break;
				}
				if (event == FDEVP_CNX_ERROR)
					fd_cnx_markerror(conn);
				else
					ret = fd_event_send(fd_cnx_target_queue(conn), event, 0, NULL);
			}
			free(complete);
		} else if (fd_cnx_teststate(conn, CC_STATUS_TLS)) {
			/* TLS: keep the bytes queued for the recv op, which the TLS pull callback drains. */
			struct fd_usrsctp_data *queued = calloc(1, sizeof(*queued));
			if (!queued) {
				ret = ENOMEM;
				free(complete);
			} else {
				queued->buffer = complete;
				queued->length = complete_len;
				pthread_mutex_lock(&ctx->lock);
				if (ctx->closing) {
					ret = ECANCELED;
				} else {
					if (ctx->data_tail)
						ctx->data_tail->next = queued;
					else
						ctx->data_head = queued;
					ctx->data_tail = queued;
					pthread_cond_broadcast(&ctx->idle);
				}
				pthread_mutex_unlock(&ctx->lock);
				if (ret) {
					free(queued);
					free(complete);
				}
			}
		} else {
			/* Clear text: copy into a message buffer the core owns, then queue it. */
			struct fd_cnx_rcvdata rcv_data;
			struct fd_msg_pmdl *pmdl;
			uint8_t *buffer;

			if (complete_len > FD_USRSCTP_MAX_MESSAGE) {
				ret = EMSGSIZE;
			} else {
				buffer = malloc(fd_msg_pmdl_sizewithoverhead(complete_len));
				if (!buffer) {
					ret = ENOMEM;
				} else {
					if (complete_len)
						memcpy(buffer, complete, complete_len);
					pmdl = fd_msg_pmdl_get_inbuf(buffer, complete_len);
					fd_list_init(&pmdl->sentinel, NULL);
					ret = pthread_mutex_init(&pmdl->lock, NULL);
					if (ret) {
						free(buffer);
					} else {
						rcv_data.length = complete_len;
						rcv_data.buffer = buffer;
						fd_hook_call(HOOK_DATA_RECEIVED, NULL, NULL, &rcv_data, pmdl);
						ret = fd_event_send(fd_cnx_target_queue(conn), FDEVP_CNX_MSG_RECV,
						                    complete_len, buffer);
						if (ret) {
							pthread_mutex_destroy(&pmdl->lock);
							free(buffer);
						}
					}
				}
			}
			free(complete);
		}
	}
	if (ret) {
		LOG_E("usrsctp receive callback failed: %s", strerror(ret));
		if (conn->cc_transport.endpoint.usrsctp == sock)
			fd_cnx_markerror(conn);
	}

	fd_usrsctp_cbctx_release(ctx);
	return 1;
}

static void *fd_usrsctp_reader(void *arg)
{
	struct cnxctx *conn = arg;
	struct fd_usrsctp_cbctx *ctx = conn->cc_usrsctp_cbctx;
	struct socket *sock = conn->cc_transport.endpoint.usrsctp;

	LOG_D("[usrsctp] reader thread started for %s:%u (fd=%p)",
	      conn->cc_remhost ? : "?", conn->cc_remport, sock);

	for (;;) {
		struct sctp_rcvinfo rcv;
		socklen_t infolen = sizeof(rcv);
		unsigned int infotype = SCTP_RECVV_RCVINFO;
		int flags = 0;
		uint8_t *data;
		ssize_t n;

		data = malloc(FD_USRSCTP_MAX_MESSAGE);
		n = data ? usrsctp_recvv(sock, data, FD_USRSCTP_MAX_MESSAGE,
		                         NULL, NULL, &rcv, &infolen, &infotype,
		                         &flags) : -1;
		if (n <= 0) {
			if (n < 0)
				LOG_D("[usrsctp] read error: %s", strerror(errno));
			else
				LOG_D("[usrsctp] connection closed (EOF)");
			free(data);
			break;
		}
		LOG_D("[usrsctp] received %zd bytes, stream %u, flags %x",
		      n, rcv.rcv_stream, flags);
		fd_usrsctp_handle_message(ctx, sock, data, (size_t)n, rcv, flags);
	}

	/* Read error or end of association: report it unless we are closing. */
	{
		struct cnxctx *live = NULL;
		if (fd_usrsctp_cbctx_acquire(ctx, &live)) {
			LOG_D("[usrsctp] marking connection as error");
			fd_cnx_markerror(live);
			fd_usrsctp_cbctx_release(ctx);
		}
	}
	LOG_D("[usrsctp] reader thread exiting");
	return NULL;
}



static pthread_once_t fd_usrsctp_once = PTHREAD_ONCE_INIT;
static int fd_usrsctp_initialized = 0;
static pthread_mutex_t fd_usrsctp_counter_lock = PTHREAD_MUTEX_INITIALIZER;
static int fd_usrsctp_live_sockets = 0;

/* usrsctp keeps process-wide state: initialise it once, before the first socket. */
static void fd_usrsctp_global_init(void)
{
	LOG_N("[usrsctp] initializing library");
	usrsctp_init(9899, NULL, NULL);
	fd_usrsctp_initialized = 1;
	LOG_N("[usrsctp] library initialized");
}

static void fd_usrsctp_ensure_init(void)
{
	pthread_once(&fd_usrsctp_once, fd_usrsctp_global_init);
}

static void fd_usrsctp_socket_inc(void)
{
	pthread_mutex_lock(&fd_usrsctp_counter_lock);
	fd_usrsctp_live_sockets++;
	pthread_mutex_unlock(&fd_usrsctp_counter_lock);
}

static void fd_usrsctp_socket_dec(void)
{
	pthread_mutex_lock(&fd_usrsctp_counter_lock);
	fd_usrsctp_live_sockets--;
	pthread_mutex_unlock(&fd_usrsctp_counter_lock);
}

void fd_usrsctp_global_finish(void)
{
	if (!fd_usrsctp_initialized) {
		TRACE_DEBUG(INFO, "usrsctp was never initialized, skipping finish");
		return;
	}

	pthread_mutex_lock(&fd_usrsctp_counter_lock);
	if (fd_usrsctp_live_sockets > 0) {
		LOG_E("usrsctp_finish skipped: %d socket(s) still open",
		      fd_usrsctp_live_sockets);
		pthread_mutex_unlock(&fd_usrsctp_counter_lock);
		return;
	}
	pthread_mutex_unlock(&fd_usrsctp_counter_lock);

	if (usrsctp_finish() != 0)
		LOG_E("usrsctp_finish returned -1");
	else
		TRACE_DEBUG(INFO, "usrsctp finished cleanly");
}

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

    LOG_D("[usrsctp] creating server socket on port %u, family %d", port, family);
    fd_usrsctp_ensure_init();
    conn->cc_usrsctp_cbctx = fd_usrsctp_cbctx_new(conn);
    if (!conn->cc_usrsctp_cbctx)
        return errno ? errno : ENOMEM;

    sock = usrsctp_socket(family, SOCK_STREAM, IPPROTO_SCTP,
                          NULL, NULL, 0,
                          NULL);
    if (!sock) {
        ret = errno ? errno : EIO;
        LOG_E("[usrsctp] failed to create socket: %s", strerror(ret));
        fd_usrsctp_cbctx_free(conn->cc_usrsctp_cbctx);
        conn->cc_usrsctp_cbctx = NULL;
        return ret;
    }
    LOG_D("[usrsctp] socket created: %p", sock);

    conn->cc_transport.type = FD_CNX_TRANSPORT_USRSCTP;
    conn->cc_transport.endpoint.usrsctp = sock;

    if (usrsctp_setsockopt(sock, IPPROTO_SCTP, SCTP_REUSE_PORT,
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

    {
        struct sctp_udpencaps encaps;
        memset(&encaps, 0, sizeof(encaps));
        encaps.sue_port = htons(9899);
        encaps.sue_assoc_id = SCTP_FUTURE_ASSOC;
        if (usrsctp_setsockopt(sock, IPPROTO_SCTP,
                               SCTP_REMOTE_UDP_ENCAPS_PORT,
                               &encaps, sizeof(encaps)) < 0) {
            ret = errno;
            goto fail;
        }
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
    fd_usrsctp_socket_inc();
    return 0;

fail:
    free(addresses);
    usrsctp_close(sock);
    conn->cc_transport.endpoint.usrsctp = NULL;
    fd_usrsctp_cbctx_free(conn->cc_usrsctp_cbctx);
    conn->cc_usrsctp_cbctx = NULL;
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

	accepted->cc_usrsctp_cbctx = fd_usrsctp_cbctx_new(accepted);
	if (!accepted->cc_usrsctp_cbctx) {
		ret = errno ? errno : ENOMEM;
		usrsctp_close(new_socket);
		accepted->cc_transport.endpoint.usrsctp = NULL;
		return ret;
	}
	ret = getnameinfo(
		(struct sockaddr *)&peer_addr,
		peer_len,
		peer_host, sizeof(peer_host),
		peer_service, sizeof(peer_service),
		NI_NUMERICHOST | NI_NUMERICSERV);

	if (ret != 0) {
		usrsctp_close(new_socket);
		accepted->cc_transport.endpoint.usrsctp = NULL;
		fd_usrsctp_cbctx_free(accepted->cc_usrsctp_cbctx);
		accepted->cc_usrsctp_cbctx = NULL;
		return EINVAL;
	}

    fd_usrsctp_socket_inc();
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

    LOG_D("[usrsctp] connecting to port %u, family %d (no_ip6=%d)", port, family, no_ip6);
    conn->cc_transport.type = FD_CNX_TRANSPORT_USRSCTP;
    conn->cc_transport.ops = &fd_usrsctp_ops;
    conn->cc_transport.endpoint.usrsctp = NULL;
    conn->cc_family = family;
    conn->cc_proto = IPPROTO_SCTP;

    fd_usrsctp_ensure_init();
    conn->cc_usrsctp_cbctx = fd_usrsctp_cbctx_new(conn);
    if (!conn->cc_usrsctp_cbctx)
        return errno ? errno : ENOMEM;

    sock = usrsctp_socket(family, SOCK_STREAM, IPPROTO_SCTP,
                          NULL, NULL, 0,
                          NULL);
    if (!sock) {
        ret = errno ? errno : EIO;
        fd_usrsctp_cbctx_free(conn->cc_usrsctp_cbctx);
        conn->cc_usrsctp_cbctx = NULL;
        return ret;
    }

    conn->cc_transport.endpoint.usrsctp = sock;

    if (usrsctp_setsockopt(sock, IPPROTO_SCTP, SCTP_REUSE_PORT,
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

    {
        struct sctp_udpencaps encaps;
        memset(&encaps, 0, sizeof(encaps));
        encaps.sue_port = htons(9899);
        encaps.sue_assoc_id = SCTP_FUTURE_ASSOC;
        if (usrsctp_setsockopt(sock, IPPROTO_SCTP,
                               SCTP_REMOTE_UDP_ENCAPS_PORT,
                               &encaps, sizeof(encaps)) < 0) {
            ret = errno;
            goto fail;
        }
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
    fd_usrsctp_socket_inc();
    return 0;

fail:
    if (peer_addresses)
        usrsctp_freepaddrs(peer_addresses);
    free(local_addresses);
    free(remote_addresses);
    if (sock)
        usrsctp_close(sock);
    conn->cc_transport.endpoint.usrsctp = NULL;
    fd_usrsctp_cbctx_free(conn->cc_usrsctp_cbctx);
    conn->cc_usrsctp_cbctx = NULL;
    return ret;
}

static int fd_usrsctp_start_clear(struct cnxctx *conn, int loop)
{
	struct fd_usrsctp_cbctx *ctx;
	int ret;

	(void)loop;
	if (!conn || conn->cc_transport.type != FD_CNX_TRANSPORT_USRSCTP ||
	    !conn->cc_transport.endpoint.usrsctp || !fd_cnx_target_queue(conn))
		return EINVAL;
	ctx = conn->cc_usrsctp_cbctx;
	if (!ctx)
		return EINVAL;
	if (ctx->reader_started)
		return 0;

	/* The reader thread relies on blocking usrsctp_recvv() calls. */
	if (usrsctp_set_non_blocking(conn->cc_transport.endpoint.usrsctp, 0) < 0)
		return errno ? errno : EIO;

	ret = pthread_create(&ctx->reader, NULL, fd_usrsctp_reader, conn);
	if (ret)
		return ret;
	ctx->reader_started = 1;
	return 0;
}

static ssize_t fd_usrsctp_recv(struct cnxctx *conn, void *buffer, size_t length)
{
	struct fd_usrsctp_cbctx *ctx;
	struct fd_usrsctp_data *queued;
	size_t copy_len;

	if (!conn || !buffer || !length || !(ctx = conn->cc_usrsctp_cbctx)) {
		errno = EINVAL;
		return -1;
	}

	pthread_mutex_lock(&ctx->lock);
	while (!ctx->data_head && !ctx->closing)
		pthread_cond_wait(&ctx->idle, &ctx->lock);
	if (!ctx->data_head) {
		pthread_mutex_unlock(&ctx->lock);
		LOG_D("[usrsctp] recv: connection closing");
		return 0;
	}

	queued = ctx->data_head;
	copy_len = queued->length - queued->offset;
	if (copy_len > length)
		copy_len = length;
	LOG_D("[usrsctp] recv: copying %zu bytes (queued %zu)", copy_len, queued->length);
	memcpy(buffer, queued->buffer + queued->offset, copy_len);
	queued->offset += copy_len;
	if (queued->offset == queued->length) {
		ctx->data_head = queued->next;
		if (!ctx->data_head)
			ctx->data_tail = NULL;
		free(queued->buffer);
		free(queued);
	}
	pthread_mutex_unlock(&ctx->lock);
	return (ssize_t)copy_len;
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

    LOG_D("[usrsctp] sending %zu bytes on stream %u (unordered=%d)", length, stream, unordered);
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

static int fd_usrsctp_get_remote_eps(struct cnxctx *conn, struct fd_list *eps)
{
	struct sockaddr *addresses = NULL;
	int count, i, ret = 0;
	uint8_t *cursor;

	if (!conn || !eps ||
	    conn->cc_transport.type != FD_CNX_TRANSPORT_USRSCTP ||
	    !conn->cc_transport.endpoint.usrsctp)
		return EINVAL;

	count = usrsctp_getpaddrs(conn->cc_transport.endpoint.usrsctp, 0,
	                          &addresses);
	if (count < 0)
		return errno ? errno : EIO;
	cursor = (uint8_t *)addresses;
	for (i = 0; i < count; i++) {
		struct sockaddr *sa = (struct sockaddr *)cursor;
		socklen_t length;
		switch (sa->sa_family) {
		case AF_INET:
			length = sizeof(struct sockaddr_in);
			break;
		case AF_INET6:
			length = sizeof(struct sockaddr_in6);
			break;
		default:
			LOG_E("usrsctp_getpaddrs returned unsupported address family %d",
			      sa->sa_family);
			ret = EAFNOSUPPORT;
			goto done;
		}
		ret = fd_ep_add_merge(eps, sa, length, EP_FL_LL);
		if (ret)
			goto done;
		cursor += length;
	}

done:
	if (addresses)
		usrsctp_freepaddrs(addresses);
	return ret;
}

static void fd_usrsctp_close(struct cnxctx *conn)
{
	struct fd_usrsctp_cbctx *ctx;
	struct socket *sock;

	if (!conn)
		return;
	LOG_D("[usrsctp] closing connection");
	ctx = conn->cc_usrsctp_cbctx;
	sock = conn->cc_transport.endpoint.usrsctp;
	conn->cc_transport.endpoint.usrsctp = NULL;

	if (ctx) {
		pthread_mutex_lock(&ctx->lock);
		ctx->closing = 1;
		pthread_mutex_unlock(&ctx->lock);
	}
	if (sock) {
		LOG_D("[usrsctp] closing socket %p", sock);
		usrsctp_close(sock);
		fd_usrsctp_socket_dec();
	}
	if (ctx && ctx->reader_started &&
	    !pthread_equal(pthread_self(), ctx->reader))
		pthread_join(ctx->reader, NULL);

	fd_usrsctp_cbctx_free(ctx);
	conn->cc_usrsctp_cbctx = NULL;
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

static int fd_usrsctp_get_streams(struct cnxctx *conn,
                                   uint16_t *in,
                                   uint16_t *out)
{
	if (!conn || conn->cc_transport.type != FD_CNX_TRANSPORT_USRSCTP ||
	    !conn->cc_transport.endpoint.usrsctp)
		return EINVAL;
	return fd_usrsctp_get_str_info(conn->cc_transport.endpoint.usrsctp, in, out);
}

const struct fd_cnx_transport_ops fd_usrsctp_ops = {
	.create_server = fd_usrsctp_create_server,
	.listen = fd_usrsctp_listen,
	.accept = fd_usrsctp_accept,
	.connect = fd_usrsctp_connect,
	.start_clear = fd_usrsctp_start_clear,
	.recv = fd_usrsctp_recv,
	.send = fd_usrsctp_send,
	.get_streams = fd_usrsctp_get_streams,
	.get_remote_eps = fd_usrsctp_get_remote_eps,
	.close = fd_usrsctp_close
};
