#!/usr/bin/env bash
set -uo pipefail

SRCDIR="$(cd "$(dirname "$0")" && pwd)"
SERVER_BIN="${1:-$SRCDIR/../live-server}"
PORT="${2:-9999}"
ROOT="$SRCDIR/test_data"
PASS=0
FAIL=0

if [ ! -x "$SERVER_BIN" ]; then
	echo "FAIL: Server binary not found: $SERVER_BIN"
	exit 1
fi

cleanup() {
	local pids
	pids=$(cat /tmp/live_server_test.pid 2>/dev/null) || true
	for pid in $pids; do
		kill "$pid" 2>/dev/null || true
	done
	# Give servers a moment to exit, then force-kill
	sleep 0.3 2>/dev/null || true
	for pid in $pids; do
		kill -9 "$pid" 2>/dev/null || true
	done
}
trap cleanup EXIT

"$SERVER_BIN" -I "$ROOT" -P "$PORT" -H 127.0.0.1 -L error -t 2 -k 5 &
echo $! > /tmp/live_server_test.pid
SERVER_PID=$!

for i in 1 2 3 4 5; do
	sleep 0.3
	if kill -0 "$SERVER_PID" 2>/dev/null && curl -s -o /dev/null "http://127.0.0.1:$PORT/" 2>/dev/null; then
		break
	fi
	if [ "$i" -eq 5 ]; then
		echo "FAIL: Server failed to start"
		exit 1
	fi
done

BASE="http://127.0.0.1:$PORT"

ok()   { PASS=$((PASS+1)); echo "  PASS: $*"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL: $*"; }

assert_status() {
	local expected="$1" desc="$2"
	shift 2
	local actual
	actual=$(curl -s -o /dev/null -w '%{http_code}' "$@")
	[ "$actual" = "$expected" ] && ok "$desc (status=$actual)" || fail "$desc (expected=$expected, got=$actual)"
}

assert_body() {
	local expected="$1" desc="$2"
	shift 2
	local actual
	actual=$(curl -s "$@")
	case "$actual" in
		*"$expected"*) ok "$desc" ;;
		*) fail "$desc (expected body containing '$expected')" ;;
	esac
}

assert_not_body() {
	local expected="$1" desc="$2"
	shift 2
	local actual
	actual=$(curl -s "$@")
	case "$actual" in
		*"$expected"*) fail "$desc (unexpected body containing '$expected')" ;;
		*) ok "$desc" ;;
	esac
}

echo ""
echo "=== Basic tests ==="
assert_status 200 "GET / (index.html)" "$BASE/"
assert_body    "Hello" "GET / returns HTML" "$BASE/"
assert_status 404 "GET /nonexistent" "$BASE/nonexistent"
assert_status 405 "POST / (method not allowed)" -X POST "$BASE/"
assert_status 301 "GET /subdir (redirect with trailing /)" "$BASE/subdir"
assert_body    "sub" "Follow redirect to /subdir/" -L "$BASE/subdir/"

echo ""
echo "=== Conditional requests ==="
ETAG=$(curl -sI "$BASE/" | grep -i etag | sed 's/.*: *//' | tr -d '\r')
[ -n "$ETAG" ] && ok "ETag header present" || fail "ETag header missing"
assert_status 304 "If-None-Match with valid ETag" -H "If-None-Match: $ETAG" "$BASE/"
assert_status 200 "If-None-Match with bad ETag" -H 'If-None-Match: "bad-etag"' "$BASE/"

echo ""
echo "=== Range requests ==="
assert_status 206 "Range request (first 5 bytes)" -H "Range: bytes=0-4" "$BASE/"
assert_body    "<!DOC" "Range body content" -H "Range: bytes=0-4" "$BASE/"
assert_status 416 "Unsatisfiable range" -H "Range: bytes=99999-99999" "$BASE/"

echo ""
echo "=== Keep-Alive ==="
# Check that responses include Connection: keep-alive when client supports it
KA=$(curl -sI -H "Connection: keep-alive" "$BASE/" | grep -i "^Connection:" | tr -d '\r' || true)
[ -n "$KA" ] && ok "Keep-alive: response header present ($KA)" || fail "Keep-alive: response header missing"
# Without keep-alive, Connection: close is the default
CL=$(curl -sI "$BASE/" | grep -i "^Connection:" | tr -d '\r' || true)
[ -n "$CL" ] && ok "Keep-alive: Connection header present ($CL)" || fail "Keep-alive: Connection header missing"
# Explicit Connection: close should return close
CL2=$(curl -sI -H "Connection: close" "$BASE/" | grep -i "^Connection:" | tr -d '\r' | grep -i close || true)
[ -n "$CL2" ] && ok "Keep-alive: close when requested ($CL2)" || fail "Keep-alive: close when requested"

echo ""
echo "=== Authentication ==="
AUTH_PORT=$((PORT + 1))
"$SERVER_BIN" -I "$ROOT" -P "$AUTH_PORT" -H 127.0.0.1 -u admin -p secret -L error -t 2 &
echo $! >> /tmp/live_server_test.pid
sleep 0.5
AUTH_BASE="http://127.0.0.1:$AUTH_PORT"
assert_status 401 "Auth: missing creds" "$AUTH_BASE/"
assert_status 200 "Auth: correct creds" -u admin:secret "$AUTH_BASE/"
assert_body "Hello" "Auth: body content" -u admin:secret "$AUTH_BASE/"

echo ""
echo "=== Security ==="
assert_status 400 "Path traversal blocked" --path-as-is "$BASE/../etc/passwd"
assert_not_body "root:" "Path traversal: no system file leaked" --path-as-is "$BASE/../etc/passwd"
assert_status 400 "Double-dot path blocked" --path-as-is "$BASE/.."

echo ""
echo "=== CLI flags ==="
"$SERVER_BIN" --help > /dev/null 2>&1 && ok "--help works" || fail "--help failed"
"$SERVER_BIN" --version > /dev/null 2>&1 && ok "--version works" || fail "--version failed"

echo ""
echo "=============================="
echo "Results: $PASS passed, $FAIL failed"
echo "=============================="
[ "$FAIL" -eq 0 ]
