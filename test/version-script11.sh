#!/bin/bash
. $(dirname $0)/common.inc

cat <<'EOF' > $t/a.ver
VER_X1 { global: *; local: b*; };
EOF

cat <<EOF | $CXX -fPIC -c -o $t/b.o -xc -
void foo() {}
void bar() {}
void baz() {}
EOF

$CC -B. -shared -Wl,--version-script=$t/a.ver -o $t/c.so $t/b.o

readelf --dyn-syms $t/c.so > $t/log
grep 'foo@@VER_X1' $t/log
not grep ' bar' $t/log
not grep ' baz' $t/log
