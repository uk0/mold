#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | $CC -c -o $t/a.o -fPIC -xc -
#include <stdio.h>

void fn1();
void fn2();

__attribute__((section(".low")))  void fn1() { fn2(); }
__attribute__((section(".high"))) void fn2() { fn1(); }

int main() {
  fn1();
}
EOF

$CC -B. -o $t/exe $t/a.o \
  -Wl,--section-start=.low=0x10000000,--section-start=.high=0x20000000

$OBJDUMP -dr $t/exe | grep -E -A7 '<fn1\$thunk[0-9]+>:' > $t/log

grep -E 'bx\s+pc' $t/log
grep -E 'add\s+pc, ip, pc' $t/log
