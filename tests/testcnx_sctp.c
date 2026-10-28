/*********************************************************************************************************
* Focused SCTP connection-context test.                                                                    *
*********************************************************************************************************/

#include "tests.h"

#include <cnxctx.h>

#define TEST_SCTP_PORT		3869
#define TEST_MESSAGE		"testcnx_sctp"

static struct fd_list endpoints = FD_LIST_INITIALIZER(endpoints);

static const char *test_sctp_transport_name(void)
{
#ifdef USE_USRSCTP
	return "usrsctp raw SCTP";
#else
	return "kernel SCTP";
#endif
}

static void *connect_sctp(void *arg)
{
	(void)arg;
	fd_log_threadname("testcnx_sctp:connect");
	LOG_N("[testcnx_sctp] client: connecting to SCTP service port %u over %s",
	      TEST_SCTP_PORT, test_sctp_transport_name());
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

	INIT_FD();

	/* Keep this test independent from the default Diameter and SCTP ports. */
	fd_g_config->cnf_flags.no_ip6 = 1;
	fd_g_config->cnf_sctp_str = 4;
	LOG_N("[testcnx_sctp] setup: backend=%s, SCTP service port=%u, streams=%u",
	      test_sctp_transport_name(), TEST_SCTP_PORT, fd_g_config->cnf_sctp_str);

	memset(&hints, 0, sizeof(hints));
	hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
	hints.ai_family = AF_INET;
	CHECK(0, getaddrinfo("127.0.0.1", _stringize(TEST_SCTP_PORT),
				     &hints, &addresses));
	for (address = addresses; address; address = address->ai_next)
		CHECK(0, fd_ep_add_merge(&endpoints, address->ai_addr,
						address->ai_addrlen, EP_FL_CONF | EP_ACCEPTALL));
	freeaddrinfo(addresses);

	LOG_N("[testcnx_sctp] server: creating SCTP listener on 127.0.0.1:%u",
	      TEST_SCTP_PORT);
	listener = fd_cnx_serv_sctp(TEST_SCTP_PORT, &endpoints);
	CHECK(1, listener ? 1 : 0);
	CHECK(0, fd_cnx_serv_listen(listener));
	LOG_N("[testcnx_sctp] server: listener is ready; starting client thread");

	CHECK(0, pthread_create(&thread, NULL, connect_sctp, NULL));
	LOG_N("[testcnx_sctp] server: waiting to accept the SCTP association");
	server = fd_cnx_serv_accept(listener);
	CHECK(1, server ? 1 : 0);
	LOG_N("[testcnx_sctp] server: association accepted");
	CHECK(0, pthread_join(thread, &thread_result));
	client = thread_result;
	CHECK(1, client ? 1 : 0);
	LOG_N("[testcnx_sctp] client: association established");

	LOG_N("[testcnx_sctp] transport: starting clear-text receivers");
	CHECK(0, fd_cnx_start_clear(server, 0));
	CHECK(0, fd_cnx_start_clear(client, 0));
	LOG_N("[testcnx_sctp] client: sending %zu-byte payload", strlen(TEST_MESSAGE));
	CHECK(0, fd_cnx_send(client, (unsigned char *)TEST_MESSAGE,
				 strlen(TEST_MESSAGE)));
	CHECK(0, fd_cnx_receive(server, NULL, &received, &received_len));
	CHECK(strlen(TEST_MESSAGE), received_len);
	CHECK(0, memcmp(received, TEST_MESSAGE, received_len));
	LOG_N("[testcnx_sctp] server: received %zu-byte payload; content verified", received_len);
	free(received);

	LOG_N("[testcnx_sctp] shutdown: closing client, accepted connection, and listener");
	fd_cnx_destroy(client);
	fd_cnx_destroy(server);
	fd_cnx_destroy(listener);
	PASSTEST();
#endif
}
