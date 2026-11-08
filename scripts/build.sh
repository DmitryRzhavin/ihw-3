#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if command -v cmake >/dev/null; then
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
    cmake --build build --parallel
else
    echo 'CMake not found; building directly with GCC.'
    mkdir -p build
    modes=(unnamed named shm mq)
    for i in 0 1 2 3; do
        gcc -std=c11 -D_GNU_SOURCE -DBACKEND="$i" -Wall -Wextra -Wpedantic -Werror \
            -g -Iinclude src/main.c src/model.c src/config.c -pthread -lrt \
            -o "build/restaurant_${modes[$i]}"
    done
    gcc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Werror -g -Iinclude \
        tests/model_test.c src/model.c -o build/model_test
fi
