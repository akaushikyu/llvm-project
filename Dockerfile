# Source - https://stackoverflow.com/a/60752800
# Posted by Mizux, modified by community. See post 'Timeline' for change history
# Retrieved 2026-03-31, License - CC BY-SA 4.0

# Create a virtual environment with all tools installed
# ref: https://hub.docker.com/_/ubuntu

# 1. Use the official Ubuntu base image (latest LTS is recommended)
FROM ubuntu:24.04

# 2. Set environment variables to avoid interactive prompts during installation
ENV DEBIAN_FRONTEND=noninteractive
# Install system build dependencies
# note: here we use the CMake package provided by Ubuntu
# see: https://repology.org/project/cmake/versions
ENV PATH=/usr/local/bin:$PATH
RUN apt-get update -q && \
apt-get install -yq git build-essential cmake llvm clang qemu-user-static binfmt-support wget debootstrap python3 python3.12-venv && \
apt-get clean && \
rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*

CMD [ "/bin/bash" ]

#RUN debootstrap --arch=riscv64 --variant=minbase --include=build-essential,symlinks unstable sysroot-deb-riscv64-unstable
#RUN git clone https://github.com/akaushikyu/llvm-workloads

#WORKDIR /llvm-project

#RUN git checkout ak-lit-test-setup
#RUN cmake -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_PROJECTS="lld;clang" -DLLVM_TARGETS_TO_BUILD=RISCV -DCMAKE_INSTALL_PREFIX=/opt/riscv-llvm -DLLVM_DEFAULT_TARGET_TRIPLE="riscv64-unknown-elf" -DDEFAULT_SYSROOT=/sysroot-deb-riscv64-unstable -S llvm -B riscv-build

#WORKDIR /llvm-project/riscv-build
#RUN make -j8 && make install

## Run LIT tests
#WORKDIR /llvm-project
#RUN /llvm-project/riscv-build/bin/llvm-lit -v /llvm-project/llvm/test/CodeGen/RISCV/check-lr-sc-stats.ll

# Run on one llvm-workload
#WORKDIR /llvm-workloads/tests/bitcodes/
#RUN /opt/riscv-llvm/bin/llvm-ar x libc.bca
#RUN /opt/riscv-llvm/bin/llc -dump-insn-stats-json -march=riscv64 .rand.cpp.o.bc -o llc-output.rand.cpp.o.bc.s

# cat the output file
#RUN cat llc-output.rand.cpp.o.bc.s
