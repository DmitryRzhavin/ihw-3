#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-shm}
config=${2:-configs/demo.conf}
unit=${3:-30}
[[ $mode == shm || $mode == mq ]] || { echo 'mode must be shm or mq' >&2; exit 2; }
bin="./${BUILD_DIR:-build}/restaurant_$mode"
session="u${UID}_${mode}_$$_${RANDOM}"
mkdir -p logs
log="logs/${session}.log"
pids=()
server=''
cleanup() {
    trap '' INT TERM
    trap - EXIT

    local p

    if [[ -n $server ]]; then
        kill -TERM "$server" 2>/dev/null || true
    fi

    for p in "${pids[@]}"; do
        kill -TERM "$p" 2>/dev/null || true
    done

    if [[ -n $server ]]; then
        wait "$server" 2>/dev/null || true
    fi

    for p in "${pids[@]}"; do
        wait "$p" 2>/dev/null || true
    done
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
"$bin" --serve --session "$session" --config "$config" --unit-ms "$unit" --log "$log" &
server=$!
ready=0
for ((i=0;i<100;i++)); do
    if [[ -f $log ]] && grep -q 'SESSION ' "$log"; then ready=1; break; fi
    kill -0 "$server" 2>/dev/null || break
    sleep 0.02
done
[[ $ready == 1 ]] || { echo 'Coordinator did not initialize' >&2; exit 1; }
count() { awk -F= -v key="$1" '$1 ~ "^[[:space:]]*"key"[[:space:]]*$" {gsub(/[[:space:]]/,"",$2); v=$2} END {print v}' "$config"; }
for role in visitor waiter cook; do
    case $role in visitor) key=visitors;; waiter) key=waiters;; cook) key=cooks;; esac
    n=$(count "$key")
    [[ $n =~ ^[0-9]+$ ]] || { echo "Missing $key" >&2; exit 2; }
    for ((id=1;id<=n;id++)); do
        "$bin" --session "$session" --role "$role" --id "$id" --log "$log" &
        pids+=("$!")
    done
done
status=0
for p in "${pids[@]}"; do wait "$p" || status=1; done
pids=()
wait "$server" || status=$?
server=''
echo "Log: $log"
exit "$status"
