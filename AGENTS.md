# Repository Guidelines

## Project Structure

This is a C implementation of the freeDiameter protocol stack. `libfdproto/` contains message and dictionary utilities; `libfdcore/` implements peer connections and core protocol behavior; `freeDiameterd/` builds the daemon; `extensions/` contains loadable features. Public headers live in `include/freeDiameter/`. `tests/` holds CTest programs, while `doc/` contains configuration examples and deployment notes. The optional vendored SCTP implementation is under `usrsctp/`; `USE_USRSCTP` selects it in place of the kernel SCTP backend. Configuration examples and localhost certificate material are under `doc/`.

## Build, Test, and Development

The repository helper builds and tests either transport backend:

- `./manage.sh build kernel` configures, builds, and runs tests with the kernel SCTP backend.
- `./manage.sh build usrsctp` does the same with bundled userspace SCTP enabled.
- `./manage.sh rebuild all` removes both helper build directories and rebuilds both variants. It does not run tests.
- `./manage.sh test kernel` and `./manage.sh test usrsctp` run tests for an already configured build; an optional third argument selects one CTest name.

For a manual build, use `cmake -S . -B build`, `cmake --build build -j`, then `ctest --test-dir build --output-on-failure`. Set `-DUSE_USRSCTP=ON` to enable the bundled backend. CMake requires Bison and Flex; consult `INSTALL` for other dependencies and installation details.

## Coding Style

Follow nearby C code conventions: use tabs for indentation where the surrounding file does, retain existing naming patterns, and keep public interfaces consistent with headers in `include/freeDiameter/`. CMake configuration is organized in the root and component-level `CMakeLists.txt` files. No repository-wide formatter or linter is configured, so match the touched file and keep changes focused.

## Testing

Tests are registered with CTest in `tests/CMakeLists.txt`; names typically use the `test` prefix. Add or extend a test for behavior changes. Run the relevant suite with `ctest --test-dir build-kernel --output-on-failure` or `ctest --test-dir build-usrsctp --output-on-failure`. SCTP connection behavior has a dedicated `testcnx_sctp` test; exercise it with the userspace backend for transport changes. No repository-wide linter is configured.

## Commits and Pull Requests

Recent commits use concise imperative subjects, often with a scoped Conventional Commit form such as `feat(usrsctp): ...` and an issue reference. Follow that pattern: keep the subject action-oriented and focused. Pull requests should describe the behavior and rationale, list build/test commands run, link related issues, and call out configuration or compatibility impacts. Include logs or configuration snippets when they clarify protocol or transport changes.

## Configuration and Generated Files

Build in separate directories (the helper uses `build-kernel/` and `build-usrsctp/`). Avoid committing generated build output or local configuration containing secrets. The localhost certificate and private key files are checked-in test fixtures; do not treat them as production credentials. Update relevant examples under `doc/` when a configuration option changes.
