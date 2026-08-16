; RUN: llc -mtriple=riscv64 -mattr=+a -O2 -debug-only=riscvcntlrsc \
; RUN:   %s -o /dev/null 2>&1 | FileCheck %s
; REQUIRES: asserts

; cmpxchg expands to an LR/SC pair in separate basic blocks with a retry cycle
; through both blocks.
define void @cmpxchg_different_blocks(ptr %p) nounwind {
  %r = cmpxchg ptr %p, i32 0, i32 1 acquire monotonic, align 4
  ret void
}

; CHECK: === Matches ===
; CHECK: LR: LR_W_AQ:{{\$x[0-9]+}} SC: SC_W Distance: {{[0-9]+}}

; Cycle spans two distinct blocks: LR block -> SC block -> LR block.
; CHECK: === Cycles ===
; CHECK: Cycle: [[LR:[0-9]+]] -> [[SC:[0-9]+]] -> [[LR]] -> [LRSC]
