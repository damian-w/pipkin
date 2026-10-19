#!/bin/sh
# Hardware-free checks: core logic, renderer and board helpers for both boards, and release
# packaging.
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
check core src/model.cpp src/protocol.cpp src/demo.cpp tests/core_test.cpp
check render src/model.cpp src/render.cpp tests/render_check.cpp
check hardware -Imain tests/hardware_check.cpp
# The 4-inch ES3C40P build: the same layout drawn 4/3 larger on a 480 x 320 panel.
large="-DPIPKIN_BOARD_ES3C40P=1 -DPIPKIN_DISPLAY_480X320=1"
check render-480x320 $large src/model.cpp src/render.cpp tests/render_check.cpp
check hardware-es3c40p $large -Imain tests/hardware_check.cpp
python3 -B -m unittest discover -s tests -p '*_test.py'
