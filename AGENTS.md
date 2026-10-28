# Repository Guidelines

## Project Structure

This is a C implementation of the freeDiameter protocol stack. `libfdproto/` contains message and dictionary utilities; `libfdcore/` implements peer connections and core protocol behavior; `freeDiameterd/` builds the daemon; `extensions/` contains loadable features. Public headers live in `include/freeDiameter/`. `tests/` holds CTest programs, while `doc/` contains configuration examples and deployment notes. The optional vendored SCTP implementation is under `usrsctp/`.

## Build, Test, and Development

The repository helper builds and tests either transport backend:

- `./manage.sh build kernel` configures, builds, and runs tests with the kernel SCTP backend.
- `./manage.sh build usrsctp` does the same with bundled userspace SCTP enabled.
- `./manage.sh rebuild all` removes both helper build directories and rebuilds/tests both variants.

For a manual build, use `cmake -S . -B build`, `cmake --build build -j`, then `ctest --test-dir build --output-on-failure`. Set `-DUSE_USRSCTP=ON` to enable the bundled backend. CMake requires Bison and Flex; consult `INSTALL` for other dependencies and installation details.

## Coding Style

Follow nearby C code conventions: use tabs for indentation where the surrounding file does, retain existing naming patterns, and keep public interfaces consistent with headers in `include/freeDiameter/`. CMake configuration is organized in the root and component-level `CMakeLists.txt` files. No repository-wide formatter or linter is configured, so match the touched file and keep changes focused.

## Testing

Tests are C executables registered with CTest in `tests/CMakeLists.txt`; names typically use the `test` prefix (for example, `testmesg`). Add or extend a test for behavior changes. Run the relevant CTest suite with `ctest --test-dir build --output-on-failure`; SCTP transport changes should also be checked with the `usrsctp` build, which includes `usrsctp_raw_test`.

## Commits and Pull Requests

Recent commits use concise imperative subjects, often with a scoped Conventional Commit form such as `feat(usrsctp): ...` and an issue reference. Follow that pattern: keep the subject action-oriented and focused. Pull requests should describe the behavior and rationale, list build/test commands run, link related issues, and call out configuration or compatibility impacts. Include logs or configuration snippets when they clarify protocol or transport changes.

## Configuration and Generated Files

Build in separate directories (the helper uses `build-kernel/` and `build-usrsctp/`). Avoid committing generated build output, certificates, or local configuration containing secrets. Update relevant examples under `doc/` when a configuration option changes.
