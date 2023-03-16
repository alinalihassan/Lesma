#!/bin/sh

# Clone LLVM
echo "[Step 1] Cloning LLVM (Could take a while) ... "
git clone --depth 1 --branch llvmorg-14.0.6 https://github.com/llvm/llvm-project llvm
echo "done."

# Build LLVM
echo "[Step 2] Building LLVM (Could take a whole while, please be patient) ... "
mkdir ./llvm/build-release 2>/dev/null
(
  cd ./llvm/build-release || exit
  cmake -DCMAKE_BUILD_TYPE=Release -DLLVM_INCLUDE_EXAMPLES=Off -DLLVM_INCLUDE_TESTS=Off -DLLVM_ENABLE_ASSERTIONS=On -DLLVM_ENABLE_PROJECTS=clang -DCMAKE_CXX_FLAGS_RELEASE="-O3" -GNinja ../llvm
  cmake --build .
)
echo "done."