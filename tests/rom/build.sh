#!/bin/sh
# Assemble the test ROM: tests/rom/build.sh [VASM] -> build/vdptest.bin
set -e
cd "$(dirname "$0")/../.."
DEFAULT=../ninety-four/ref/vasm-src/vasm/vasmm68k_mot                   # (ninety-four beside it,
[ -x "$DEFAULT" ] || DEFAULT=../../ref/vasm-src/vasm/vasmm68k_mot         #  or around it as a submodule)
VASM="${1:-${VASM:-$DEFAULT}}"
mkdir -p build
"$VASM" -quiet -Fbin -m68000 -o build/vdptest.bin tests/rom/vdptest.s
# pad to 256 KB
python3 -c "import sys; d=open('build/vdptest.bin','rb').read(); open('build/vdptest.bin','wb').write(d+b'\xff'*(0x40000-len(d)))"
echo "build/vdptest.bin"
