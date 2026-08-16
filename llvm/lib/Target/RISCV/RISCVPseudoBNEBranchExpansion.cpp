//===-- RISCVPseudoBNEBranchExpansion.cpp - Canonicalize branches to BNE -===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the RISC-V PseudoBNE branch Expansion pass.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/RISCVMCTargetDesc.h"
#include "RISCVSubtarget.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Instructions.h"
#define RISCV_PSEUDO_BNE_BRANCH_EXPANSION_NAME \
  "RISC-V PseudoBNE branch Expansion"

#define DEBUG_TYPE "riscv-pseudo-bne-branch-expansion"

#include "RISCVRegisterInfo.h"
#include "LRSCCountUtils.hpp"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "RISCV.h"
#include "RISCVInstrInfo.h"
#include "llvm/Support/Debug.h"
#include <string>


using namespace llvm;
using namespace utils::lrsc;

namespace {

class RISCVPseudoBNEBranchExpansion : public MachineFunctionPass {
public:
  static char ID;

  static std::string ModuleName;
  RISCVPseudoBNEBranchExpansion()
      : MachineFunctionPass(ID) {}

  StringRef getPassName() const override {
    return RISCV_PSEUDO_BNE_BRANCH_EXPANSION_NAME;
  }

  bool isPseudoBranchOpc(unsigned Opc);
  bool branchToBNE (MachineBasicBlock &MBB, MachineBasicBlock::iterator BrI, bool isAfterLR);
  bool isSCMBB(const MachineBasicBlock &MBB);

  bool runOnMachineFunction(MachineFunction &MF) override;
private:
  const RISCVInstrInfo *TII = nullptr;
  MachineRegisterInfo *MRI = nullptr;
  
};

} // end anonymous namespace

char RISCVPseudoBNEBranchExpansion::ID = 0;
std::string RISCVPseudoBNEBranchExpansion::ModuleName = "";

INITIALIZE_PASS(
    RISCVPseudoBNEBranchExpansion,
    "riscv-bne-branch-expansion",
    RISCV_PSEUDO_BNE_BRANCH_EXPANSION_NAME,
    false,
    false)

FunctionPass *llvm::createRISCVPseudoBNEBranchExpansionPass() {
  return new RISCVPseudoBNEBranchExpansion();
}

bool RISCVPseudoBNEBranchExpansion::runOnMachineFunction(
    MachineFunction &MF) {
  MRI = &MF.getRegInfo();
  TII = MF.getSubtarget<RISCVSubtarget>().getInstrInfo();

  llvm::Module *M = MF.getFunction().getParent();
  RISCVPseudoBNEBranchExpansion::ModuleName = M->getModuleIdentifier();

  bool Changed = false;
  bool isAfterLR = false;
  LLVM_DEBUG(
      dbgs() << "[PSEUDOBNEExpansion] entered function"
             << " MF=" << &MF
             << " function=" << MF.getName()
             << " module=" << ModuleName
             << "\n");

  for (MachineBasicBlock &MBB : MF) {
    LLVM_DEBUG(
        dbgs() << "[PSEUDOBNEExpansion] examining MBB"
               << " MBB=" << &MBB
               << " MBB#=" << MBB.getNumber()
               << " instructions=" << MBB.size()
               << "\n");

    auto MBBI = MBB.begin();
    auto E = MBB.end();
    isAfterLR = false;
    while (MBBI != E) {
      unsigned Opc = MBBI->getOpcode();
      auto NewMBBI = MBBI++;

      LLVM_DEBUG(
          dbgs() << "[PSEUDOBNEExpansion] current instruction"
                << " MBB=" << &MBB
                << " MBB#=" << MBB.getNumber()
                << " MI=" << &*NewMBBI
                << " opcode=" << TII->getName(Opc)
                << "\n";
          dbgs() << "[PSEUDOBNEExpansion] MI: ";
          NewMBBI->print(dbgs());
      );

      if (isLR(Opc)) {
        isAfterLR = true;

        auto ScanI = std::next(NewMBBI);
        bool FoundBranch = false;

        while (ScanI != E) {
          unsigned ScanOpc = ScanI->getOpcode();

          LLVM_DEBUG(
              dbgs() << "[PSEUDOBNEExpansion] scanning for branch"
                    << " MBB=" << &MBB
                    << " MBB#=" << MBB.getNumber()
                    << " ScanMI=" << &*ScanI
                    << " opcode=" << TII->getName(ScanOpc)
                    << "\n";
              dbgs() << "[PSEUDOBNEExpansion] scanned MI: ";
              ScanI->print(dbgs());
          );

          if (isPseudoBranchOpc(ScanOpc)) {
            FoundBranch = true;

            LLVM_DEBUG(
                dbgs() << "[PSEUDOBNEExpansion] found candidate branch"
                      << " LR_MBB=" << &MBB
                      << " MBB#=" << MBB.getNumber()
                      << " Branch=" << &*ScanI
                      << " opcode=" << TII->getName(ScanOpc)
                      << "\n");

            bool BranchChanged = branchToBNE(MBB, ScanI, isAfterLR);
            Changed |= BranchChanged;

            LLVM_DEBUG(
                dbgs() << "[PSEUDOBNEExpansion] branchToBNE returned"
                      << " MBB=" << &MBB
                      << " MBB#=" << MBB.getNumber()
                      << " changed=" << BranchChanged
                      << " totalChanged=" << Changed
                      << "\n");

            break;
          }

          ++ScanI;
        }

        LLVM_DEBUG(
            if (!FoundBranch && ScanI == E) {
              dbgs() << "[PSEUDOBNEExpansion] reached end of MBB after LR"
                    << " MBB=" << &MBB
                    << " MBB#=" << MBB.getNumber()
                    << "\n";
            }
        );
      } else {
        LLVM_DEBUG(
            dbgs() << "[PSEUDOBNEExpansion] found candidate branch -> original instruction"
                  << " NON_LR_MBB=" << &MBB
                  << " MBB#=" << MBB.getNumber()
                  << " Branch=" << &*NewMBBI
                  << " opcode=" << TII->getName(NewMBBI->getOpcode())
                  << "\n");

        bool BranchChanged = branchToBNE(MBB, NewMBBI, isAfterLR);
        Changed |= BranchChanged;

        LLVM_DEBUG(
            dbgs() << "[PSEUDOBNEExpansion] branchToBNE returned"
                  << " MBB=" << &MBB
                  << " MBB#=" << MBB.getNumber()
                  << " changed=" << BranchChanged
                  << " totalChanged=" << Changed
                  << "\n");
      }
    }

    LLVM_DEBUG(
        dbgs() << "[PSEUDOBNEExpansion] finished MBB"
               << " MBB=" << &MBB
               << " MBB#=" << MBB.getNumber()
               << " totalChanged=" << Changed
               << "\n");
  }

  LLVM_DEBUG(
      dbgs() << "[PSEUDOBNEExpansion] leaving function"
             << " function=" << MF.getName()
             << " changed=" << Changed
             << "\n");
  return Changed;

}

bool RISCVPseudoBNEBranchExpansion::branchToBNE(
    MachineBasicBlock &MBB,
    MachineBasicBlock::iterator BrI,
    bool isAfterLR) {

  MachineInstr &Br = *BrI;
  unsigned BrOpcode = Br.getOpcode();

  LLVM_DEBUG(
      dbgs() << "[branchToBNE] entered"
             << " MBB=" << &MBB
             << " MBB#=" << MBB.getNumber()
             << " Br=" << &Br
             << " opcode=" << TII->getName(BrOpcode)
             << "\n";
      dbgs() << "[branchToBNE] original instruction: ";
      Br.print(dbgs());
  );

  if (!isPseudoBranchOpc(BrOpcode)) {
    LLVM_DEBUG(
        dbgs() << "[branchToBNE] unsupported branch opcode="
               << TII->getName(BrOpcode) << "\n");
    return false;
  }

  if (BrOpcode == RISCV::BNE) {
    LLVM_DEBUG(
        dbgs() << "[branchToBNE] branch is already BNE; skipping\n");
    return false;
  }

  if (Br.getNumExplicitOperands() < 4 ||
      !Br.getOperand(0).isReg() ||
      !Br.getOperand(1).isReg() ||
      !Br.getOperand(2).isReg() ||
      !Br.getOperand(3).isMBB()) {
    LLVM_DEBUG(
        dbgs() << "[branchToBNE] invalid branch operands"
               << " explicit-operands=" << Br.getNumExplicitOperands()
               << "\n");
    return false;
  }
  Register ScratchReg = Br.getOperand(0).getReg();
  Register RS1 = Br.getOperand(1).getReg();
  Register RS2 = Br.getOperand(2).getReg();

  MachineBasicBlock *OriginalTarget = Br.getOperand(3).getMBB();

  MachineBasicBlock *OtherSuccessor = nullptr;

  for (MachineBasicBlock *Successor : MBB.successors()) {
    LLVM_DEBUG(
        dbgs() << "[branchToBNE] examining successor"
               << " CurrentMBB=" << &MBB
               << " Successor=" << Successor
               << " Successor#=" << Successor->getNumber()
               << " OriginalTarget=" << OriginalTarget
               << "\n");

    if (Successor != OriginalTarget) {
      OtherSuccessor = Successor;
      break;
    }
  }

  LLVM_DEBUG(
      dbgs() << "[branchToBNE] selected blocks"
             << " LR_MBB=" << &MBB
             << " OriginalTarget=" << OriginalTarget
             << " OtherSuccessor=" << OtherSuccessor
             << "\n";
      dbgs() << "[branchToBNE] operands"
              << " ScratchReg=" << printReg(ScratchReg, MRI->getTargetRegisterInfo())
             << " Rs1=" << printReg(RS1, MRI->getTargetRegisterInfo())
             << " Rs2=" << printReg(RS2, MRI->getTargetRegisterInfo())
             << "\n";
  );

  DebugLoc DL = Br.getDebugLoc();
  if(isAfterLR) {
    switch (BrOpcode) {
      case RISCV::PseudoBEQUsingBNE: {
        if (!OtherSuccessor) {
          LLVM_DEBUG(
              dbgs() << "[branchToBNE][BEQ] no other successor; skipping\n");
          return false;
        }

        LLVM_DEBUG(
            dbgs() << "[branchToBNE][BEQ] converting"
                  << " LR_MBB=" << &MBB
                  << " BNETarget=" << OtherSuccessor
                  << " PseudoBRTarget=" << OriginalTarget
                  << "\n");

        MachineInstr *NewBNE =
            BuildMI(MBB, BrI, DL, TII->get(RISCV::BNE))
                .addReg(RS1)
                .addReg(RS2)
                .addMBB(OtherSuccessor)
                .getInstr();

        MachineInstr *NewPseudoBR =
            BuildMI(MBB, BrI, DL, TII->get(RISCV::PseudoBR))
                .addMBB(OriginalTarget)
                .getInstr();

        LLVM_DEBUG(
            dbgs() << "[branchToBNE][BEQ] created"
                  << " NewBNE=" << NewBNE
                  << " NewPseudoBR=" << NewPseudoBR
                  << "\n";
            dbgs() << "[branchToBNE][BEQ] new BNE: ";
            NewBNE->print(dbgs());
            dbgs() << "[branchToBNE][BEQ] new PseudoBR: ";
            NewPseudoBR->print(dbgs());
            dbgs() << "[branchToBNE][BEQ] erasing old branch=" << &Br
                  << "\n";
        );

        Br.eraseFromParent();
        

        LLVM_DEBUG(
            dbgs() << "[branchToBNE][BEQ] replacement complete"
                  << " MBB=" << &MBB
                  << " MBB#=" << MBB.getNumber()
                  << "\n");

        return true;
      }

      case RISCV::PseudoBLTUsingBNE: {
        
            

        LLVM_DEBUG(
            dbgs() << "[branchToBNE][BLT] converting"
                  << " LR_MBB=" << &MBB
                  << " TargetMBB=" << OriginalTarget
                  << " ScratchReg="
                  << printReg(ScratchReg, MRI->getTargetRegisterInfo())
                  << "\n");

        MachineInstr *NewSLT =
            BuildMI(MBB, BrI, DL, TII->get(RISCV::SLT), ScratchReg)
                .addReg(RS1)
                .addReg(RS2)
                .getInstr();

        MachineInstr *NewBNE =
            BuildMI(MBB, BrI, DL, TII->get(RISCV::BNE))
                .addReg(ScratchReg)
                .addReg(RISCV::X0)
                .addMBB(OriginalTarget)
                .getInstr();

        LLVM_DEBUG(
            dbgs() << "[branchToBNE][BLT] created"
                  << " NewSLT=" << NewSLT
                  << " NewBNE=" << NewBNE
                  << " TargetMBB=" << OriginalTarget
                  << "\n";
            dbgs() << "[branchToBNE][BLT] new SLT: ";
            NewSLT->print(dbgs());
            dbgs() << "[branchToBNE][BLT] new BNE: ";
            NewBNE->print(dbgs());
            dbgs() << "[branchToBNE][BLT] erasing old branch=" << &Br
                  << "\n";
        );

        Br.eraseFromParent();
        
        return true;
      }

      case RISCV::PseudoBLTUUsingBNE: {
        
            

        LLVM_DEBUG(
            dbgs() << "[branchToBNE][BLTU] converting"
                  << " LR_MBB=" << &MBB
                  << " TargetMBB=" << OriginalTarget
                  << " ScratchReg="
                  << printReg(ScratchReg, MRI->getTargetRegisterInfo())
                  << "\n");

        MachineInstr *NewSLTU =
            BuildMI(MBB, BrI, DL, TII->get(RISCV::SLTU), ScratchReg)
                .addReg(RS1)
                .addReg(RS2)
                .getInstr();

        MachineInstr *NewBNE =
            BuildMI(MBB, BrI, DL, TII->get(RISCV::BNE))
                .addReg(ScratchReg)
                .addReg(RISCV::X0)
                .addMBB(OriginalTarget)
                .getInstr();

        LLVM_DEBUG(
            dbgs() << "[branchToBNE][BLTU] created"
                  << " NewSLTU=" << NewSLTU
                  << " NewBNE=" << NewBNE
                  << " TargetMBB=" << OriginalTarget
                  << "\n";
            dbgs() << "[branchToBNE][BLTU] new SLTU: ";
            NewSLTU->print(dbgs());
            dbgs() << "[branchToBNE][BLTU] new BNE: ";
            NewBNE->print(dbgs());
            dbgs() << "[branchToBNE][BLTU] erasing old branch=" << &Br
                  << "\n";
        );

        Br.eraseFromParent();
        
        return true;
      }

      case RISCV::PseudoBGEUsingBNE: {
        if (!OtherSuccessor) {
          LLVM_DEBUG(
              dbgs() << "[branchToBNE][BGE] no other successor; skipping\n");
          return false;
        }

        
            

        LLVM_DEBUG(
            dbgs() << "[branchToBNE][BGE] converting"
                  << " LR_MBB=" << &MBB
                  << " OriginalTarget=" << OriginalTarget
                  << " OtherSuccessor=" << OtherSuccessor
                  << " ScratchReg="
                  << printReg(ScratchReg, MRI->getTargetRegisterInfo())
                  << "\n");

        MachineInstr *NewSLT =
            BuildMI(MBB, BrI, DL, TII->get(RISCV::SLT), ScratchReg)
                .addReg(RS1)
                .addReg(RS2)
                .getInstr();

        MachineInstr *NewBNE =
            BuildMI(MBB, BrI, DL, TII->get(RISCV::BNE))
                .addReg(ScratchReg)
                .addReg(RISCV::X0)
                .addMBB(OtherSuccessor)
                .getInstr();

        MachineInstr *NewPseudoBR =
            BuildMI(MBB, BrI, DL, TII->get(RISCV::PseudoBR))
                .addMBB(OriginalTarget)
                .getInstr();

        LLVM_DEBUG(
            dbgs() << "[branchToBNE][BGE] created"
                  << " NewSLT=" << NewSLT
                  << " NewBNE=" << NewBNE
                  << " NewPseudoBR=" << NewPseudoBR
                  << "\n";
            dbgs() << "[branchToBNE][BGE] new SLT: ";
            NewSLT->print(dbgs());
            dbgs() << "[branchToBNE][BGE] new BNE: ";
            NewBNE->print(dbgs());
            dbgs() << "[branchToBNE][BGE] new PseudoBR: ";
            NewPseudoBR->print(dbgs());
            dbgs() << "[branchToBNE][BGE] erasing old branch=" << &Br
                  << "\n";
        );

        Br.eraseFromParent();
        
        LLVM_DEBUG(
          dbgs() << "[branchToBNE][BGE] MBB after erasing old branch:\n";
          MBB.print(dbgs());
        );
        return true;
      }

      case RISCV::PseudoBGEUUsingBNE: {
        if (!OtherSuccessor) {
          LLVM_DEBUG(
              dbgs() << "[branchToBNE][BGEU] no other successor; skipping\n");
          return false;
        }

        
            

        LLVM_DEBUG(
            dbgs() << "[branchToBNE][BGEU] converting"
                  << " LR_MBB=" << &MBB
                  << " OriginalTarget=" << OriginalTarget
                  << " OtherSuccessor=" << OtherSuccessor
                  << " ScratchReg="
                  << printReg(ScratchReg, MRI->getTargetRegisterInfo())
                  << "\n");

        MachineInstr *NewSLTU =
            BuildMI(MBB, BrI, DL, TII->get(RISCV::SLTU), ScratchReg)
                .addReg(RS1)
                .addReg(RS2)
                .getInstr();

        MachineInstr *NewBNE =
            BuildMI(MBB, BrI, DL, TII->get(RISCV::BNE))
                .addReg(ScratchReg)
                .addReg(RISCV::X0)
                .addMBB(OtherSuccessor)
                .getInstr();

        MachineInstr *NewPseudoBR =
            BuildMI(MBB, BrI, DL, TII->get(RISCV::PseudoBR))
                .addMBB(OriginalTarget)
                .getInstr();

        LLVM_DEBUG(
            dbgs() << "[branchToBNE][BGEU] created"
                  << " NewSLTU=" << NewSLTU
                  << " NewBNE=" << NewBNE
                  << " NewPseudoBR=" << NewPseudoBR
                  << "\n";
            dbgs() << "[branchToBNE][BGEU] new SLTU: ";
            NewSLTU->print(dbgs());
            dbgs() << "[branchToBNE][BGEU] new BNE: ";
            NewBNE->print(dbgs());
            dbgs() << "[branchToBNE][BGEU] new PseudoBR: ";
            NewPseudoBR->print(dbgs());
            dbgs() << "[branchToBNE][BGEU] erasing old branch=" << &Br
                  << "\n";
        );

        Br.eraseFromParent();
        
        return true;
      }

      default:
        LLVM_DEBUG(
            dbgs() << "[branchToBNE] unhandled opcode="
                  << TII->getName(BrOpcode) << "\n");
        return false;
      }
  }
  else {
    switch (BrOpcode) {
      case RISCV::PseudoBEQUsingBNE: {
        LLVM_DEBUG(
        dbgs() << "[branchToBNE][BEQ] converting to Original"
              << " NON_LR_MBB=" << &MBB
              << " BNETarget=" << OtherSuccessor
              << " PseudoBRTarget=" << OriginalTarget
              << "\n");
        BuildMI(MBB, BrI, DL, TII->get(RISCV::BEQ))
            .addReg(RS1)
            .addReg(RS2)
            .addMBB(OriginalTarget);
        Br.eraseFromParent();
        return true;
        
      }
      case RISCV::PseudoBLTUsingBNE: {
        LLVM_DEBUG(
        dbgs() << "[branchToBNE][BLT] converting to Original"
              << " NON_LR_MBB=" << &MBB
              << " BNETarget=" << OtherSuccessor
              << " PseudoBRTarget=" << OriginalTarget
              << "\n");
        BuildMI(MBB, BrI, DL, TII->get(RISCV::BLT))
            .addReg(RS1)
            .addReg(RS2)
            .addMBB(OriginalTarget);
        Br.eraseFromParent();
        return true;
        
      }
      case RISCV::PseudoBLTUUsingBNE: {
        LLVM_DEBUG(
        dbgs() << "[branchToBNE][BLTU] converting to Original"
              << " NON_LR_MBB=" << &MBB
              << " BNETarget=" << OtherSuccessor
              << " PseudoBRTarget=" << OriginalTarget
              << "\n");
        BuildMI(MBB, BrI, DL, TII->get(RISCV::BLTU))
            .addReg(RS1)
            .addReg(RS2)
            .addMBB(OriginalTarget);
        Br.eraseFromParent();
        return true;
        
      }
      case RISCV::PseudoBGEUsingBNE: {
        LLVM_DEBUG(
        dbgs() << "[branchToBNE][BGE] converting to Original"
              << " NON_LR_MBB=" << &MBB
              << " BNETarget=" << OtherSuccessor
              << " PseudoBRTarget=" << OriginalTarget
              << "\n");
        BuildMI(MBB, BrI, DL, TII->get(RISCV::BGE))
            .addReg(RS1)
            .addReg(RS2)
            .addMBB(OriginalTarget);
        Br.eraseFromParent();
        return true;
        
      }
      case RISCV::PseudoBGEUUsingBNE: {
        LLVM_DEBUG(
        dbgs() << "[branchToBNE][BGEU] converting to Original"
              << " NON_LR_MBB=" << &MBB
              << " BNETarget=" << OtherSuccessor
              << " PseudoBRTarget=" << OriginalTarget
              << "\n");
        BuildMI(MBB, BrI, DL, TII->get(RISCV::BGEU))
            .addReg(RS1)
            .addReg(RS2)
            .addMBB(OriginalTarget);
        Br.eraseFromParent();
        
        return true;
      }
        
      default:
        LLVM_DEBUG(
            dbgs() << "[branchToBNE] unhandled opcode="
                  << TII->getName(BrOpcode) << "\n");
        return false;
    }
  }
  
}

bool RISCVPseudoBNEBranchExpansion::isSCMBB(const MachineBasicBlock &MBB) {
  for (const MachineInstr &MI : MBB)
    if (utils::lrsc::isSC(MI.getOpcode()))
      return true;
  return false;
}

bool RISCVPseudoBNEBranchExpansion::isPseudoBranchOpc(unsigned Opc) {
  LLVM_DEBUG(dbgs() << "[isPseudoBranchOpc] Opc=" << Opc << "\n");
  return Opc == RISCV::BNE
      || Opc == RISCV::BEQ
      || Opc == RISCV::BLT
      || Opc == RISCV::BLTU
      || Opc == RISCV::BGE
      || Opc == RISCV::BGEU
      || Opc == RISCV::PseudoBEQUsingBNE 
      || Opc == RISCV::PseudoBLTUsingBNE
      || Opc == RISCV::PseudoBGEUsingBNE
      || Opc == RISCV::PseudoBLTUUsingBNE
      || Opc == RISCV::PseudoBGEUUsingBNE;
}