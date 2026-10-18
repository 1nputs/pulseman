#!/bin/sh
# Assemble the test ROM: tests/rom/build.sh [VASM] -> build/vdptest.bin (vasm's 68k assembler:
# the argument, else $VASM, else vasmm68k_mot on the PATH)
set -e
cd "$(dirname "$0")/../.."
VASM="${1:-${VASM:-vasmm68k_mot}}"
mkdir -p build
"$VASM" -quiet -Fbin -m68000 -o build/vdptest.bin tests/rom/vdptest.s
# pad to 256 KB
python3 -c "import sys; d=open('build/vdptest.bin','rb').read(); open('build/vdptest.bin','wb').write(d+b'\xff'*(0x40000-len(d)))"
echo "build/vdptest.bin"
