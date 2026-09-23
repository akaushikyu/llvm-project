# RUN: not llvm-mc -triple=riscv32 %s 2>&1 \
# RUN:        | FileCheck %s --check-prefix=CHECK-ERROR
# RUN: not llvm-mc -triple=riscv64 %s 2>&1 \
# RUN:        | FileCheck %s --check-prefix=CHECK-ERROR
bnerd x31, x28, 2
# CHECK-ERROR: error: invalid operand for instruction
# CHECK-ERROR-LABEL: bnerd x31, x28, 2
bnerd x6, x31, x28, 264
# CHECK-ERROR: error: immediate must be a multiple of 2 bytes in the range [-128, 126]
# CHECK-ERROR-LABEL: bnerd x6, x31, x28, 264
bnerd x6, x31, x28, -130
# CHECK-ERROR: error: immediate must be a multiple of 2 bytes in the range [-128, 126]
# CHECK-ERROR-LABEL: bnerd x6, x31, x28, -130
bned x5, x31, x28, 26
# CHECK-ERROR: error: unrecognized instruction mnemonic, did you mean: bne, bnerd, bnez?
# CHECK-ERROR-LABEL: bned x5, x31, x28, 26

beqrd x31, x28, 2
# CHECK-ERROR: error: invalid operand for instruction
# CHECK-ERROR-LABEL: beqrd x31, x28, 2
beqrd x6, x31, x28, 264
# CHECK-ERROR: error: immediate must be a multiple of 2 bytes in the range [-128, 126]
# CHECK-ERROR-LABEL: beqrd x6, x31, x28, 264
beqrd x6, x31, x28, -130
# CHECK-ERROR: error: immediate must be a multiple of 2 bytes in the range [-128, 126]
# CHECK-ERROR-LABEL: beqrd x6, x31, x28, -130
