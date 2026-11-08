#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build_ubsan
modes=(unnamed named shm mq)
for i in 0 1 2 3; do
    gcc -std=c11 -D_GNU_SOURCE -DBACKEND="$i" -Wall -Wextra -Wpedantic -Werror \
        -g -fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
        -Iinclude src/main.c src/model.c src/config.c -pthread -lrt \
        -o "build_ubsan/restaurant_${modes[$i]}"
done
gcc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Werror -g \
    -fsanitize=undefined -fno-sanitize-recover=all -Iinclude tests/model_test.c src/model.c \
    -o build_ubsan/model_test
BUILD_DIR=build_ubsan bash scripts/run_tests.sh
