#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=${BUILD_DIR:-build}
tmp=$(mktemp -d)
server=''
cleanup() {
    if [[ -n $server ]]; then
        kill -TERM "$server" 2>/dev/null || true
        wait "$server" 2>/dev/null || true
    fi
    rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
fail() { echo "FAIL: $*" >&2; exit 1; }
resources() {
    local dir
    for dir in /dev/shm /dev/mqueue; do
        if [[ -d $dir ]]; then
            find "$dir" -maxdepth 1 -type f \( -name 'r32_*' -o -name 'sem.r32_*' \) -print
        fi
    done | sort
}
resources > "$tmp/before"
check_resources() {
    resources > "$tmp/after"
    if ! cmp -s "$tmp/before" "$tmp/after"; then
        diff -u "$tmp/before" "$tmp/after" >&2 || true
        fail 'IPC objects changed after termination'
    fi
}
field() {
    awk -v key="$2" '/] SUMMARY / {for(i=1;i<=NF;i++) {split($i,p,"=");if(p[1]==key) v=p[2]}} END{print v}' "$1"
}
make_command() {
    local mode=$1 config=${2:-demo} unit=${3:-1}
    if [[ $mode == shm || $mode == mq ]]; then
        cmd=(bash scripts/run_independent.sh "$mode" "configs/$config.conf" "$unit")
    else
        cmd=("./$build/restaurant_$mode" --config "configs/$config.conf" --unit-ms "$unit" --no-log)
    fi
}
passed=0
pass() { passed=$((passed+1)); echo "PASS $*"; }
"./$build/model_test" > "$tmp/model.log" 2>&1 || { cat "$tmp/model.log"; fail model; }
echo 'PASS deterministic model checks'
export BUILD_DIR="$build"
for mode in unnamed named shm mq; do
    for config in demo limited_queues ready_full lifo random_orders impossible; do
        make_command "$mode" "$config"
        if ! timeout --kill-after=3 15 "${cmd[@]}" > "$tmp/run.log" 2>&1; then
            cat "$tmp/run.log"; fail "$mode/$config"
        fi
        bash scripts/check_log.sh "$tmp/run.log"
        delivered=4; canceled=0; operations=16
        [[ $config != impossible ]] || { delivered=2; canceled=2; operations=8; }
        [[ $config != random_orders ]] || operations=24
        [[ $(field "$tmp/run.log" termination) == normal ]] || fail 'not normal'
        [[ $(field "$tmp/run.log" delivered) == "$delivered" ]] || fail delivered
        [[ $(field "$tmp/run.log" canceled) == "$canceled" ]] || fail canceled
        [[ $(field "$tmp/run.log" completed) == "$operations" ]] || fail operations
        if [[ $config == limited_queues ]]; then
            [[ $(field "$tmp/run.log" max_kitchen) -le 1 && $(field "$tmp/run.log" max_ready) -le 1 ]] || fail capacity
        fi
        check_resources
        pass "$mode/$config"
    done
    for sig in INT TERM; do
        make_command "$mode" demo 300
        setsid timeout --foreground --kill-after=3 10 "${cmd[@]}" > "$tmp/signal.log" 2>&1 &
        server=$!
        sleep 0.25
        kill -"$sig" -- "-$server" || fail 'process group missing'
        status=0
        wait "$server" || status=$?
        server=''
        expected=130
        [[ $sig != TERM ]] || expected=143
        if [[ $status != "$expected" ]]; then cat "$tmp/signal.log"; fail "$mode/SIG$sig status=$status"; fi
        grep -q 'SUMMARY termination=interrupted' "$tmp/signal.log" || { cat "$tmp/signal.log"; fail 'missing interrupted summary'; }
        check_resources
        pass "$mode/SIG$sig"
    done
    make_command "$mode"
    timeout --kill-after=3 15 "${cmd[@]}" > "$tmp/restart.log" 2>&1 || fail restart
    bash scripts/check_log.sh "$tmp/restart.log"
    check_resources
    pass "$mode/restart"
done
for mode in unnamed named shm mq; do
    make_command "$mode" stress
    timeout --kill-after=3 20 "${cmd[@]}" > "$tmp/stress.log" 2>&1 || { cat "$tmp/stress.log"; fail stress; }
    bash scripts/check_log.sh "$tmp/stress.log"
    [[ $(field "$tmp/stress.log" delivered) == 24 && $(field "$tmp/stress.log" completed) == 288 ]] || fail 'stress counters'
    [[ $(field "$tmp/stress.log" max_kitchen) -le 1 && $(field "$tmp/stress.log" max_ready) -le 1 ]] || fail 'stress capacity'
    check_resources
    pass "$mode/stress"
done
for mode in unnamed named; do
    status=0
    "./$build/restaurant_$mode" --config tests/invalid.conf --no-log > "$tmp/invalid.log" 2>&1 || status=$?
    [[ $status == 2 ]] || fail 'invalid configuration not rejected'
    check_resources
    pass "$mode/invalid"
done
for mode in shm mq; do
    session="test_duplicate_$$_${mode}"
    bin="./$build/restaurant_$mode"
    "$bin" --serve --session "$session" --no-log > "$tmp/owner.log" 2>&1 &
    server=$!
    ready=0
    for ((i=0;i<100;i++)); do
        if grep -q 'SESSION ' "$tmp/owner.log"; then ready=1; break; fi
        sleep 0.02
    done
    [[ $ready == 1 ]] || fail 'owner initialization'
    status=0
    "$bin" --serve --session "$session" --no-log > "$tmp/duplicate.log" 2>&1 || status=$?
    [[ $status != 0 ]] || fail 'second owner accepted'
    resources > "$tmp/active"
    cmp -s "$tmp/before" "$tmp/active" && fail 'second owner removed live objects'
    kill -TERM "$server"
    wait "$server" || true
    server=''
    check_resources
    pass "$mode/exclusive-owner"
done
echo "All $passed tests passed."
if command -v ctest >/dev/null && [[ -f $build/CTestTestfile.cmake ]]; then
    ctest --test-dir "$build" --output-on-failure
fi
