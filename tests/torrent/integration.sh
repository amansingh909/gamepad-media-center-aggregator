#!/usr/bin/env bash
# Drives a running torrent server with curl. Start the harness first, e.g.:
#   build-torrent/torrent_serve --temp /tmp/torrent-serve --grace 5 --stall 20
#   tests/torrent/integration.sh --grace 5 --stall 20
# A console debug build (TORRENT_DEBUG_LAN), from this machine:
#   tests/torrent/integration.sh --remote http://<console ip>:11470 --stall 60
set -uo pipefail

BASE=http://127.0.0.1:11470
TEMP=/tmp/torrent-serve
REMOTE=0
GRACE=30
STALL=60
while [ $# -gt 0 ]; do
    case "$1" in
        --remote) REMOTE=1; BASE=$2; shift ;;
        --temp) TEMP=$2; shift ;;
        --grace) GRACE=$2; shift ;;
        --stall) STALL=$2; shift ;;
        *) echo "unknown option $1"; exit 2 ;;
    esac
    shift
done

# Ubuntu 24.04.4 desktop ISO: legal, well seeded, a single 6655619072 byte file
HASH=01c137287d6f0ed05a56742dae794f632c79ff3d
TR=udp%3A%2F%2Ftracker.opentrackr.org%3A1337%2Fannounce
URL="$BASE/$HASH/0?tr=$TR"
SIZE=6655619072
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0
pass() { echo "ok   $1"; }
fail() { echo "FAIL $1"; fails=$((fails + 1)); }

# fetch URL RANGE OUTFILE [max seconds] -> prints the HTTP status
fetch() { curl -s -o "$3" -D "$3.h" -w '%{http_code}' --max-time "${4:-120}" ${2:+-r "$2"} "$1"; }
size() { stat -c %s "$1" 2>/dev/null || echo 0; }

out=$(curl -s -I --max-time 120 "$URL")
if echo "$out" | grep -q "^HTTP/1.1 200" && echo "$out" | grep -qi "^content-length: $SIZE" &&
    echo "$out" | grep -qi "^accept-ranges: bytes"; then pass "HEAD"; else fail "HEAD: $out"; fi

c=$(fetch "$URL" 0-1048575 "$WORK/a")
[ "$c" = 206 ] && [ "$(size "$WORK/a")" = 1048576 ] && pass "first MiB" || fail "first MiB: $c $(size "$WORK/a")"

c=$(fetch "$URL" 0-1048575 "$WORK/b")
[ "$c" = 206 ] && cmp -s "$WORK/a" "$WORK/b" && pass "same bytes twice" || fail "same bytes twice: $c"

c=$(fetch "$URL" 0-99 "$WORK/c")
[ "$c" = 206 ] && cmp -s "$WORK/c" <(head -c 100 "$WORK/a") && pass "prefix matches" || fail "prefix matches: $c"

c=$(fetch "$URL" -4096 "$WORK/tail")
if [ "$c" = 206 ] && [ "$(size "$WORK/tail")" = 4096 ] &&
    grep -qi "^content-range: bytes $((SIZE - 4096))-$((SIZE - 1))/$SIZE" "$WORK/tail.h"; then
    pass "tail of the file"; else fail "tail of the file: $c"; fi

c=$(fetch "$URL" 3000000000-3001048575 "$WORK/mid")
[ "$c" = 206 ] && [ "$(size "$WORK/mid")" = 1048576 ] && pass "seek to the middle" || fail "seek to the middle: $c"

# a player reads the start while it probes the end
fetch "$URL" 8388608-12582911 "$WORK/p1" > "$WORK/p1.code" &
fetch "$URL" 6600000000-6601048575 "$WORK/p2" > "$WORK/p2.code" &
wait
if [ "$(cat "$WORK/p1.code")" = 206 ] && [ "$(size "$WORK/p1")" = 4194304 ] &&
    [ "$(cat "$WORK/p2.code")" = 206 ] && [ "$(size "$WORK/p2")" = 1048576 ]; then
    pass "two readers at once"; else fail "two readers at once: $(cat "$WORK/p1.code") $(cat "$WORK/p2.code")"; fi

# a reader back from a pause longer than the stall timeout, on a new connection, still
# gets data (the torrent was dropped meanwhile and is added again). The engine-level
# pause, same reader, is the harness's --pause check.
sleep $((STALL + 5))
c=$(fetch "$URL" 13000000-14048575 "$WORK/resume")
[ "$c" = 206 ] && [ "$(size "$WORK/resume")" = 1048576 ] && pass "back after a pause, on a new connection" ||
    fail "back after a pause, on a new connection: $c"

# a reader back just after the grace period, while the previous copy is still being
# deleted: what it downloads must still be on disk afterwards. The first read is slowed
# down to outlast the engine's 5 s deletion delay; the second opens the file again.
sleep $((GRACE + 1))
c1=$(curl -s -o "$WORK/again1" -w '%{http_code}' --max-time 120 --limit-rate 150k -r 20000000-21048575 "$URL")
c2=$(fetch "$URL" 20000000-21048575 "$WORK/again2")
if [ "$c1" = 206 ] && [ "$c2" = 206 ] && [ "$(size "$WORK/again2")" = 1048576 ] &&
    cmp -s "$WORK/again1" "$WORK/again2"; then
    pass "back right after the grace period"; else fail "back right after the grace period: $c1 $c2"; fi

c=$(fetch "$URL" 7000000000- "$WORK/x")
[ "$c" = 416 ] && pass "range past the end" || fail "range past the end: $c"
c=$(fetch "$BASE/nothex/0" "" "$WORK/x" 10)
[ "$c" = 400 ] && pass "bad path" || fail "bad path: $c"
c=$(fetch "$BASE/$HASH/7?tr=$TR" "" "$WORK/x")
[ "$c" = 404 ] && pass "missing file index" || fail "missing file index: $c"
c=$(fetch "$BASE/0000000000000000000000000000000000000001/0" "" "$WORK/x" $((STALL + 30)))
[ "$c" = 504 ] && pass "dead torrent" || fail "dead torrent: $c"

if [ "$REMOTE" = 0 ]; then
    # a client that disconnects mid-transfer: its reader goes, then the data
    curl -s -o /dev/null --max-time 300 -r 0- "$URL" &
    sleep 10
    kill $! 2>/dev/null
    wait 2>/dev/null
    sleep $((GRACE + 12))
    left=$(ls -d "$TEMP/$HASH"* 2>/dev/null)
    [ -z "$left" ] && pass "data deleted after the last reader" || fail "data deleted: $left still there"
fi

echo "$fails failed"
exit "$fails"
