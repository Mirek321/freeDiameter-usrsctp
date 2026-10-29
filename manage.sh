#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JOBS="${JOBS:-$(nproc)}"

build_one() {
    local name="$1"
    local use_usrsctp="$2"
    local build_dir="${ROOT_DIR}/build-${name}"

    cmake -S "$ROOT_DIR" -B "$build_dir" \
        -DUSE_USRSCTP="$use_usrsctp"

    cmake --build "$build_dir" -j"$JOBS"
}

test_one() {
    local name="$1"
    local use_usrsctp="$2"
    local test_name="${3:-}"
    local build_dir="${ROOT_DIR}/build-${name}"

    if [ -n "$test_name" ]; then
        ctest --test-dir "$build_dir" --output-on-failure -R "^${test_name}$"
    else
        ctest --test-dir "$build_dir" --output-on-failure
    fi
}

clean_one() {
    local name="$1"
    rm -rf "${ROOT_DIR}/build-${name}"
}

run_one() {
    local action="$1"
    local name="$2"

    case "$name" in
        kernel)
            use_usrsctp=OFF
            ;;
        usrsctp)
            use_usrsctp=ON
            ;;
        *)
            echo "Unknown build type: $name"
            exit 1
            ;;
    esac

    if [ "$action" = "rebuild" ]; then
        clean_one "$name"
    fi

    build_one "$name" "$use_usrsctp"
}

action="${1:-build}"
name="${2:-kernel}"
test_name="${3:-}"

case "$action:$name" in
    build:kernel|rebuild:kernel)
        run_one "$action" kernel
        ;;
    build:usrsctp|rebuild:usrsctp)
        run_one "$action" usrsctp
        ;;
    rebuild:all)
        clean_one kernel
        clean_one usrsctp
        build_one kernel OFF
        build_one usrsctp ON
        ;;
    test:kernel)
        test_one kernel OFF "$test_name"
        ;;
    test:usrsctp)
        test_one usrsctp ON "$test_name"
        ;;
    *)
        echo "Usage:"
        echo "  $0 build kernel"
        echo "  $0 rebuild kernel"
        echo "  $0 build usrsctp"
        echo "  $0 rebuild usrsctp"
        echo "  $0 rebuild all"
        echo "  $0 test kernel [test_name]"
        echo "  $0 test usrsctp [test_name]"
        exit 1
        ;;
esac
