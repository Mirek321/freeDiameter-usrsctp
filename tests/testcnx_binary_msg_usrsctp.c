/*********************************************************************************************************
* SCTP connection-context test for binary payloads containing zero bytes.                                  *
*********************************************************************************************************/

#include "tests.h"

#include <cnxctx.h>

#define TEST_SCTP_PORT 3870
static const unsigned char test_message[] = {
	0x00, 0x01, 0xff, 0x00, 0x7f, 0x80, 0x00, 0x42
};

static struct fd_list endpoints = FD_LIST_INITIALIZER(endpoints);

static void *connect_sctp(void *arg)
{
	(void)arg;
	fd_log_threadname("testcnx_binary_usrsctp:connect");
	return fd_cnx_cli_connect_sctp(1, TEST_SCTP_PORT, &endpoints, NULL);
}

int main(int argc, char *argv[])
{
#ifdef DISABLE_SCTP
	INIT_FD();
	PASSTEST();
#else
	struct addrinfo hints, *addresses, *address;
	struct cnxctx *listener, *server, *client;
	pthread_t thread;
	void *thread_result;
	unsigned char *received;
	size_t received_len;

	(void)argc;
	(void)argv;
	INIT_FD();
	fd_g_config->cnf_flags.no_ip6 = 1;
	fd_g_config->cnf_sctp_str = 4;

	memset(&hints, 0, sizeof(hints));
	hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
	hints.ai_family = AF_INET;
	CHECK(0, getaddrinfo("127.0.0.1", _stringize(TEST_SCTP_PORT), &hints, &addresses));
	for (address = addresses; address; address = address->ai_next)
		CHECK(0, fd_ep_add_merge(&endpoints, address->ai_addr,
					address->ai_addrlen, EP_FL_CONF | EP_ACCEPTALL));
	freeaddrinfo(addresses);

	listener = fd_cnx_serv_sctp(TEST_SCTP_PORT, &endpoints);
	CHECK(1, listener ? 1 : 0);
	CHECK(0, fd_cnx_serv_listen(listener));
	CHECK(0, pthread_create(&thread, NULL, connect_sctp, NULL));
	server = fd_cnx_serv_accept(listener);
	CHECK(1, server ? 1 : 0);
	CHECK(0, pthread_join(thread, &thread_result));
	client = thread_result;
	CHECK(1, client ? 1 : 0);

	CHECK(0, fd_cnx_start_clear(server, 0));
	CHECK(0, fd_cnx_start_clear(client, 0));
	LOG_N("[testcnx_binary_usrsctp] sending %zu bytes including embedded zero bytes",
	      sizeof(test_message));
	CHECK(0, fd_cnx_send(client, (unsigned char *)test_message, sizeof(test_message)));
	CHECK(0, fd_cnx_receive(server, NULL, &received, &received_len));
	CHECK(sizeof(test_message), received_len);
	CHECK(0, memcmp(received, test_message, sizeof(test_message)));
	CHECK(0, fd_cnx_send(server, received, received_len));
	free(received);
	CHECK(0, fd_cnx_receive(client, NULL, &received, &received_len));
	CHECK(sizeof(test_message), received_len);
	CHECK(0, memcmp(received, test_message, sizeof(test_message)));
	free(received);

	fd_cnx_destroy(client);
	fd_cnx_destroy(server);
	fd_cnx_destroy(listener);
	PASSTEST();
#endif
}
