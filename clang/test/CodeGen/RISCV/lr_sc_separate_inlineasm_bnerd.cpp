// REQUIRES: riscv-registered-target
//
// LR and SC are in SEPARATE inline-asm statements.
//
// Critical input:
//
// retry_lr:
//
//   INLINE ASM #1:
//     LR
//     BNE -> after_sc       <- immediately after LR
//
//   INLINE ASM #2:
//     SC
//     BNE status,x0 -> LR   <- immediately after SC
//
// Expected:
//
//     LR
//     BNERD
//     SC
//     BNE
//
// RUN: %clangxx \
// RUN:   --target=riscv64-linux-gnu \
// RUN:   --gcc-toolchain=/usr \
// RUN:   --sysroot=/usr/riscv64-linux-gnu \
// RUN:   -march=rv64imafdc \
// RUN:   -mabi=lp64d \
// RUN:   -Xclang -target-feature -Xclang -zaamo \
// RUN:   -Xclang -target-feature -Xclang +bnerd \
// RUN:   -mllvm -riscv-run-bnerd \
// RUN:   -mllvm -verify-machineinstrs \
// RUN:   -mllvm -dump-insn-stats-json \
// RUN:   -O3 -static -pthread %s -lstdc++ -o %t.exe
//
// RUN: test -x %t.exe
//
// RUN: llvm-objdump \
// RUN:   --disassemble-symbols=test7 \
// RUN:   --no-leading-addr \
// RUN:   --no-show-raw-insn \
// RUN:   --no-print-imm-hex \
// RUN:   -M no-aliases \
// RUN:   %t.exe | FileCheck %s

using u64 = unsigned long;

alignas(8) volatile u64 g_word = 1;
volatile u64 g_guard = 0;

#define NOINLINE __attribute__((noinline, used))

static inline u64 *word_ptr() {
  return const_cast<u64 *>(&g_word);
}

extern "C" NOINLINE
u64 test7(u64 *p, u64 value, u64 guard) {
  u64 old;
  u64 status;

retry_lr:
  // INLINE ASM #1: LR followed immediately by the post-LR branch.
  asm goto(
      ".option push\n\t"
      ".option norvc\n\t"
      "lr.d %[old], (%[addr])\n\t"
      "bne %[guard], %[value], %l[after_sc]\n\t"
      ".option pop\n\t"
      : [old] "=&r"(old)
      : [addr] "r"(p),
        [guard] "r"(guard),
        [value] "r"(value)
      : "memory"
      : after_sc);

  // INLINE ASM #2: SC followed immediately by the retry branch.
  //
  // Because INLINE ASM #1 is asm-goto/callbr, this is in its fallthrough MBB,
  // so it is the first INLINEASM in this MBB.
  asm goto(
      ".option push\n\t"
      ".option norvc\n\t"
      "sc.d %[status], %[value], (%[addr])\n\t"
      "bne %[status], zero, %l[retry_lr]\n\t"
      ".option pop\n\t"
      : [status] "=&r"(status)
      : [addr] "r"(p),
        [value] "r"(value)
      : "memory"
      : retry_lr);

after_sc:
  return old ^ value;
}

int main() {
  return static_cast<int>(test7(word_ptr(), 19, g_guard) & 0xff);
}

// CHECK-LABEL: <test7>:
// CHECK:       lr.d {{[^,]+}}, ({{[^)]+}})
// CHECK-NEXT:  bnerd {{.*}}
// CHECK:       sc.d {{[^,]+}}, {{[^,]+}}, ({{[^)]+}})
// CHECK-NEXT:  bne {{[^,]+}}, zero, {{.*}}
// CHECK:       {{c\.jr|jalr}}
