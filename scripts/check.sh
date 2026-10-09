#!/bin/sh
# Hardware-free checks: core logic, renderer, board helpers and release packaging.
set -eu
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
flags="-std=c++17 -Wall -Wextra -Werror -Iinclude"
check() {
    name=$1
    shift
    c++ $flags "$@" -o "$out/$name"
    "$out/$name"
}
check core src/model.cpp src/protocol.cpp tests/core_test.cpp
check render src/model.cpp src/render.cpp tests/render_check.cpp
check hardware -Imain tests/hardware_check.cpp
python3 -B -m unittest discover -s tests -p '*_test.py'
