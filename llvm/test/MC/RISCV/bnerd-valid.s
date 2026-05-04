# RUN: llvm-mc -triple=riscv32 -show-encoding %s \
# RUN:        | FileCheck %s --check-prefixes=CHECK-ENCODING,CHECK-ASM
# RUN: llvm-mc -triple=riscv64 -show-encoding %s \
# RUN:        | FileCheck %s --check-prefixes=CHECK-ENCODING,CHECK-ASM
;BNERD
bnerd x7, x30, x5, 8 
# CHECK-ASM: bnerd t2, t5, t0, 8 
# CHECK-ENCODING: [0x63,0x34,0x5f,0x38] 
bnerd x7, x30, x5, -2
# CHECK-ASM: bnerd t2, t5, t0, -2 
# CHECK-ENCODING: [0x63,0x3f,0x5f,0xbe]
bnerd x5, x31, x28, 0
# CHECK-ASM:  bnerd t0, t6, t3, 0 
# CHECK-ENCODING: [0x63,0xb0,0xcf,0x29] 
bnerd x5, x31, x28, 126
# CHECK-ASM: bnerd t0, t6, t3, 126 
# CHECK-ENCODING: [0x63,0xbf,0xcf,0x2f]
bnerd x6, x31, x28, -128
# CHECK-ASM: bnerd t1, t6, t3, -128 
# CHECK-ENCODING: [0x63,0xb0,0xcf,0xb1]

;BEQRD
beqrd t2, t5, t0, 8
# CHECK-ASM: beqrd t2, t5, t0, 8
# CHECK-ENCODING: [0x63,0x24,0x5f,0x38] 
beqrd x7, x30, x5, -2
# CHECK-ASM: beqrd t2, t5, t0, -2
# CHECK-ENCODING: [0x63,0x2f,0x5f,0xbe]
beqrd x5, x31, x28, 0
# CHECK-ASM:  beqrd t0, t6, t3, 0
# CHECK-ENCODING: [0x63,0xa0,0xcf,0x29] 
beqrd x5, x31, x28, 126
# CHECK-ASM: beqrd t0, t6, t3, 126
# CHECK-ENCODING: [0x63,0xaf,0xcf,0x2f]
beqrd x6, x31, x28, -128
# CHECK-ASM: beqrd t1, t6, t3, -128
# CHECK-ENCODING: [0x63,0xa0,0xcf,0xb1]