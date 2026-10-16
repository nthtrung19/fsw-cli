#!/usr/bin/env bash
# End-to-end file transfer: the mcs executable against mcs_cfdp_peer (a CF
# stand-in running the same CFDP engine), over real UDP on loopback.
#
#   file_transfer.sh <mcs> <mcs_cfdp_peer> <source dir> [drop]
#
# For class 1 and class 2: "file put" a ground file, "file get" an FSW file,
# then compare both byte for byte. With [drop] = N the peer drops every Nth
# FileData PDU it receives, so class 2 must recover through NAKs (class 1 is
# then skipped: it cannot recover by design).
set -euo pipefail

MCS=$1
PEER=$2
SRC=$3
DROP=${4:-0}

WORK=$(mktemp -d)
PEER_PID=""
cleanup() {
    [[ -n "$PEER_PID" ]] && kill "$PEER_PID" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

# Private ports, so a running cFS or mcs (1234/5011) is never disturbed.
CMD_PORT=$((20000 + RANDOM % 20000))
TLM_PORT=$((CMD_PORT + 1))

cat > "$WORK/targets.json" <<EOF
{ "targets": { "peer": {
    "endian": "little",
    "catalog": ["$SRC/config/telecommand/cf.json"],
    "format": { "type": "ccsds_v1" },
    "transport": { "type": "udp", "host": "127.0.0.1", "port": $CMD_PORT },
    "telemetry": { "receiver": { "type": "udp", "host": "127.0.0.1", "port": $TLM_PORT } },
    "cfdp": { "downlink_dir": "$WORK/downlink", "pdus_per_second": 500,
              "ack_timeout": 1, "nak_timeout": 1, "ack_limit": 4, "nak_limit": 6, "inactivity_timeout": 10 }
} } }
EOF

mkdir -p "$WORK/fsw/cf/log"
head -c 50000 /dev/urandom > "$WORK/up.bin"
head -c 30000 /dev/urandom > "$WORK/fsw/cf/log/hk.dat"

"$PEER" --port "$CMD_PORT" --tlm-port "$TLM_PORT" --root "$WORK/fsw" --drop "$DROP" --lifetime 120 \
    2> "$WORK/peer.log" &
PEER_PID=$!
sleep 0.3

run() {
    if ! "$MCS" --config "$WORK/targets.json" --no-log -c "$1" > "$WORK/out.txt" 2>&1; then
        echo "FAILED: mcs -c \"$1\""
        cat "$WORK/out.txt"
        echo "--- peer ---"
        cat "$WORK/peer.log"
        exit 1
    fi
    cat "$WORK/out.txt"
}

classes="class1 class2"
[[ "$DROP" != "0" ]] && classes="class2"
for cls in $classes; do
    run "file put $WORK/up.bin /cf/up_$cls.bin $cls"
    cmp "$WORK/up.bin" "$WORK/fsw/cf/up_$cls.bin"
    run "file get /cf/log/hk.dat $WORK/got_$cls.dat $cls"
    cmp "$WORK/fsw/cf/log/hk.dat" "$WORK/got_$cls.dat"
    if [[ "$DROP" != "0" ]] && ! grep -q "NAK" "$WORK/out.txt"; then
        echo "expected NAKs with --drop $DROP"
        exit 1
    fi
done
echo "file transfer end-to-end ok ($classes, drop=$DROP)"
