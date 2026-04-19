; RUN: llc -mtriple=riscv64 -mattr=+a -O2 -debug-only=riscvcntlrsc \
; RUN:   %s -o /dev/null 2>&1 | FileCheck %s
; REQUIRES: asserts

; atomicrmw xchg expands to an LR/SC pair in the same basic block with a
; self-edge retry loop.
define void @xchg_same_block(ptr %p) nounwind {
  %r = atomicrmw xchg ptr %p, i8 1 acquire, align 1
  ret void
}

; CHECK: === Matches ===
; CHECK: LR: {{\$x[0-9]+}} SC: SC_W Distance: {{[0-9]+}}

; Cycle is a self-edge on a single block.
; CHECK: === Cycles ===
; CHECK: Cycle: [[BB:[0-9]+]] -> [[BB]] -> [LR cycle]
; Safety check: make sure we don't get a repeated 3-step loop back to the same node now that the succ1==succ2 bug is fixed.
; CHECK-NOT: Cycle: [[BB]] -> [[BB]] -> [[BB]] ->
