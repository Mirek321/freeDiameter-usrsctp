# freeDiameter

freeDiameter is an implementation of the Diameter protocol, used by network
services for authentication, authorization, and accounting (AAA). It provides
the base protocol and a modular framework that can be extended for specific
Diameter applications and routing behavior.

## Components

- `libfdproto` provides utilities for creating, parsing, and handling Diameter
  messages and dictionaries.
- `libfdcore` implements peer connections and core Diameter behavior, including
  watchdogs, routing, and extension loading.
- `freeDiameterd` is the daemon for running a Diameter peer or agent.
- `extensions/` contains loadable applications and routing features.
- `usrsctp/` contains the optional bundled userspace SCTP implementation.

Applications can also link against the libraries directly; see the public
headers in `include/freeDiameter/`.

## Build

Requirements include CMake, a C compiler, Bison, and Flex. See [INSTALL](INSTALL)
for dependency and platform details.

Build with the system's kernel SCTP transport:

```sh
cmake -S . -B build-kernel
cmake --build build-kernel -j
ctest --test-dir build-kernel --output-on-failure
```

To build with the bundled userspace SCTP transport, enable `USE_USRSCTP`:

```sh
cmake -S . -B build-usrsctp -DUSE_USRSCTP=ON
cmake --build build-usrsctp -j
ctest --test-dir build-usrsctp --output-on-failure
```

The `manage.sh` helper performs the same configure, build, and test steps:

```sh
./manage.sh build kernel
./manage.sh build usrsctp
./manage.sh rebuild all
```

The userspace backend supports SCTP over UDP (RFC 6951). Set
`SCTP_Transport = "UDP"` and, when needed, `SCTP_UDP_Port` in the daemon or
peer configuration. See [doc/freediameter.conf.sample](doc/freediameter.conf.sample)
for configuration options.

## Localhost SCTP example

[`doc/localhost/README.md`](doc/localhost/README.md) describes a two-process
localhost setup using userspace SCTP, including certificate generation,
configuration, packet capture, and a peer-connection check.

## Documentation

- [INSTALL](INSTALL) covers building and platform notes.
- [`doc/`](doc/) contains sample configurations and deployment examples.
- [`usrsctp/README.md`](usrsctp/README.md) documents the bundled SCTP library.
- [LICENSE](LICENSE) contains the software license.

Project information: <https://www.freediameter.net/>.
