#!/bin/sh -l
set -e

# setup
cd /github/workspace
yum -y update
yum -y install python3 python3-devel

# env
export PYTHONPATH=$(pwd)/test/python
export CODON_PYTHON=$(python3 test/python/find-python-library.py)
python3 -m pip install numpy

# deps
if [ ! -d ./llvm ]; then
  /bin/bash scripts/deps.sh 2;
fi

# build
mkdir build
export CC="$(pwd)/llvm/bin/clang"
export CXX="$(pwd)/llvm/bin/clang++"
export LLVM_DIR=$(llvm/bin/llvm-config --cmakedir)
(cd build && cmake .. -DCMAKE_BUILD_TYPE=Release \
                      -DCMAKE_C_COMPILER=${CC} \
                      -DCMAKE_CXX_COMPILER=${CXX})
cmake --build build --config Release -- VERBOSE=1

# build cython
export LD_LIBRARY_PATH=$(pwd)/build:$LD_LIBRARY_PATH
python3 -m pip install cython
python3 extra/cython/setup.py build_ext --inplace --force

# test
ln -s build/libcodonrt.so .
build/codon_test
build/codon run test/core/helloworld.codon
build/codon run test/core/exit.codon || if [[ $? -ne 42 ]]; then false; fi
export PYTHONPATH=$(pwd):$PYTHONPATH
python3 test/python/cython_jit.py

# package
export CODON_BUILD_ARCHIVE=codon-$(uname -s | awk '{print tolower($0)}')-$(uname -m).tar.gz
mkdir -p codon-deploy/bin codon-deploy/lib/codon codon-deploy/plugins
cp build/codon codon-deploy/bin/
cp build/libcodon*.so codon-deploy/lib/codon/
cp build/libomp.so codon-deploy/lib/codon/
cp -r build/include codon-deploy/
cp -r stdlib codon-deploy/lib/codon/
tar -czf ${CODON_BUILD_ARCHIVE} codon-deploy
du -sh codon-deploy
