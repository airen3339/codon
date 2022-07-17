#!/bin/bash -l

export arg=$1
export testdir=$(dirname $0)
export codon="$arg/codon"

# argv test
[ "$($codon run "$dir/argv.codon" aa bb cc)" == "aa,bb,cc" ] || exit 1

# build test
$codon build -release -o "$arg/test_binary" "$testdir/build.codon"
[ "$($arg/test_binary)" == "hello" ] || exit 1

# library test
$codon build -release -o "$arg/libcodon_export_test.so" "$testdir/export.codon"
gcc "$testdir/test.c" -L"$arg" -lcodon_export_test -o "$arg/test_binary"
[ "$($arg/test_binary)" == "abcabcabc" ] || exit 1

# exit code test
$codon run "$testdir/exit.codon" || if [[ $? -ne 42 ]]; then exit 1; fi
