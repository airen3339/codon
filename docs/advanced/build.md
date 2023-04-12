Unless you really need to build Codon for whatever reason, we strongly
recommend using pre-built binaries if possible.

# Dependencies

Codon uses an LLVM fork based on LLVM 15. To build it, you can do:

``` bash
git clone --depth 1 -b codon https://github.com/exaloop/llvm-project
cmake -S llvm-project/llvm -B llvm-project/build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_INCLUDE_TESTS=OFF \
    -DLLVM_ENABLE_RTTI=ON \
    -DLLVM_ENABLE_ZLIB=OFF \
    -DLLVM_ENABLE_TERMINFO=OFF \
    -DLLVM_TARGETS_TO_BUILD=all
cmake --build llvm-project/build
cmake --install llvm-project/build --prefix=llvm-project/install
```

You can also add `-DLLVM_ENABLE_PROJECTS=clang` if you do not have clang installed on your system. We also recommend setting a local prefix during the installation to avoid
clashes with the system LLVM.

# Build

The following can generally be used to build Codon. The build process
will automatically download and build several smaller dependencies.

```bash
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_DIR=$(llvm-config --cmakedir) \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++
cmake --build build --config Release
cmake --install build --prefix=install
```

This will produce the `codon` executable in the `install/bin` directory, as
well as `codon_test` in `build` directory which runs the test suite.
Additionally, a number of shared libraries are produced in `install/lib/codon`:

- `libcodonc`: The compiler library used by the `codon` compiler.
- `libcodonrt`: The runtime library used during execution.
- `libomp`: OpenMP runtime used to execute parallel code.

> **Warning**: make sure that `llvm-config` points to the Codon LLVM's
> `llvm-config`! If not sure, use `-DLLVM_DIR=llvm-project/install/lib/cmake/llvm`
> if you followed the instructions above for compiling LLVM.

# Build options

The following additional flags can be passed to CMake:

- *CODON_GPU* = `ON|OFF`: Enable or disable GPU support; requires CUDA (default: `OFF`)

# Jupyter support

For enabling Jupyter support, you need to build Jupyter plugin:
```bash
# Linux version:
cmake -S jupyter -B jupyter/build \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DLLVM_DIR=$(llvm-config --cmakedir) \
    -DCODON_PATH=install \
    -DOPENSSL_ROOT_DIR=$(openssl version -d | cut -d' ' -f2 | tr -d '"') \
    -DOPENSSL_CRYPTO_LIBRARY=/usr/lib64/libssl.so \
    -DXEUS_USE_DYNAMIC_UUID=ON
# n.b. OPENSSL_CRYPTO_LIBRARY might differ on your system.

# On macOS, do this instead:
OPENSSL_ROOT_DIR=/usr/local/opt/openssl cmake -S jupyter -B jupyter/build \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DLLVM_DIR=$(llvm-config --cmakedir) \
    -DCODON_PATH=install

# Then:
cmake --build jupyter/build
cmake --install jupyter/build
```
