//===-- RISCVCountLRSC.cpp - Count LR/SC instruction pairs -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains a pass that counts the # of the LR/SC pairs. This pass
// should run just before code generation.
//
//===----------------------------------------------------------------------===//

#include "RISCV.h"
#include "RISCVInstrInfo.h"
#include "RISCVTargetMachine.h"
#include "RISCVCountLRSC.h"
using namespace llvm;
#define RISCV_COUNT_LR_SC_NAME "RISC-V count LR/SC instruction pairs"
#define DEBUG_TYPE "riscvcntlrsc"

namespace {

class RISCVCountLRSC : public MachineFunctionPass {
public:
  const RISCVSubtarget *STI;
  const RISCVInstrInfo *TII;

  static char ID;

  RISCVCountLRSC() : MachineFunctionPass(ID) {}
  ~RISCVCountLRSC();

  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override { return RISCV_COUNT_LR_SC_NAME; }

  void print(raw_ostream &OS) const;

private:
  /* Struct defined in LRSCCountUtils.hpp. */
  LRSCCounts Counts;
  
  /* Added MachineFunction &MF as a parameter so LR/SC counts can be
   * attributed to the containing function.
   */
  unsigned countLRSC(MachineBasicBlock &MBB,
                     MachineFunction &MF);
  unsigned totalCount = 0;


};

} // end anonymous namespace

char RISCVCountLRSC::ID = 0;
INITIALIZE_PASS(RISCVCountLRSC, "riscv-count-lr-sc", RISCV_COUNT_LR_SC_NAME,
                false, false)

RISCVCountLRSC::~RISCVCountLRSC() { print(dbgs()); }

FunctionPass *llvm::createRISCVCountLRSCPass() { return new RISCVCountLRSC(); }

bool RISCVCountLRSC::runOnMachineFunction(MachineFunction &MF) {
  llvm::errs() << "RISCVCountLRSC: " << MF.getName() << "\n";

  /* Clear stored basic block iteration order for this MachineFunction so the
   * basic block index runs from 0 to N - 1 for this function.
   */
  Counts.basicBlockOrder[&MF]
      .clear();

  /* Alias the per-function basic block order vector
   * (MF -> [basic block pointers in traversal order]) for a stable bb_index.
   */
  auto &Order =
      Counts.basicBlockOrder[&MF];

  /* Subtarget instruction CPU features: extensions, scheduling model, etc. */
  STI =
      &MF.getSubtarget<RISCVSubtarget>();

  /* Instruction info table: opcodes, pseudo expansion info, etc. */
  TII = STI->getInstrInfo();

  unsigned bbCount = 0;
  unsigned mfCount = 0;

  /* Traverse the machine basic blocks in this MachineFunction and accumulate
   * LR/SC instruction counts per basic block and for the entire function.
   * Note: this currently counts individual LR/SC instructions, not explicit
   * LR/SC pairs.
   */
  for (auto &MBB : MF) {

    /* Record this basic block pointer in traversal order so JSON can emit all
     * basic blocks (including zero-count ones) with a stable bb_index.
     */
    Order.push_back(
        &MBB);

    /* Number of LR/SC instructions detected in this basic block. */
    bbCount = countLRSC(MBB, MF);

    /* Ensure the MF -> BB entry exists even if this basic block has zero
     * LR/SC instructions.
     */
    Counts.basicBlocksCounts[&MF].try_emplace(
        &MBB, 0);

    /* Update the MF -> BB -> count mapping with the LR/SC count for this
     * basic block.
     */
    Counts.updateBBCnt(MF,MBB, bbCount);

    /* Accumulate the LR/SC count for this basic block into the total for this
     * function.
     */
    mfCount += bbCount;
  }

  /* Accumulate the per-function LR/SC count into the total for the entire
   * compilation unit.
   */
  totalCount += mfCount;

  /* Update the MF -> count mapping with the LR/SC count for this function. */
  Counts.updateMFCnt(MF, mfCount);

  /* This pass is read-only and does not modify the MachineFunction, so
   * return false.
   */
  return false;
}

unsigned
RISCVCountLRSC::countLRSC(MachineBasicBlock &MBB,
                          MachineFunction &MF) {

  MachineBasicBlock::iterator MBBI = MBB.begin();
  MachineBasicBlock::iterator E = MBB.end();

  /* Iterate over each instruction in the basic block, classify its opcode
   * against all LR/SC instruction flavours, and update the per-flavour
   * counts in the LRSCCounts mapping.
   */
  unsigned total = 0;
  uint16_t opc = 0;
  while (MBBI != E) {

   opc = MBBI->getOpcode();
switch (opc) {
        // LR flavours
      case RISCV::LR_W:      Counts.updateBBFlavCnt(MF, MBB, "LR_W");      total++; break;
      case RISCV::LR_D:      Counts.updateBBFlavCnt(MF, MBB, "LR_D");      total++; break;
      case RISCV::LR_D_AQ:   Counts.updateBBFlavCnt(MF, MBB, "LR_D_AQ");   total++; break;
      case RISCV::LR_W_AQ:   Counts.updateBBFlavCnt(MF, MBB, "LR_W_AQ");   total++; break;
      case RISCV::LR_D_RL:   Counts.updateBBFlavCnt(MF, MBB, "LR_D_RL");   total++; break;
      case RISCV::LR_W_RL:   Counts.updateBBFlavCnt(MF, MBB, "LR_W_RL");   total++; break;
      case RISCV::LR_D_AQRL: Counts.updateBBFlavCnt(MF, MBB, "LR_D_AQRL"); total++; break;
      case RISCV::LR_W_AQRL: Counts.updateBBFlavCnt(MF, MBB, "LR_W_AQRL"); total++; break;

      // SC flavours
      case RISCV::SC_W:      Counts.updateBBFlavCnt(MF, MBB, "SC_W");      total++; break;
      case RISCV::SC_D:      Counts.updateBBFlavCnt(MF, MBB, "SC_D");      total++; break;
      case RISCV::SC_D_AQ:   Counts.updateBBFlavCnt(MF, MBB, "SC_D_AQ");   total++; break;
      case RISCV::SC_W_AQ:   Counts.updateBBFlavCnt(MF, MBB, "SC_W_AQ");   total++; break;
      case RISCV::SC_D_RL:   Counts.updateBBFlavCnt(MF, MBB, "SC_D_RL");   total++; break;
      case RISCV::SC_W_RL:   Counts.updateBBFlavCnt(MF, MBB, "SC_W_RL");   total++; break;
      case RISCV::SC_D_AQRL: Counts.updateBBFlavCnt(MF, MBB, "SC_D_AQRL"); total++; break;
      case RISCV::SC_W_AQRL: Counts.updateBBFlavCnt(MF, MBB, "SC_W_AQRL"); total++; break;

      default:
    /* Opcode is not an LR/SC flavour that this pass tracks. */
    break;
  }
}
  return total;
}

void RISCVCountLRSC::print(raw_ostream &OS) const {
  OS << "Number of LR/SC instruction pairs: " << " " << totalCount << "\n";
  llvm::json::Value J = Counts.toJSON();
  #if defined(EM_JSON)
  OS << "EM_JSON enabled\n";
  OS << llvm::formatv("{0:2}", J) << "\n";
  
  #else
  OS << "EM_JSON disabled\n";
  #endif
  /* Pretty-print JSON with indent = 2. */
}
  
