#!/bin/sh

set -eu

build_dir=${WEBSDR_HARNESS_BUILD_DIR:-build-native}
port=${WEBSDR_HARNESS_PORT:-8073}
fixture_port=${WEBSDR_UPDATE_FIXTURE_PORT:-18074}
fixture_root="$build_dir/update-fixture"
log_file=$(mktemp)
server_pid=
fixture_pid=

cleanup()
{
    if [ -n "$fixture_pid" ] && kill -0 "$fixture_pid" 2>/dev/null; then
        kill "$fixture_pid"
        wait "$fixture_pid" 2>/dev/null || true
    fi
    if [ -n "$server_pid" ] && kill -0 "$server_pid" 2>/dev/null; then
        kill "$server_pid"
        wait "$server_pid" 2>/dev/null || true
    fi
    rm -f "$log_file"
}

trap cleanup EXIT INT TERM

cmake -S . -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DNATIVE_HARNESS=ON \
    -DENABLE_HDFL=OFF
cmake --build "$build_dir" --parallel "${BUILD_JOBS:-$(nproc)}"
ctest --test-dir "$build_dir" --output-on-failure

mkdir -p "$fixture_root"
WEBSDR_UPDATE_ROOT="$(cd "$fixture_root" && pwd)" \
WEBSDR_UPDATE_FIXTURE_PORT=$fixture_port \
    node tests/update_fixture_server.js &
fixture_pid=$!

attempt=0
while ! curl --fail --silent --max-time 2 "http://127.0.0.1:$fixture_port/stats" >/dev/null; do
    if ! kill -0 "$fixture_pid" 2>/dev/null; then
        exit 1
    fi
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 30 ]; then
        exit 1
    fi
    sleep 1
done

fixture_root=$(cd "$fixture_root" && pwd)
WEBSDR_HARNESS_PORT=$port \
WEBSDR_UPDATE_API_BASE="http://127.0.0.1:$fixture_port/api" \
WEBSDR_UPDATE_ROOT=$fixture_root \
    "$build_dir/websdr.bin" >"$log_file" 2>&1 &
server_pid=$!

attempt=0
while ! curl --fail --silent --max-time 2 "http://127.0.0.1:$port/status" >/dev/null; do
    if ! kill -0 "$server_pid" 2>/dev/null; then
        cat "$log_file"
        exit 1
    fi
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 30 ]; then
        cat "$log_file"
        exit 1
    fi
    sleep 1
done

if ! WEBSDR_HARNESS_URL="http://127.0.0.1:$port/" \
    WEBSDR_UPDATE_FIXTURE_URL="http://127.0.0.1:$fixture_port/" \
    WEBSDR_UPDATE_ROOT=$fixture_root \
    node tests/native_browser_smoke.js; then
    if kill -0 "$server_pid" 2>/dev/null; then
        echo "native server is still running after browser test failure"
    else
        wait "$server_pid" || echo "native server exited with status $?"
    fi
    tail -200 "$log_file"
    exit 1
fi
