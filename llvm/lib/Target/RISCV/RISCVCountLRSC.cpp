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

#include "LRSCCountUtils.hpp"
#include "llvm/Support/CommandLine.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "RISCV.h"
#include "RISCVInstrInfo.h"
#include "RISCVTargetMachine.h"
#include <string>
#include <unordered_map>
#include <vector>

using namespace llvm;
#define RISCV_COUNT_LR_SC_NAME "RISC-V count LR/SC instruction pairs"
#define DEBUG_TYPE "riscvcntlrsc"

static cl::opt<bool> RISCVCountLRSCEmitJSON(
    "dump-insn-stats-json", cl::Hidden,
    cl::desc(
        "The JSON Emission Control is used for controlling JSON format emission. "),
    cl::init(false));

namespace {

class RISCVCountLRSC : public MachineFunctionPass {
public:
  const RISCVSubtarget *STI;
  const RISCVInstrInfo *TII;

  static char ID;

  RISCVCountLRSC() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override { return RISCV_COUNT_LR_SC_NAME; }

  void dumpStats(raw_ostream &OS, MachineFunction &MF) const;

private:
  /* Struct defined in LRSCCountUtils.hpp. */
  utils::LRSCCounts Counts;

  /* Added MachineFunction &MF as a parameter so LR/SC counts can be
   * attributed to the containing function.
   */
  unsigned countLRSC(MachineBasicBlock &MBB,
                     MachineFunction &MF);
  unsigned totalCount = 0;
};

} // end anonymous namespace

std::string stringifyOpcode(uint16_t opc){
  switch (opc) {
  // LR flavours
  case RISCV::LR_W:      return "LR_W";
  case RISCV::LR_D:      return "LR_D";
  case RISCV::LR_D_AQ:   return "LR_D_AQ";
  case RISCV::LR_W_AQ:   return "LR_W_AQ";
  case RISCV::LR_D_RL:   return "LR_D_RL";
  case RISCV::LR_W_RL:   return "LR_W_RL";
  case RISCV::LR_D_AQRL: return "LR_D_AQRL";
  case RISCV::LR_W_AQRL: return "LR_W_AQRL";

  // SC flavours
  case RISCV::SC_W:      return "SC_W";
  case RISCV::SC_D:      return "SC_D";
  case RISCV::SC_D_AQ:   return "SC_D_AQ";
  case RISCV::SC_W_AQ:   return "SC_W_AQ";
  case RISCV::SC_D_RL:   return "SC_D_RL";
  case RISCV::SC_W_RL:   return "SC_W_RL";
  case RISCV::SC_D_AQRL: return "SC_D_AQRL";
  case RISCV::SC_W_AQRL: return "SC_W_AQRL";

  default:
    return "";
  }
}
char RISCVCountLRSC::ID = 0;
INITIALIZE_PASS(RISCVCountLRSC, "riscv-count-lr-sc", RISCV_COUNT_LR_SC_NAME,
                false, false)

FunctionPass *llvm::createRISCVCountLRSCPass() { return new RISCVCountLRSC(); }

bool RISCVCountLRSC::runOnMachineFunction(MachineFunction &MF) {
  llvm::errs() << "RISCVCountLRSC: " << MF.getName() << "\n";

  /* Clear stored basic block iteration order for this MachineFunction so the
   * basic block index runs from 0 to N - 1 for this function.
   */
  auto &F = MF;

  Counts.basicBlockOrder[&F] = std::vector<const MachineBasicBlock*>(0, nullptr);

  /* Alias the per-function basic block order vector
   * (MF -> [basic block pointers in traversal order]) for a stable bb_index.
   */
  auto &Order =
      Counts.basicBlockOrder[&F];

  /* Subtarget instruction CPU features: extensions, scheduling model, etc. */
  STI =
      &MF.getSubtarget<RISCVSubtarget>();

  /* Instruction info table: opcodes, pseudo expansion info, etc. */
  TII = STI->getInstrInfo();

  unsigned insnPerBBCnt = 0;
  unsigned insnPerMFCnt = 0;

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
    insnPerBBCnt = countLRSC(MBB, MF);

    /* Ensure the MF -> BB entry exists even if this basic block has zero
     * LR/SC instructions.
     */
    Counts.basicBlocksCounts[&F].try_emplace(
        &MBB, 0);

    /* Update the MF -> BB -> count mapping with the LR/SC count for this
     * basic block.
     */
    Counts.updateBBCnt(MF, MBB, insnPerBBCnt);

    /* Accumulate the LR/SC count for this basic block into the total for this
     * function.
     */
    insnPerMFCnt += insnPerBBCnt;
  }

  /* Accumulate the per-function LR/SC count into the total for the entire
   * compilation unit.
   */
  totalCount += insnPerMFCnt;

  /* Update the Func -> count mapping with the LR/SC count for this function. */
  Counts.updateFuncCnt(MF, insnPerMFCnt);

  dumpStats(dbgs(), MF);

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
      case RISCV::LR_W:
      case RISCV::LR_D:
      case RISCV::LR_D_AQ:
      case RISCV::LR_W_AQ:
      case RISCV::LR_D_RL:
      case RISCV::LR_W_RL:
      case RISCV::LR_D_AQRL:
      case RISCV::LR_W_AQRL:
      // SC flavours
      case RISCV::SC_W:
      case RISCV::SC_D:
      case RISCV::SC_D_AQ:
      case RISCV::SC_W_AQ:
      case RISCV::SC_D_RL:
      case RISCV::SC_W_RL:
      case RISCV::SC_D_AQRL:
      case RISCV::SC_W_AQRL:
        Counts.updateBBFlavCnt(MF, MBB, stringifyOpcode(opc));
        total++;
        break;

      default:
      /* Opcode is not an LR/SC flavour that this pass tracks. */
        break;
    }
      MBBI++;
  }
  return total;
}

void RISCVCountLRSC::dumpStats(raw_ostream &OS, MachineFunction& MF) const {
  if (RISCVCountLRSCEmitJSON) {
      std::string fName =  MF.getName().str() + ".lrscStats.json";
      int FD;
      std::error_code EC = sys::fs::openFileForWrite(
        fName, FD, sys::fs::CD_CreateAlways, sys::fs::OF_Text);
      raw_fd_ostream OS(FD, /*shouldClose=*/ true);
      llvm::json::Value J = Counts.toJSON();
      OS << llvm::formatv("{0:2}", J) << "\n";
      return;
  }

  OS << "Number of LR/SC instruction: " << totalCount << "\n";
}

