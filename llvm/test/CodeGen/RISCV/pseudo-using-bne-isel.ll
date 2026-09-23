; RUN: llc \
; RUN:   -mtriple=riscv64-linux-gnu \
; RUN:   -mattr=+m,+a,+f,+d,+c,+bnerd,-zaamo \
; RUN:   -O2 \
; RUN:   -riscv-run-bnerd \
; RUN:   -stop-before=riscv-pseudo-bne-branch-expansion \
; RUN:   -verify-machineinstrs \
; RUN:   %s -o - \
; RUN:   | FileCheck %s

; REQUIRES: riscv-registered-target


; -----------------------------------------------------------------------------
; PseudoBEQUsingBNE
;
; 'ne' is inverted because the true path is laid out as fallthrough,
; producing an equality branch to the other block.
; -----------------------------------------------------------------------------

define i64 @pseudo_beq(i64 %a, i64 %b) {
entry:
  %cmp = icmp ne i64 %a, %b
  br i1 %cmp, label %taken, label %fallthrough

taken:
  ret i64 1

fallthrough:
  ret i64 0
}

; CHECK-LABEL: name: pseudo_beq
; CHECK: PseudoBEQUsingBNE


; -----------------------------------------------------------------------------
; PseudoBLTUsingBNE
; -----------------------------------------------------------------------------

define i64 @pseudo_blt(i64 %a, i64 %b) {
entry:
  %cmp = icmp sge i64 %a, %b
  br i1 %cmp, label %taken, label %fallthrough

taken:
  ret i64 1

fallthrough:
  ret i64 0
}

; CHECK-LABEL: name: pseudo_blt
; CHECK: PseudoBLTUsingBNE


; -----------------------------------------------------------------------------
; PseudoBGEUsingBNE
; -----------------------------------------------------------------------------

define i64 @pseudo_bge(i64 %a, i64 %b) {
entry:
  %cmp = icmp slt i64 %a, %b
  br i1 %cmp, label %taken, label %fallthrough

taken:
  ret i64 1

fallthrough:
  ret i64 0
}

; CHECK-LABEL: name: pseudo_bge
; CHECK: PseudoBGEUsingBNE


; -----------------------------------------------------------------------------
; PseudoBLTUUsingBNE
; -----------------------------------------------------------------------------

define i64 @pseudo_bltu(i64 %a, i64 %b) {
entry:
  %cmp = icmp uge i64 %a, %b
  br i1 %cmp, label %taken, label %fallthrough

taken:
  ret i64 1

fallthrough:
  ret i64 0
}

; CHECK-LABEL: name: pseudo_bltu
; CHECK: PseudoBLTUUsingBNE


; -----------------------------------------------------------------------------
; PseudoBGEUUsingBNE
; -----------------------------------------------------------------------------

define i64 @pseudo_bgeu(i64 %a, i64 %b) {
entry:
  %cmp = icmp ult i64 %a, %b
  br i1 %cmp, label %taken, label %fallthrough

taken:
  ret i64 1

fallthrough:
  ret i64 0
}

; CHECK-LABEL: name: pseudo_bgeu
; CHECK: PseudoBGEUUsingBNE


; -----------------------------------------------------------------------------
; BNE control case
;
; BNE doesn't need a UsingBNE pseudo.
; -----------------------------------------------------------------------------

define i64 @bne(i64 %a, i64 %b) {
entry:
  %cmp = icmp eq i64 %a, %b
  br i1 %cmp, label %taken, label %fallthrough

taken:
  ret i64 1

fallthrough:
  ret i64 0
}

; CHECK-LABEL: name: bne
; CHECK: BNE