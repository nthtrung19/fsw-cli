#!/usr/bin/env bash
# End-to-end script checks: the mcs executable runs a test script against
# mcs_fake_fsw (a DS stand-in that answers commands with housekeeping and
# events), over real UDP on loopback.
#
#   verify.sh <mcs> <mcs_fake_fsw> <source dir>
#
# 1. A passing script: exit 0, every test PASS.
# 2. The same script expecting a wrong counter value: exit 1, the FAIL line
#    carries its location, the test FAILs, the other test still passes.
# 3. --check on a script with a typo: exit 1, nothing sent.
set -euo pipefail

MCS=$1
FAKE=$2
SRC=$3

WORK=$(mktemp -d)
FAKE_PID=""
cleanup() {
    [[ -n "$FAKE_PID" ]] && kill "$FAKE_PID" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

# Private ports, so a running cFS or mcs (1234/5011) is never disturbed.
CMD_PORT=$((20000 + RANDOM % 20000))
TLM_PORT=$((CMD_PORT + 1))

cat > "$WORK/targets.json" <<EOF
{ "targets": { "fake": {
    "endian": "little",
    "catalog": ["$SRC/config/telecommand/ds.json"],
    "format": { "type": "ccsds_v1" },
    "transport": { "type": "udp", "host": "127.0.0.1", "port": $CMD_PORT },
    "telemetry": { "receiver": { "type": "udp", "host": "127.0.0.1", "port": $TLM_PORT },
                   "catalog": ["$SRC/config/telemetry/ds.json", "$SRC/config/telemetry/evs.json"] }
} } }
EOF

write_script() {   # $1 = file, $2 = expected counter increase in DS-001
    cat > "$1" <<EOF
# Script checks against mcs_fake_fsw
timeout 3

test DS-001 "NOOP increments the command counter" req DS-R1
  save n ds.hk cmd_accepted_counter
  fsw ds noop
  expect-event DS id 31 type INFO
  expect ds.hk cmd_accepted_counter == \$n+$2
  expect-no-event DS type ERROR for 0.5
end

test DS-002 "An invalid command code is rejected" req DS-R2
  save r ds.hk cmd_rejected_counter
  raw 19 4B C0 00 00 01 00 63
  expect-event DS id 21 type ERROR text "invalid command code"
  expect ds.hk cmd_rejected_counter == \$r+1
end
EOF
}

"$FAKE" --config-dir "$SRC/config" --port "$CMD_PORT" --tlm-port "$TLM_PORT" --lifetime 60 2> "$WORK/fake.log" &
FAKE_PID=$!
sleep 0.3

mcs() {
    "$MCS" --config "$WORK/targets.json" --no-log "$@"
}

fail() {
    echo "FAILED: $1"
    echo "--- output ---"
    cat "$WORK/out.txt"
    echo "--- fake_fsw ---"
    cat "$WORK/fake.log"
    exit 1
}

expect_output() {   # $1 = fixed text that must appear
    grep -qF -- "$1" "$WORK/out.txt" || fail "output lacks: $1"
}

# 1. passing script
write_script "$WORK/pass.mcs" 1
status=0
mcs -f "$WORK/pass.mcs" > "$WORK/out.txt" 2>&1 || status=$?
cat "$WORK/out.txt"
[[ $status -eq 0 ]] || fail "passing script: exit $status"
expect_output "=== DS-001 PASS  (3 check(s))"
expect_output "=== DS-002 PASS  (2 check(s))"
expect_output "result: PASS  tests: 2 passed, 0 failed; checks: 5 passed, 0 failed; commands sent: 2; errors: 0"

# 2. a wrong expected value
write_script "$WORK/fail.mcs" 2
status=0
mcs -f "$WORK/fail.mcs" > "$WORK/out.txt" 2>&1 || status=$?
cat "$WORK/out.txt"
[[ $status -eq 1 ]] || fail "failing script: exit $status, expected 1"
expect_output "$WORK/fail.mcs:8: FAIL expect ds.hk cmd_accepted_counter == "
expect_output "=== DS-001 FAIL  (1 of 3 check(s) failed)"
expect_output "=== DS-002 PASS"
expect_output "result: FAIL  tests: 1 passed, 1 failed; checks: 4 passed, 1 failed"

# 3. --check finds a typo; nothing is sent
sed 's/cmd_accepted_counter ==/cmd_acepted_counter ==/' "$WORK/pass.mcs" > "$WORK/typo.mcs"
status=0
mcs --check -f "$WORK/typo.mcs" > "$WORK/out.txt" 2>&1 || status=$?
cat "$WORK/out.txt"
[[ $status -eq 1 ]] || fail "--check with a typo: exit $status, expected 1"
expect_output "$WORK/typo.mcs:8: error: expect: ds.hk has no field 'cmd_acepted_counter' (did you mean cmd_accepted_counter?)"
expect_output "not run: 1 error(s)"
grep -q "sent fsw" "$WORK/out.txt" && fail "--check sent a command"

echo "script checks end-to-end ok"
