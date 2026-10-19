# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

A fork of freeDiameter (Diameter AAA protocol daemon, C, CMake) adding an optional
**userspace SCTP transport (usrsctp)** next to the stock kernel-socket SCTP. Branch names
and commit subjects follow `feat(#<issue> <scope>): ...`.

## Build and test

System deps (from CI): `libsctp-dev libgnutls28-dev libgcrypt-dev libidn2-dev bison flex cmake`.

Two build flavours, each in its own out-of-tree dir (all `build*` dirs are gitignored):

    ./manage.sh build kernel      # -DUSE_USRSCTP=OFF -> build-kernel/
    ./manage.sh build usrsctp     # -DUSE_USRSCTP=ON  -> build-usrsctp/
    ./manage.sh rebuild all       # wipe both dirs, configure, build, ctest
    JOBS=4 ./manage.sh build usrsctp

`manage.sh` = `cmake -S . -B build-<name> -DUSE_USRSCTP=..` + `cmake --build` + `ctest --output-on-failure`.
Default `CMAKE_BUILD_TYPE` is Debug (`-Wall -g -O0`, defines `DEBUG`). Useful cache options
(all in `include/freeDiameter/CMakeLists.txt` / `extensions/CMakeLists.txt`):
`USE_USRSCTP`, `DISABLE_SCTP`, `DEBUG_SCTP`, `ERRORS_ON_WARNINGS`, `ALL_EXTENSIONS`,
`BUILD_<EXTNAME>` (e.g. `BUILD_APP_ACCT`), `BUILD_TESTING`.

Tests (CTest, sources in `tests/`, one executable per `test*.c`):

    ctest --test-dir build-kernel --output-on-failure             # all
    ctest --test-dir build-usrsctp -R testcnx --output-on-failure # one test
    build-usrsctp/tests/testcnx -d                                # run binary directly

Test binaries take `-d` (more verbose, repeatable), `-q`, `-n` (disable the 120 s alarm,
see `TEST_TIMEOUT`), `-f <func>` / `-F <file>` (full trace for one function/file).
Transport-relevant tests: `testcnx` (connections, TLS, SCTP streams), `testsctp`, `testpeers`.
Always run the test suite in **both** flavours when touching `libfdcore/cnxctx.*` or `sctp*.c`.

Run the daemon: `build-kernel/freeDiameterd/freeDiameterd -c <conf>`; `doc/freediameter.conf.sample`
documents every option, `doc/single_host/` has a ready two-peer setup (`make_certs.sh`).

## Architecture

Four components (see README): `libfdproto` (messages, dictionary, sessions, fifo, logging –
no network), `libfdcore` (peers, routing, connections, extension loading), `freeDiameterd`
(thin main), `extensions/*` (`.fdx` plugins built with `FD_ADD_EXTENSION`, registered in
`extensions/CMakeLists.txt` via `FD_EXTENSION_SUBDIR`, entry point via `EXTENSION_ENTRY`
in `include/freeDiameter/extension.h`). Public API: `include/freeDiameter/libfdproto.h`,
`libfdcore.h`; `freeDiameter-host.h` is generated from `.h.in` and carries the
`#cmakedefine`s (`DISABLE_SCTP`, `USE_USRSCTP`, ...).

### Connection layer in libfdcore

- `cnxctx.h` / `cnxctx.c` – `struct cnxctx`, the connection context used by the peer state
  machine (`p_psm.c`, `p_cnx.c`, `p_out.c`, ...) and `server.c` (accept loops). All
  send/recv/TLS/receiver-thread logic lives here; it dispatches on `cc_proto` (TCP/SCTP)
  and `cc_transport.type`.
- `tcp.c`, `sctp.c` – kernel-socket helpers (`fd_sctp_*` take an `int` fd).
- `sctp3436.c` – TLS over multi-stream SCTP (RFC 3436, one TLS session per stream pair).
- `sctp_usrsctp.c` – the userspace backend (only compiled when `USE_USRSCTP`).

### Transport abstraction (`cnxctx.h`)

`struct fd_cnx_transport { type (KERNEL|USRSCTP); endpoint union { int kernel_fd; struct socket *usrsctp; }; const struct fd_cnx_transport_ops *ops; }`
is embedded in `cnxctx` as `cc_transport`. `fd_cnx_transport_ops` is a vtable
(create_server, listen, accept, connect, start_clear, recv, send, get_streams,
get_remote_eps, close); `fd_usrsctp_ops` is the only implementation – the kernel path is
still called directly, not through the vtable.

Rules that follow from this design:
- **Backend selection is compile-time**: with `USE_USRSCTP` every SCTP connection uses
  usrsctp (`fd_cnx_serv_sctp`, `fd_cnx_cli_connect_sctp`, `fd_cnx_serv_accept`); TCP is
  always kernel. There is no runtime switch.
- For usrsctp connections `cc_socket` stays `-1`. Never test `cc_socket > 0` for liveness;
  use `fd_cnx_transport_valid()`, and `fd_cnx_transport_kernel_fd()` (returns -1 for
  usrsctp) before passing an fd to `fd_sctp_*` / `fd_tcp_*`. Close via `fd_cnx_transport_close()`.
- Dispatch points in `cnxctx.c` to keep in sync when adding an op: `fd_cnx_serv_listen`,
  `fd_cnx_serv_accept`, `fd_cnx_getremoteeps`, `fd_cnx_s_recv`, `fd_cnx_s_sendv`,
  `fd_cnx_start_clear`, `fd_cnx_handshake`, `fd_cnx_send`, `fd_cnx_destroy`.

### usrsctp backend design (`sctp_usrsctp.c`)

usrsctp sockets are created without a receive upcall. Each accepted or connected socket
gets a reader thread, started from `start_clear`, that blocks in `usrsctp_recvv()`:
- `fd_usrsctp_reader` loops on `usrsctp_recvv()` and hands each message to
  `fd_usrsctp_handle_message()`, which does reassembly and posts `FDEVP_CNX_*` events or
  queues bytes for the `recv` op (TLS pull). On read error or EOF it marks the connection in error.
- Per-connection state is `struct fd_usrsctp_cbctx` (`cc_usrsctp_cbctx`): mutex, `closing`
  flag and `callback_refs` guard the handover to the reader, and `reader` / `reader_started`
  hold the thread.
- `fd_usrsctp_close` sets `closing`, closes the socket (which wakes the reader), joins the
  reader, then frees the context.
- Stream counts come from the `get_streams` op (backed by `fd_usrsctp_get_str_info`). The
  accept and connect paths in `cnxctx.c` call it and derive `pairs`, as the kernel path does.
- `fd_usrsctp_ensure_init()` calls `usrsctp_init(0, NULL, NULL)` once, before any usrsctp
  socket is created (server and client paths).
- With `conn_output == NULL` usrsctp sends and receives IP packets through raw sockets, which
  need `CAP_NET_RAW`. Without it, associations never complete, so `testcnx`'s SCTP cases hang.
  Run usrsctp-flavour tests only where raw sockets are allowed.

Known gaps: TLS over usrsctp with multiple streams is not implemented – `sctp3436.c` and
`rcvthr_notls_sctp` call `fd_sctp_sendstrv` / `fd_sctp_recvmeta` with the kernel fd; DTLS is
a `TODO` in both backends.

### Vendored `usrsctp/`

`usrsctp/` is recorded in the index as a gitlink (mode 160000) but there is **no
`.gitmodules` and no nested `.git`** – `git submodule` commands fail and the sources are
present only in this working tree (a fresh clone gets an empty dir). The root
`CMakeLists.txt` adds it with `ADD_SUBDIRECTORY(usrsctp)` when `USE_USRSCTP`, forces
`sctp_build_programs=OFF`, sets PIC, and `libfdcore/CMakeLists.txt` adds
`usrsctp/usrsctplib` to the include path of `sctp_usrsctp.c` only. Do not modify files
under `usrsctp/`.

## Conventions

- Tabs, 8-column indent (`.dir-locals.el`). Error handling through the project macros
  (`CHECK_FCT`, `CHECK_FCT_DO`, `CHECK_SYS`, `CHECK_MALLOC_DO`, `CHECK_PARAMS`,
  `CHECK_POSIX`), tracing with `TRACE_ENTRY` / `TRACE_DEBUG` / `LOG_*`; functions return 0
  or an errno value.
- Keep `DISABLE_SCTP` builds compiling: SCTP code goes inside `#ifndef DISABLE_SCTP`, and
  usrsctp-only code inside `#ifdef USE_USRSCTP` (note `fdcore-internal.h` poisons
  `IPPROTO_SCTP` when SCTP is disabled).
- Bison/Flex generate the config parser from `libfdcore/fdd.y` / `fdd.l`.
