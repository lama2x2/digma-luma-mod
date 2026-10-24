#!/bin/sh
# Сборка libluma.so под читалку (ARMv7 hard-float, glibc 2.38) через zig.
#   zig: brew install zig, или архив с ziglang.org; путь можно задать в ZIG=
set -e
cd "$(dirname "$0")"
mkdir -p build
"${ZIG:-zig}" cc -target arm-linux-gnueabihf.2.38 -mcpu=cortex_a7 \
	-shared -fPIC -O2 -s -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
	-U_FORTIFY_SOURCE -fno-stack-protector \
	-o build/libluma.so libluma.c -ldl -lm -lpthread
echo "build/libluma.so: $(wc -c < build/libluma.so) байт"
