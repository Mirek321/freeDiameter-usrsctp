# Localhost SCTP example

This example connects two freeDiameter processes on one host using SCTP. The
server listens on
`127.0.0.1:3868`; the client listens on `127.0.0.1:3869` and initiates the
connection. Both load `test_app`. TCP and IPv6 are disabled, and the peer
connection uses `No_TLS`.

Run the commands below from the repository root.

## Build and create a local certificate

Configure the userspace backend and enable `test_app`. The userspace backend
uses raw SCTP sockets, which may require `CAP_NET_RAW`:

```sh
cmake -S . -B build-usrsctp -DUSE_USRSCTP=ON -DBUILD_TEST_APP=ON
cmake --build build-usrsctp -j
```

Grant the daemon binary permission to open raw sockets, or run it as root:

```sh
sudo setcap cap_net_raw+ep ./build-usrsctp/freeDiameterd/freeDiameterd
```

Reapply `setcap` after rebuilding the binary. If `setcap` is unavailable, start
each daemon command below with `sudo` instead.

The configuration parser still requires `TLS_Cred` and `TLS_CA` entries even
though this example disables TLS for the peer connection. Create a self-signed
credential for local use:

```sh
mkdir -p doc/localhost/certs
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
  -keyout doc/localhost/certs/localhost.key \
  -out doc/localhost/certs/localhost.pem \
  -subj "/CN=server.localhost" \
  -addext "subjectAltName=DNS:server.localhost,DNS:client.localhost,IP:127.0.0.1"
```

The generated files are local credentials and are kept out of version control.

## Start the peers

Open two terminals. Start the server first:

```sh
./build-usrsctp/freeDiameterd/freeDiameterd -c doc/localhost/server.conf
```

Then start the client in the other terminal:

```sh
./build-usrsctp/freeDiameterd/freeDiameterd -c doc/localhost/client.conf
```

## Peer connection test

The repository also includes `server-peercnx.conf` and the server-side
`test_peercnx` extension configuration in `server-test_peercnx.conf`. Enable the
extension when configuring the build:

```sh
cmake -S . -B build-usrsctp -DUSE_USRSCTP=ON -DBUILD_TEST_PEERCNX=ON
cmake --build build-usrsctp -j
```

The `test_peercnx` extension can check that a peer reaches the open state and
exchange test requests and answers. Its configuration format is described in
the comments in `extensions/test_peercnx/tpc_conf.y`. The localhost directory
currently provides the server configuration; use a matching client configuration
with `Identity = "client.localhost"`, the client ports above, and the client
mode of `test_peercnx` to run the paired check.
