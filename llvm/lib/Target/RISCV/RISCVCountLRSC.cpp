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

#define RISCV_COUNT_LR_SC_NAME "RISC-V count LR/SC instruction pairs"
#define DEBUG_TYPE "riscvcntlrsc"


#include "LRSCCountUtils.hpp"
#include "llvm/Support/CommandLine.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include "RISCV.h"
#include "RISCVInstrInfo.h"
#include "RISCVTargetMachine.h"
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <algorithm>

using namespace llvm;
using namespace utils;

static cl::opt<bool> RISCVCountLRSCEmitJSON(
    "dump-insn-stats-json", cl::Hidden,
    cl::desc(
        "The JSON Emission Control is used for controlling JSON format emission. "),
    cl::init(false));

namespace {

class RISCVCountLRSC : public MachineFunctionPass {
public:

  static char ID;

  static std::string ModuleName;

  RISCVCountLRSC() : MachineFunctionPass(ID) {}
  ~RISCVCountLRSC();

  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override { return RISCV_COUNT_LR_SC_NAME; }

  void updateStats(MachineFunction &MF);
  void dumpJSONStats(raw_ostream &OS);

private:

  /* Added MachineFunction &MF as a parameter so LR/SC counts can be
   * attributed to the containing function.
   */
  unsigned countLRSC(utils::LRSCCounts &Counts, MachineBasicBlock &MBB);
  unsigned totalCount = 0;

  /* Struct defined in LRSCCountUtils.hpp. */
  utils::LRSCCounts Counts;
  utils::LRSCAnalyzer DistanceAndCycle;
};

} // end anonymous namespace



char RISCVCountLRSC::ID = 0;
std::string RISCVCountLRSC::ModuleName = "";

INITIALIZE_PASS(RISCVCountLRSC, "riscv-count-lr-sc", RISCV_COUNT_LR_SC_NAME,
                false, false)

FunctionPass *llvm::createRISCVCountLRSCPass() { return new RISCVCountLRSC(); }

RISCVCountLRSC::~RISCVCountLRSC() {
  if (RISCVCountLRSCEmitJSON) {
    dumpJSONStats(dbgs());
  }
}

bool RISCVCountLRSC::runOnMachineFunction(MachineFunction &MF) {

  LLVM_DEBUG(dbgs() << "=== Function: " << MF.getName() << " ===\n");

  /* Clear stored basic block iteration order for this MachineFunction so the
   * basic block index runs from 0 to N - 1 for this function.
   */

  llvm::Module* m = MF.getFunction().getParent();
  RISCVCountLRSC::ModuleName = m->getModuleIdentifier();

  Counts.basicBlockOrder = std::vector<const MachineBasicBlock*>(0, nullptr);

  /* Alias the per-function basic block order vector
   * (MF -> [basic block pointers in traversal order]) for a stable bb_index.
   */
  auto &Order = Counts.basicBlockOrder;

  unsigned insnPerBBCnt = 0;
  unsigned insnPerMFCnt = 0;

  /* Traverse the machine basic blocks in this MachineFunction and accumulate
   * LR/SC instruction counts per basic block and for the entire function.
   * Note: this currently counts individual LR/SC instructions, not explicit
   * LR/SC pairs.
   */
  for (auto &MBB : MF) {

    
    // skip unreachable blocks
    if (MBB.getNumber() == -1) {
      continue;
    }

    /* Record this basic block pointer in traversal order so JSON can emit all
     * basic blocks (including zero-count ones) with a stable bb_index.
     */
    Order.push_back(&MBB);

    /* Number of LR/SC instructions detected in this basic block. */
    insnPerBBCnt = countLRSC(Counts, MBB);

    /* Ensure the MF -> BB entry exists even if this basic block has zero
     * LR/SC instructions.
     */
    Counts.BBCount.try_emplace(&MBB, 0);

    /* Update the MF -> BB -> count mapping with the LR/SC count for this
     * basic block.
     */
    Counts.updateBBCnt(MBB, insnPerBBCnt);

    /* Accumulate the LR/SC count for this basic block into the total for this
     * function.
     */
    insnPerMFCnt += insnPerBBCnt;
  }
  
  utils::MatchResult result = DistanceAndCycle.computeLRSCDistancesAndCycles(MF);
  lrsc::dump(result);

  /* Accumulate the per-function LR/SC count into the total for the entire
   * compilation unit.
   */
  totalCount += insnPerMFCnt;

  /* Update the Func -> count mapping with the LR/SC count for this function. */
  Counts.updateFuncCnt(insnPerMFCnt);

  updateStats(MF);

  /* This pass is read-only and does not modify the MachineFunction, so
   * return false.
   */
  return false;
}

unsigned
RISCVCountLRSC::countLRSC(utils::LRSCCounts &Counts, MachineBasicBlock &MBB) {

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
        Counts.updateBBFlavCnt(MBB, lrsc::stringifyOpcode(opc));
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

void RISCVCountLRSC::dumpJSONStats(raw_ostream &OS) {
  if (RISCVCountLRSCEmitJSON) {
    std::string fName =  RISCVCountLRSC::ModuleName + ".lrscStats.json";
    int FD;
    std::error_code EC = sys::fs::openFileForWrite(
        fName, FD, sys::fs::CD_CreateAlways, sys::fs::OF_Text);
    raw_fd_ostream OS(FD, /*shouldClose=*/ true);
    OS << llvm::formatv("{0:2}", Counts.getJSONObj()) << "\n";
    
  }
  return;
}

void RISCVCountLRSC::updateStats(MachineFunction& MF) {
  if (RISCVCountLRSCEmitJSON) {
    Counts.toJSON(MF);
    return;
  }
}