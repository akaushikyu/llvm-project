# README for custom fork

## Installing RISC-V LLVM

- Ensure to have the latest CMake (CMake >= 4.2.1)

- For building LLVM for RISC-V target, use the following cmake command:

```
cmake \
    -DCMAKE_BUILD_TYPE=Debug \ 
    -DLLVM_ENABLE_PROJECTS="lld;clang" \
    -DLLVM_TARGETS_TO_BUILD=RISCV \
    -DCMAKE_INSTALL_PREFIX=/opt/riscv-llvm \
    -DLLVM_DEFAULT_TARGET_TRIPLE="riscv64-unknown-elf" \
    -DDEFAULT_SYSROOT=/home/kaushika/REPOS/riscv-support/sysroot-deb-riscv64-unstable/ \ 
    -S llvm -B riscv-build/
```

- Instructions for building the RISC-V sysroot can be found [here](https://llvm.org/docs/HowToCrossCompileLLVM.html)

- After `cmake` command, run `sudo make -j8`. This will install the LLVM utilities in `/opt/riscv-llvm`.

## Testing

- A simple `helloWorld.c` program with calls to `pthread` and `stdio` as below
```
#include <stdio.h>
#include <pthread.h>

void* print_hello(void* data) {
    printf("Hello world\n");
    pthread_exit(NULL);
}

int main() {
    pthread_t thread;
    // Create the thread, passing NULL for default attributes and arguments
    pthread_create(&thread, NULL, print_hello, NULL);
    // Wait for the thread to finish
    pthread_join(thread, NULL);
    return 0;
}
```
- Compile with `/opt/riscv-llvm/bin/clang --target=riscv64-unknown-linux -static -march=rv64gc -o hello.o hello.c -I/usr/include/riscv64-linux-gnu/ -L/usr/lib/riscv64-linux-gnu/ -lc -lm -lpthread`

- Confirm the RV64 binary with `/opt/riscv-llvm/bin/llvm-objdump -S hello.o`

## Installing libc + Compiler RT

- From this [GitHub issue](https://github.com/llvm/llvm-project/issues/126229), the following `cmake` command followed by `ninja` works. 

```
cmake ../runtimes/ -G Ninja \
    -DCMAKE_INSTALL_PREFIX=/opt/riscv-llvm/ \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_C_FLAGS="--target=riscv64-unknown-elf -march=rv64gc --sysroot=/opt/riscv/riscv64-unknown-elf/ --gcc-toolchain=/opt/riscv" \
    -DCMAKE_CXX_FLAGS="--target=riscv64-unknown-elf -march=rv64gc --sysroot=/opt/riscv/riscv64-unknown-elf/ --gcc-toolchain=/opt/riscv" \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DLLVM_ENABLE_RUNTIMES=libc \
    -DLLVM_LIBC_FULL_BUILD=ON \
    -DLIBC_TARGET_TRIPLE=riscv64-unknown-elf \
    -DCMAKE_BUILD_TYPE=Release
```

- Note that this will *not* use the clang compiler built in the previous step; rather this will use clang in `/usr/bin`. 
- If you want to compile against the built `clang` in the previous step, then append the above `cmake` command with `PATH=/opt/riscv-llvm/bin:$PATH cmake `
- To generate the LLVM bitcode of libc using `wllvm`, use the following `cmake` command: 
```
cmake  -S runtimes -B build-libc -G Ninja \
    -DCMAKE_C_COMPILER=/opt/riscv-llvm/bin/clang \
    -DCMAKE_CXX_COMPILER=/opt/riscv-llvm/bin/clang++ \
    -DCMAKE_C_FLAGS="--target=riscv64-linux-gnu -march=rv64gc -mabi=lp64d --sysroot=/home/kaushika/sysroot-deb-riscv64-unstable/ -L/usr/lib/riscv64-linux-gnu/ -Qunused-arguments"\ 
    -DCMAKE_CXX_FLAGS="--target=riscv64-linux-gnu -march=rv64gc -mabi=lp64d --sysroot=/home/kaushika/sysroot-deb-riscv64-unstable/ -L/usr/lib/riscv64-linux-gnu/ -Qunused-arguments" \
    -DCMAKE_ASM_FLAGS="--target=riscv64-linux-gnu -march=rv64gc -mabi=lp64d --sysroot=/home/kaushika/sysroot-deb-riscv64-unstable/ -L/usr/lib/riscv64-linux-gnu/ -Qunused-arguments" \
    -DLLVM_ENABLE_RUNTIMES="libc;compiler-rt" \
    -DLLVM_LIBC_FULL_BUILD=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_LIBC_INCLUDE_SCUDO=ON \
    -DCOMPILER_RT_BUILD_SCUDO_STANDALONE_WITH_LLVM_LIBC=ON \
    -DCOMPILER_RT_BUILD_GWP_ASAN=OFF \
    -DCOMPILER_RT_SCUDO_STANDALONE_BUILD_SHARED=OFF \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DLLVM_ENABLE_SPHINX=ON \
    -DLIBC_INCLUDE_DOCS=ON \
    -DLIBC_CMAKE_VERBOSE_LOGGING=ON \
    -DLIBC_TARGET_TRIPLE=riscv64-unknown-linux-gnu \
    -DLLVM_DEFAULT_TARGET_TRIPLE=riscv64-unknown-linux-gnu \
    -DCOMPILER_RT_DEFAULT_TARGET_TRIPLE=riscv64-unknown-linux-gnu
```

## LIT tests and Github CI
- Each push triggers a Github CI action that builds and verifies the build against the RISC-V LIT tests and bitcodes from the LLVM-bitcodes repository
- The CI runs the build and verification in a docker container. For more details, refer the code changes in PR #7
- Merging a pull request to main branch is permitted only when the CI returns success. If the CI fails, look at the logs and determine where the CI failed. 
- Each pull request must have LIT tests as part of the commit. The LIT tests must be short and extensive. An example of LIT test can be found in `llvm/test/CodeGen/RISCV/check-lr-sc-stats.ll` 

## Things to keep in mind

- A lot of time was spent in preparing `/usr/lib/riscv64-linux-gnu`. This directory must have the necessary C runtime startup files (`crt*.o`, `libgcc` etc.). One way is to create symlinks with the correct files in the sysroot directory. 
- Similarly, create a `/usr/include/riscv64-linux-gnu` and copy over the files from directory `sysroot-deb-riscv64-unstable/usr/include/riscv64-linux-gnu/` in `/usr/include/riscv64-linux-gnu`
- CMake flags such a `-DCMAKE_ASM_FLAGS`, `-DCMAKE_C_FLAGS`, and `-DCMAKE_CXX_FLAGS` are critical for ensuring correct compilation
