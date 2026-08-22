//===-- RISCVInsertBNERDSC.cpp - Insert BNERD branch and an SC after LR to clear the reservation -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass detects every LR (load-reserved) instruction immediately followed
// by a conditional branch, picks a register that is unused in the branch's
// non-SC successor, rewrites the branch as a custom BNERD form (func3=0x3,
// imm[11:7] repurposed as a 5-bit register field), and inserts a "bridge"
// MachineBasicBlock that issues a no-op SC using the chosen free register
// before falling through to the original successor.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/RISCVMCTargetDesc.h"
#include "llvm/ADT/Statistic.h"
#define RISCV_INSERT_BNERD_SC_NAME "RISC-V insert bnerd sc"
#define DEBUG_TYPE "riscv-insert-bnerd-sc"

#include "LRSCCountUtils.hpp"
#include "RISCV.h"
#include "RISCVInstrInfo.h"
#include "RISCVSubtarget.h"
#include <cassert>
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/CodeGen/MachinePostDominators.h"



using namespace llvm;
using namespace utils;

static cl::opt<bool> EnableRISCVBNERDSCInsertion(
    "bnerd-stats", cl::Hidden,
    cl::desc(
        "Insert a BNERD branch and an SC after each LR-then-branch pair to clear the reservation. "),
    cl::init(false));

namespace {

  class RISCVInsertBNERDSC : public MachineFunctionPass {
    public:
        static char ID;
    
        RISCVInsertBNERDSC() : MachineFunctionPass(ID) {}
        ~RISCVInsertBNERDSC();
        bool runOnMachineFunction(MachineFunction &MF) override;
        void getAnalysisUsage(AnalysisUsage &AU) const override {
          AU.addRequired<MachinePostDominatorTreeWrapperPass>();
          MachineFunctionPass::getAnalysisUsage(AU);
        }
        StringRef getPassName() const override { return RISCV_INSERT_BNERD_SC_NAME; }
    private:
        // static Register getFreeReg(const MachineBasicBlock &MBB,
        //                         const MachineInstr &Br);

        static MachineBasicBlock::iterator isBranchAfter(MachineInstr &MI);
        const RISCVInstrInfo *TII = nullptr;
          unsigned NumFunctions   = 0;
          unsigned NumLRs   = 0;
  };

} // end anonymous namespace

char RISCVInsertBNERDSC::ID = 0;
INITIALIZE_PASS(RISCVInsertBNERDSC, "riscv-insert-bnerd-sc", RISCV_INSERT_BNERD_SC_NAME, false, false)
FunctionPass *llvm::createRISCVInsertBNERDSCPass() { return new RISCVInsertBNERDSC(); }
RISCVInsertBNERDSC::~RISCVInsertBNERDSC() {
  if (EnableRISCVBNERDSCInsertion) {
    dbgs() << "=== RISCVInsertBNERDSC Stats ===\n";
    dbgs() << "  Functions processed : " << NumFunctions << "\n";
    dbgs() << "  LR/Br pairs found   : " << NumLRs << "\n";
    dbgs() << "================================\n";
  }
}



// Register RISCVInsertBNERDSC::getFreeReg(const MachineBasicBlock &MBB,
//                                 const MachineInstr &Br) {
//   SmallSet<Register, 32> UsedRegs;
//   Register FreeReg;
//   for (const MachineInstr &MI : MBB) {
//     for (const MachineOperand &MO : MI.operands()) {
//       if (!MO.isReg())
//         continue;
//       Register Reg = MO.getReg();
//       if (Reg == 0)
//         continue;
//       UsedRegs.insert(Reg);
//     }
//   }
 
//   // Branch's source operands (rs1, rs2) — these must remain valid through
//   // the BNERD we are about to emit, so they are unavailable as scratch.
//   for (const MachineOperand &MO : Br.operands()) {
//     if (MO.isReg() && MO.getReg() != 0)
//       UsedRegs.insert(MO.getReg());
//   }
 
//   /*
//    * Unconditionally forbidden registers — these must never be used as scratch
//    * regardless of what appears in the target block:
//    *   x0  (zero)  : hardwired zero, writes are discarded by the hardware.
//    *   x1  (ra)    : return address, clobbering it corrupts the call stack.
//    *   x2  (sp)    : stack pointer, clobbering it corrupts the stack frame.
//    *   x3  (gp)    : global pointer, set once by the runtime and never changed;
//    *                  clobbering it breaks all gp-relative global variable accesses.
//    *   x4  (tp)    : thread pointer, set per-thread by the OS/runtime; clobbering
//    *                  it breaks all TLS accesses (errno, __thread variables, etc.).
//    *   x8  (fp/s0) : frame pointer, clobbering it corrupts the frame chain and
//    *                  breaks stack unwinding and debugger backtraces.
//    */
//   UsedRegs.insert(RISCV::X0);
//   UsedRegs.insert(RISCV::X1);  // ra
//   UsedRegs.insert(RISCV::X2);  // sp
//   UsedRegs.insert(RISCV::X3);  // gp
//   UsedRegs.insert(RISCV::X4);  // tp
//   UsedRegs.insert(RISCV::X8);  // fp

//   /*
//    * First-choice pool: caller-saved temporaries (t0–t2 = x5–x7, t3–t6 = x28–x31).
//    * These are the safest scratch registers — they carry no ABI meaning across
//    * call boundaries, are not used as argument or return registers, and the
//    * calling convention already assumes callers do not expect them to survive
//    * a call. The register allocator prefers these for short-lived values too,
//    * so they are the least likely to be live in the target block.
//    */
//   static constexpr std::array<Register, 7> TempRegs = {
//       RISCV::X5,  RISCV::X6,  RISCV::X7,
//       RISCV::X28, RISCV::X29, RISCV::X30, RISCV::X31,
//   };
//   for (Register Reg : TempRegs){
//     if (!UsedRegs.count(Reg)){
//       FreeReg = Reg;
//       return FreeReg;
//     }
//   }
    

//   /*
//    * Fallback pool: callee-saved (s1–s11 = x9, x18–x27) and argument/return
//    * registers (a0–a7 = x10–x17). These are more likely to carry live values
//    * across blocks than the caller-saved temps, but are still valid scratch
//    * choices if every temp is taken. s0/fp (x8) is excluded — it is already
//    * in UsedRegs and serves as the frame pointer.
//    */
//   static constexpr std::array<Register, 19> FallbackRegs = {
//       RISCV::X9,  RISCV::X10, RISCV::X11, RISCV::X12, RISCV::X13,
//       RISCV::X14, RISCV::X15, RISCV::X16, RISCV::X17, RISCV::X18,
//       RISCV::X19, RISCV::X20, RISCV::X21, RISCV::X22, RISCV::X23,
//       RISCV::X24, RISCV::X25, RISCV::X26, RISCV::X27,
//   };
//   for (Register Reg : FallbackRegs)
//     if (!UsedRegs.count(Reg)){
//       FreeReg = Reg;
//       return FreeReg;
//     }
//     assert(FreeReg.isValid() && "RISCVInsertBNERDSC: no free register available in target block");
//   return Register(); // no free register found
  
// }

MachineBasicBlock::iterator RISCVInsertBNERDSC::isBranchAfter(MachineInstr &MI) {
  MachineBasicBlock::iterator MBBI = MI.getIterator();
  MachineBasicBlock::iterator E = MI.getParent()->end();
  while(MBBI != E) {
    switch (MBBI->getOpcode()) {
      case RISCV::BEQ:
      case RISCV::BNE:
      case RISCV::BLT:
      case RISCV::BGE:
      case RISCV::BLTU:
      case RISCV::BGEU:
        return MBBI;
      case RISCV::SC_W:
      case RISCV::SC_D:
      case RISCV::SC_D_AQ:
      case RISCV::SC_W_AQ:
      case RISCV::SC_D_RL:
      case RISCV::SC_W_RL:
      case RISCV::SC_D_AQRL:
      case RISCV::SC_W_AQRL:
        return E;
      default:
        break;
        
    }
    MBBI++;
  }
  return E;
}
// ===========================================================================
// Pass entry point
// ===========================================================================
bool RISCVInsertBNERDSC::runOnMachineFunction(MachineFunction &MF) {
  LLVM_DEBUG(dbgs() << "=== Function: " << MF.getName() << " ===\n");

  
  MachinePostDominatorTree &MPDT = getAnalysis<MachinePostDominatorTreeWrapperPass>().getPostDomTree();
  TII = MF.getSubtarget<RISCVSubtarget>().getInstrInfo();
  ++NumFunctions;
  bool changed = false;
  /*
  * Use SmallPtrSet instead of a vector because we only need fast membership
  * checks and uniqueness for already-seen LR MBBs. A vector would require a
  * linear scan each time we check whether an MBB was already processed, while
  * SmallPtrSet provides efficient lookup and avoids duplicate entries.
  */
  SmallPtrSet<MachineBasicBlock *, 8> seenLRMBBs;
  for (auto &MBB : MF) {
    LLVM_DEBUG(dbgs() << "  *** Exploring BasicBlock: " << MBB.getNumber()
                        << " *** \n");
    if (MBB.getNumber() == -1) {
      continue;
    }
    if(seenLRMBBs.count(&MBB)){
      continue;
    }
    /* Iterator pointing to the first instruction in this basic block. */
    MachineBasicBlock::iterator MBBI = MBB.begin();
    /* Sentinel iterator marking one past the last instruction in this basic
     * block. */
    MachineBasicBlock::iterator E = MBB.end();
    while (MBBI != E){
      MachineInstr &MI = *MBBI;
      ++MBBI;
      /* Extract the opcode of the current instruction for classification. */
      uint16_t opc = MI.getOpcode();
      /* Log the raw opcode value of the current instruction. */
      LLVM_DEBUG(dbgs() << "  Instr: " << TII->getName(MI.getOpcode()) << "\n");
      if(!(utils::lrsc::isLR(opc))){
        continue;
      }
      // found LR
      ++NumLRs;
      if(MBBI == E){
        seenLRMBBs.insert(&MBB);
        continue;
      }
      MachineBasicBlock::iterator BrI= isBranchAfter(*MBBI);
      
      if(BrI==E){
        seenLRMBBs.insert(&MBB);
        continue;
      }
      MachineInstr &Br = *BrI;

      MachinePostDominatorTree &MPDT = getAnalysis<MachinePostDominatorTreeWrapperPass>().getPostDomTree();
      MachineBasicBlock *TargetMBB = nullptr;
      MachineBasicBlock *FalseMBB = nullptr;
      SmallVector<MachineOperand, 4> Cond;

      const TargetInstrInfo *TII = MBB.getParent()->getSubtarget().getInstrInfo();

      if (!TII->analyzeBranch(MBB, TargetMBB, FalseMBB, Cond)) {
        FalseMBB = FalseMBB ? FalseMBB : MBB.getFallThrough();
      }

      SmallPtrSet<MachineBasicBlock *, 16> Visited;
      MachineBasicBlock *SCMBB = lrsc::findSCMBBDFS(&MBB, Visited);
      MachineBasicBlock *LR_MBB = &MBB;
      if ( !lrsc::isConditionalLRSC(&MBB, LR_MBB, SCMBB, TargetMBB, MPDT)) {
        LLVM_DEBUG(dbgs() << "=== Unconditional" << " ===\n");
        seenLRMBBs.insert(&MBB);
        continue;
      }
      LLVM_DEBUG(dbgs() << "=== Conditional" << " ===\n");
      if(Br.getOpcode() != RISCV::BNE ){
        continue;
      }

      MachineBasicBlock *CondTarget = Br.getOperand(Br.getNumExplicitOperands() - 1).getMBB();

      MachineInstr *UncondBr = nullptr;
      auto NextI = std::next(BrI);

      while(NextI != E && NextI->isDebugInstr()){
        ++NextI;
      }

      if(NextI != E && NextI->getOpcode() == RISCV::PseudoBR){
        UncondBr = &*NextI;
      }


      if(!TargetMBB || !SCMBB){
        continue;
      }

      bool NonSCIsTaken = (CondTarget == TargetMBB);
      bool NonSCIsNotTaken = false;

      if(UncondBr){
        MachineBasicBlock *UncondTarget = UncondBr->getOperand(UncondBr->getNumExplicitOperands() - 1).getMBB();

        if(CondTarget == SCMBB && UncondTarget == TargetMBB){
          NonSCIsNotTaken = true;
        }
      }

      if(!NonSCIsTaken && !NonSCIsNotTaken){
        continue;
      }
      // Register FreeReg = getFreeReg(*TargetMBB, Br);
      Register FreeReg = RISCV::X0;

      LLVM_DEBUG(dbgs() << "RISCVInsertBNERDSC: rewriting LR/Br in "
                  << MBB.getName()
                  << ", FreeReg=" << printReg(FreeReg) << "\n");
      
      DebugLoc DL = Br.getDebugLoc();
      Register Rs1 = Br.getOperand(0).getReg();
      Register Rs2 = Br.getOperand(1).getReg();
      

      MachineBasicBlock *BridgeSCBB = MF.CreateMachineBasicBlock(LR_MBB->getBasicBlock());
      //  MF.insert(InsertPos, BridgeSCBB); This would cause problems after applying the changes with the successors
      // insert at end of function — doesn't disrupt any existing fall-throughs
      MF.push_back(BridgeSCBB);

      // SC_W x0, FreeReg, x0 — clears the LR reservation as a side-effect.
      BuildMI(BridgeSCBB, DebugLoc(), TII->get(RISCV::SC_W))
          .addReg(RISCV::X0, RegState::Define)  // rd
          .addReg(FreeReg)                      // rs2
          .addReg(RISCV::X0);                   // rs1

      BridgeSCBB->addLiveIn(FreeReg);
      // Unconditional jump to the original target.
      BuildMI(BridgeSCBB, DebugLoc(), TII->get(RISCV::PseudoBR)).addMBB(TargetMBB);
      

      BuildMI(*LR_MBB, Br, DL, TII->get(RISCV::BNERD))
          .addReg(FreeReg, RegState::Define)   // encoded into imm[11:7]
          .addReg(Rs1)
          .addReg(Rs2)
          .addMBB(BridgeSCBB) // retargeted below
          .getInstr();

      BridgeSCBB->addLiveIn(FreeReg);
      // Advancing past Br before erasing it.
      MBBI = std::next(Br.getIterator());
      Br.eraseFromParent();
      // Rewire CFG edges.
      LR_MBB->removeSuccessor(TargetMBB);
      LR_MBB->addSuccessor(BridgeSCBB);

      BridgeSCBB->addSuccessor(TargetMBB);

      // LR_MBB->updateTerminator(LR_MBB->getNextNode());
      // BridgeSCBB->updateTerminator(BridgeSCBB->getNextNode());

      seenLRMBBs.insert(LR_MBB);

      changed = true;
      break;
    } //end of inner loop
    
  } // end of outer loop
  return changed;
} //end of function

