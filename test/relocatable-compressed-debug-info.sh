#!/bin/bash
. $(dirname $0)/common.inc

# OneTBB isn't tsan-clean
nm mold | grep '__tsan_init' && skip

test_cflags -g3 -gz || skip

cat <<EOF | $CC -c -o $t/a.o -xc - -g3 -gz
#include <stdio.h>
void hello() { printf("Hello world\n"); }
EOF

cat <<EOF | $CC -c -o $t/b.o -xc - -g3 -gz
void hello();
int main() { hello(); }
EOF

./mold --relocatable -o $t/c.o $t/a.o $t/b.o
$CC -B. -o $t/exe $t/c.o
$QEMU $t/exe | grep 'Hello world'
