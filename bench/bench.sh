#!/usr/bin/env bash

export BENCH_DIR=$(dirname $0)

echo "benchmark,python-time,pypy-time,cpp-time,codon-time"

# SUM
echo -n "sum"
echo -n ","
echo -n $(python3 ${BENCH_DIR}/sum/sum.py | tail -n 1)
echo -n ","
# nothing for pypy
echo -n ","
# nothing for cpp
echo -n ","
echo -n $(build/codon run -release ${BENCH_DIR}/sum/sum.py | tail -n 1)
echo ""

# FLOAT
echo -n "float"
echo -n ","
echo -n $(python3 ${BENCH_DIR}/float/float.py | tail -n 1)
echo -n ","
# nothing for pypy
echo -n ","
# nothing for cpp
echo -n ","
echo -n $(build/codon run -release ${BENCH_DIR}/float/float.py | tail -n 1)
echo ""

# GO
echo -n "go"
echo -n ","
echo -n $(python3 ${BENCH_DIR}/go/go.py | tail -n 1)
echo -n ","
# nothing for pypy
echo -n ","
# nothing for cpp
echo -n ","
echo -n $(build/codon run -release ${BENCH_DIR}/go/go.codon | tail -n 1)
echo ""

# NBODY
echo -n "nbody"
echo -n ","
echo -n $(python3 ${BENCH_DIR}/nbody/nbody.py 1000000 | tail -n 1)
echo -n ","
# nothing for pypy
echo -n ","
# nothing for cpp
echo -n ","
echo -n $(build/codon run -release ${BENCH_DIR}/nbody/nbody.py 1000000 | tail -n 1)
echo ""

# CHAOS
echo -n "chaos"
echo -n ","
echo -n $(python3 ${BENCH_DIR}/chaos/chaos.py /dev/null | tail -n 1)
echo -n ","
# nothing for pypy
echo -n ","
# nothing for cpp
echo -n ","
echo -n $(build/codon run -release ${BENCH_DIR}/chaos/chaos.codon /dev/null | tail -n 1)
echo ""

# SPECTRAL_NORM
echo -n "spectral_norm"
echo -n ","
echo -n $(python3 ${BENCH_DIR}/spectral_norm/spectral_norm.py | tail -n 1)
echo -n ","
# nothing for pypy
echo -n ","
# nothing for cpp
echo -n ","
echo -n $(build/codon run -release ${BENCH_DIR}/spectral_norm/spectral_norm.py | tail -n 1)
echo ""

# SET_PARTITION
echo -n "set_partition"
echo -n ","
echo -n $(python3 ${BENCH_DIR}/set_partition/set_partition.py | tail -n 1)
echo -n ","
# nothing for pypy
echo -n ","
# nothing for cpp
echo -n ","
echo -n $(build/codon run -release ${BENCH_DIR}/set_partition/set_partition.py | tail -n 1)
echo ""

# PRIMES
echo -n "primes"
echo -n ","
echo -n $(python3 ${BENCH_DIR}/primes/primes.py 30000 | tail -n 1)
echo -n ","
# nothing for pypy
echo -n ","
# nothing for cpp
echo -n ","
echo -n $(build/codon run -release ${BENCH_DIR}/primes/primes.py 30000 | tail -n 1)
echo ""

# WORD_COUNT
if [[ ! -z "${DATA_WORD_COUNT}" ]]; then
  echo -n "word_count"
  echo -n ","
  echo -n $(python3 ${BENCH_DIR}/word_count/word_count.py $DATA_WORD_COUNT | tail -n 1)
  echo -n ","
  # nothing for pypy
  echo -n ","
  # nothing for cpp
  echo -n ","
  echo -n $(build/codon run -release ${BENCH_DIR}/word_count/word_count.py $DATA_WORD_COUNT | tail -n 1)
  echo ""
fi

# TAQ
if [[ ! -z "${DATA_TAQ}" ]]; then
  echo -n "taq"
  echo -n ","
  echo -n $(python3 ${BENCH_DIR}/taq/taq.py $DATA_TAQ | tail -n 1)
  echo -n ","
  # nothing for pypy
  echo -n ","
  # nothing for cpp
  echo -n ","
  echo -n $(build/codon run -release ${BENCH_DIR}/taq/taq.py $DATA_TAQ | tail -n 1)
  echo ""
fi
