//===-- RISCVExpandINLINEASM.cpp - Expand inline asm and updating Machine CFG
//---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass expands supported RISC-V INLINEASM MachineInstrs into ordinary
// target MachineInstrs and explicit MachineBasicBlocks. It reads the inline
// asm template string, decodes the INLINEASM operand-group flags to map
// placeholders such as $0, $1, and $2 to their real MachineOperands, lowers
// parsed instructions with BuildMI, converts internal asm labels into
// MachineBasicBlock targets, and updates CFG successor edges to match emitted
// branches.
//
// In other words, this pass turns an opaque inline-asm block such as:
//
//   INLINEASM "lr.d $0, ($1)\nbne $0, $2, 1f\nsc.d $3, $4, ($1)\n1:",
//             ExtraInfo, Flag0, Reg0, Flag1, Reg1, ...
//
// into real MachineInstrs and MachineBasicBlocks, for example:
//
//   LR_D
//   BNE <target MBB>
//   SC_D
//   BNE <target MBB>
//
// This allows later MachineFunction analyses and transformations to inspect
// LR/SC instructions and control flow that would otherwise remain hidden
// inside INLINEASM.
//
//===----------------------------------------------------------------------===//
#define RISCV_EXPAND_INLINE_ASM_NAME "RISC-V Expand Inline Assembly "
#define DEBUG_TYPE "riscv-expand-inline-asm"

#include "LRSCCountUtils.hpp"
#include "RISCV.h"
#include "RISCVInstrInfo.h"
#include "RISCVTargetMachine.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

using namespace llvm;
using namespace utils;

cl::opt<bool> RISCVEXPANDINLINEASMEMIT(
    "dump-expand-inline-asm-stat", cl::Hidden,
    cl::desc("The stat emission control is used for enabling and disabling the "
             "emission of instruction stats for inline assembly blocks. "),
    cl::init(false));
struct InlineAsmOperand {
  enum OperandKind { OK_Reg, OK_DefReg, OK_Imm };

  OperandKind Kind;
  Register Reg;
  int64_t Imm;
};
struct InlineAsmLineInstrRecord {

  unsigned Opc;
  SmallVector<InlineAsmOperand, 4> Operands;
};
namespace {
class RISCVExpandINLINEASM : public MachineFunctionPass {
public:
  static char ID;

  static std::string ModuleName;

  RISCVExpandINLINEASM() : MachineFunctionPass(ID) {}

  ~RISCVExpandINLINEASM();

  std::string expandInlineAsm(const MachineInstr &MI, const char *AsmStr,
                              const TargetRegisterInfo *TRI);

  bool isBranchMnemonic(StringRef M);

  bool isBranchTargetTok(StringRef Tok);

  bool isBranchOpc(unsigned Opc);

  Register parseRegName(StringRef N);

  int64_t encodeFenceArg(StringRef S);

  InlineAsmOperand parseOperand(StringRef Tok, StringRef Mnemonic,
                                unsigned OpIdx);

  unsigned mnemonicToOpcode(StringRef M);

  bool runOnMachineFunction(MachineFunction &MF) override;

  void copyLiveInsManually(MachineBasicBlock *Dst,
                           const MachineBasicBlock &Src);

  StringRef getPassName() const override {
    return RISCV_EXPAND_INLINE_ASM_NAME;
  }

  void dumpStats(raw_ostream &OS);

  void inlineAsmToMachineInstrsRewireCFG(StringRef AsmStr,
                                         MachineBasicBlock &MBB,
                                         MachineBasicBlock::iterator MBBI,
                                         const TargetInstrInfo *TII);

  void buildInlineAsmLineRecords(StringRef Line);

private:
  SmallVector<InlineAsmLineInstrRecord, 6> InlineAsmInstructionsRecord;

}; // end class RISCVExpandINLINEASM

} // end anonymous namespace

char RISCVExpandINLINEASM::ID = 0;
std::string RISCVExpandINLINEASM::ModuleName = "";

INITIALIZE_PASS(RISCVExpandINLINEASM, "riscv-expand-inline-asm",
                RISCV_EXPAND_INLINE_ASM_NAME, false, false)

/*--------------------------------------------------------------------------*/
/* createRISCVExpandINLINEASMPass:
   Factory function that allocates and returns a new instance of the
   RISCVExpandINLINEASM MachineFunctionPass.
   - Returns a heap-allocated pass object registered with the LLVM pass
     manager infrastructure.
   - Called by the RISC-V target to install this pass into the code-gen
     pipeline. */
FunctionPass *llvm::createRISCVExpandINLINEASMPass() {
  return new RISCVExpandINLINEASM();
}

/*--------------------------------------------------------------------------*/
/* ~RISCVExpandINLINEASM (destructor):
   Destructor for the RISCVExpandINLINEASM pass.
   - If the -dump-expand-inline-asm-stat command-line flag is set, emits
     the collected per-module instruction statistics to the debug stream
     before the pass object is destroyed.
   - Ensures stats are flushed even when the pass is torn down after the
     last MachineFunction has been processed. */
RISCVExpandINLINEASM::~RISCVExpandINLINEASM() {
  if (RISCVEXPANDINLINEASMEMIT) {
    dumpStats(dbgs());
  }
} // end destructor

/*--------------------------------------------------------------------------*/
/* runOnMachineFunction:
   Entry point for the RISCVExpandINLINEASM MachineFunctionPass.
   - MF: The MachineFunction being compiled.
   Iterates over every MachineBasicBlock and every MachineInstr inside it.
   When an INLINEASM MachineInstr is found, reads its raw asm template string
   from operand 0, calls expandInlineAsm() to substitute register and
   immediate placeholders, then calls inlineAsmToMachineInstrsRewireCFG() to
   lower the expanded text into real MachineInstrs and update the CFG.
   Stops after processing the first INLINEASM encountered in each basic block
   (inner break) and always returns true to indicate the function was
   modified. */
bool RISCVExpandINLINEASM::runOnMachineFunction(MachineFunction &MF) {
  LLVM_DEBUG(
      dbgs() << "\n=== RISCVExpandINLINEASM runOnMachineFunction ENTER ===\n");
  LLVM_DEBUG(dbgs() << "[run] Function: " << MF.getName() << "\n");
  LLVM_DEBUG(dbgs() << "[run] Initial MBB count: " << MF.size() << "\n");

  llvm::Module *m = MF.getFunction().getParent();
  RISCVExpandINLINEASM::ModuleName = m->getModuleIdentifier();
  LLVM_DEBUG(dbgs() << "[run] Module: " << RISCVExpandINLINEASM::ModuleName
                    << "\n");
  for (auto &MBB : MF) {
    LLVM_DEBUG(dbgs() << "[run] Visiting MBB #" << MBB.getNumber() << " size="
                      << MBB.size() << " succs=" << MBB.succ_size() << "\n");
    const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
    const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
    LLVM_DEBUG(dbgs() << "[run] TRI=" << TRI << " TII=" << TII << "\n");
    for (auto MBBI = MBB.begin(), E = MBB.end(); MBBI != E; ++MBBI) {
      LLVM_DEBUG(dbgs() << "[run]   MI opcode=" << MBBI->getOpcode()
                        << " numOperands=" << MBBI->getNumOperands()
                        << " isInlineAsm=" << MBBI->isInlineAsm() << "\n");
      if (MBBI->isInlineAsm()) {
        LLVM_DEBUG(dbgs() << "[run] >>> Found INLINEASM in MBB #"
                          << MBB.getNumber() << "\n");
        LLVM_DEBUG(dbgs() << "[run] INLINEASM MachineInstr dump follows:\n");
        LLVM_DEBUG(MBBI->dump());
        DebugLoc DL = MBBI->getDebugLoc();
        LLVM_DEBUG(dbgs() << "[run] DebugLoc valid=" << bool(DL) << "\n");
        // Process inline assembly instruction
        LLVM_DEBUG(
            dbgs()
            << "[run] About to read asm string from operand 0. numOperands="
            << MBBI->getNumOperands() << "\n");
        const char *AsmStr = MBBI->getOperand(0).getSymbolName();

        LLVM_DEBUG(dbgs() << "Found inline assembly: " << AsmStr << "\n");

        LLVM_DEBUG(dbgs() << "[run] About to call expandInlineAsm()\n");
        std::string ExpandedAssemblyStr = expandInlineAsm(*MBBI, AsmStr, TRI);
        LLVM_DEBUG(
            dbgs() << "[run] Returned from expandInlineAsm(), expanded length="
                   << ExpandedAssemblyStr.size() << "\n");
        LLVM_DEBUG(dbgs() << "[run] Expanded asm:\n"
                          << ExpandedAssemblyStr << "\n");

        llvm::StringRef ExpandedAssemblyStrRef(ExpandedAssemblyStr);

        LLVM_DEBUG(
            dbgs()
            << "[run] About to call inlineAsmToMachineInstrsRewireCFG()\n");
        inlineAsmToMachineInstrsRewireCFG(ExpandedAssemblyStrRef, MBB, MBBI,
                                          TII);
        LLVM_DEBUG(
            dbgs()
            << "[run] Returned from inlineAsmToMachineInstrsRewireCFG()\n");

        break;
      }

    } // end inner loop over instructions in basic block

  } // end outer loop over MachineBasicBlocks
  LLVM_DEBUG(dbgs() << "[run] Final MBB count: " << MF.size() << "\n");
  LLVM_DEBUG(
      dbgs() << "=== RISCVExpandINLINEASM runOnMachineFunction EXIT ===\n");
  return true;
} // end runOnMachineFunction

/*--------------------------------------------------------------------------*/
/* stringifyOperand:
   Converts a single MachineOperand to its textual asm representation.
   - MO:  The MachineOperand to stringify.
   - TRI: Target register info used to look up canonical asm register names
          (e.g. "a3", "t0", "zero").
   Handles the following operand kinds in order:
     1. Register   - emits the target asm name via TRI->getRegAsmName(), or
                     "zero" for the null register.
     2. Immediate  - emits the decimal integer value.
     3. Global     - emits the GlobalValue's symbol name.
     4. Symbol     - emits the external symbol name directly.
     5. BlockAddress - emits the IR BasicBlock name, or "label" if unnamed.
     6. CImm       - emits the sign-extended 64-bit value of a wide constant.
     7. Other      - emits "0" as a safe fallback for unsupported kinds.
   Returns the stringified operand text, which is later substituted into the
   expanded asm template in place of $N placeholders. */
std::string stringifyOperand(const MachineOperand &MO,
                             const TargetRegisterInfo *TRI) {
  LLVM_DEBUG(dbgs() << "[stringifyOperand] ENTER, TRI=" << TRI << "\n";
             MO.print(dbgs(), TRI); dbgs() << "\n");

  LLVM_DEBUG(dbgs() << "[stringifyOperand] operand dump: "; MO.print(dbgs());
             dbgs() << "\n";);
  // Possibility 1: register operand.
  if (MO.isReg()) {
    Register R = MO.getReg();
    LLVM_DEBUG(dbgs() << "[stringifyOperand] isReg, reg=" << R << "\n");

    if (!R) {
      return "zero";
    }

    // print real target asm name like "a3", "t0", "zero".
    LLVM_DEBUG(dbgs() << "[stringifyOperand] asm reg name="
                      << TRI->getRegAsmName(R) << "\n");
    return TRI->getRegAsmName(R).str();
  }

  // Possibility 2: immediate integer.
  if (MO.isImm()) {
    LLVM_DEBUG(dbgs() << "[stringifyOperand] isImm, imm=" << MO.getImm()
                      << "\n");
    return std::to_string(MO.getImm());
  }

  // Possibility 3: global address, such as a function or variable symbol.
  if (MO.isGlobal()) {
    return MO.getGlobal()->getName().str();
  }

  // Possibility 4: external symbol.
  if (MO.isSymbol()) {
    return MO.getSymbolName();
  }

  // Possibility 5: block address.
  if (MO.isBlockAddress()) {
    std::string Name = MO.getBlockAddress()->getBasicBlock()->getName().str();

    if (Name.empty()) {
      return "label";
    }

    return Name;
  }

  // Possibility 6: wide constant immediate.
  if (MO.isCImm()) {
    return std::to_string(MO.getCImm()->getSExtValue());
  }

  // Possibility 7: anything else.
  LLVM_DEBUG(
      dbgs() << "[stringifyOperand] unsupported operand kind, returning 0\n");
  return "0";
}

/*--------------------------------------------------------------------------*/
/* expandInlineAsm:
   Expands an INLINEASM MachineInstr's template string by substituting every
   $N / ${N} / ${N:modifier} placeholder with the stringified text of the
   corresponding MachineOperand.
   - MI:     The INLINEASM MachineInstr whose operand list is decoded.
   - AsmStr: The raw asm template string extracted from MI operand 0.
   - TRI:    Target register info forwarded to stringifyOperand().

   Phase 1 - Operand decoding:
     Walks the MI operand list starting at MIOp_FirstOperand. Each group
     begins with an InlineAsm::Flag immediate that encodes the kind
     (RegUse, RegDef, RegDefEarlyClobber, Clobber, Imm, Mem, Func) and the
     number of associated operand slots. Each group is stringified into a
     single entry in OpStrings[].

   Phase 2 - Template substitution:
     Scans AsmStr character by character. Handles:
       "$$"      -> literal '$'
       "$N"      -> OpStrings[N]
       "${N}"    -> OpStrings[N]
       "${N:z}"  -> "zero" if OpStrings[N] == "0", else OpStrings[N]
     Out-of-range placeholder indices emit "0" rather than crashing.

   Returns the fully expanded asm string ready for line-by-line parsing. */
std::string RISCVExpandINLINEASM::expandInlineAsm(
    const MachineInstr &MI, const char *AsmStr, const TargetRegisterInfo *TRI) {
  LLVM_DEBUG(dbgs() << "\n[expand] ENTER expandInlineAsm\n");
  LLVM_DEBUG(dbgs() << "[expand] MI numOperands=" << MI.getNumOperands()
                    << " TRI=" << TRI << "\n");
  LLVM_DEBUG(dbgs() << "[expand] raw asm string:\n" << AsmStr << "\n");
  // ============================================================
  // PHASE 1: walk the MI operand list and build OpStrings.
  // ============================================================

  std::vector<std::string> OpStrings;

  unsigned OpPointer = InlineAsm::MIOp_FirstOperand;

  while (OpPointer < MI.getNumOperands()) {
    LLVM_DEBUG(dbgs() << "[expand][phase1] OpPointer=" << OpPointer << " / "
                      << MI.getNumOperands() << "\n");
    const MachineOperand &FlagMO = MI.getOperand(OpPointer);
    LLVM_DEBUG(dbgs() << "[expand][phase1] FlagMO: "; FlagMO.print(dbgs());
               dbgs() << "\n";);

    // The first slot of every operand group MUST be an immediate flag.
    // If it is not, stop .
    if (!FlagMO.isImm()) {
      LLVM_DEBUG(dbgs() << "[expand][phase1] FlagMO is not immediate, break\n");
      break;
    }

    const InlineAsm::Flag F(FlagMO.getImm());
    unsigned FlagRelatedOperandCount = F.getNumOperandRegisters();
    LLVM_DEBUG(dbgs() << "[expand][phase1] rawFlag=" << FlagMO.getImm()
                      << " relatedOperandCount=" << FlagRelatedOperandCount
                      << " isMem=" << F.isMemKind() << " isRegUse="
                      << F.isRegUseKind() << " isRegDef=" << F.isRegDefKind()
                      << " isRegDefEC=" << F.isRegDefEarlyClobberKind()
                      << " isClobber=" << F.isClobberKind() << " isImm="
                      << F.isImmKind() << " isFunc=" << F.isFuncKind() << "\n");

    std::string StringifiedOp;

    // ============================================================
    // Possible operand-group KINDS the flag can describe:
    //
    //   RegUse              "r" input register
    //   RegDef              "=r" output register
    //   RegDefEarlyClobber  "=&r" early-clobber output
    //   Clobber             clobber list entry
    //   Imm                 "i" / "n" immediate
    //   Mem                 "m" memory operand
    //   Func                "X" / "s" function or symbolic operand
    //
    // ============================================================

    // ----------------------------------------------------------------
    // Possibility 1: MEMORY operand.
    //
    // Slot layout:
    //   OpPointer       : Flag
    //   OpPointer + 1   : base register
    //   OpPointer + 2   : offset, usually an immediate
    //
    // We format it as "offset(base)".
    // ----------------------------------------------------------------
    if (F.isMemKind() && FlagRelatedOperandCount >= 2) {
      LLVM_DEBUG(dbgs() << "[expand][phase1] MEMORY group: flag=" << OpPointer
                        << " base=" << (OpPointer + 1)
                        << " off=" << (OpPointer + 2) << "\n");
      const MachineOperand &BaseMO = MI.getOperand(OpPointer + 1);
      const MachineOperand &OffMO = MI.getOperand(OpPointer + 2);

      std::string Base = stringifyOperand(BaseMO, TRI);
      std::string Off;

      if (F.getMemoryConstraintID() == InlineAsm::ConstraintCode::A) {
        StringifiedOp = "(" + Base + ")";
      } else if (OffMO.isImm()) {
        Off = std::to_string(OffMO.getImm());
      } else if (OffMO.isGlobal()) {
        Off = OffMO.getGlobal()->getName().str();
      } else if (OffMO.isSymbol()) {
        Off = OffMO.getSymbolName();
      } else if (OffMO.isReg() && OffMO.getReg()) {
        Off = stringifyOperand(OffMO, TRI);
      } else {
        Off = "0";
      }

      StringifiedOp = Off + "(" + Base + ")";
    }

    // ----------------------------------------------------------------
    // Possibility 2: REGISTER operand.
    //
    // Slot layout:
    //   OpPointer                : Flag
    //   OpPointer + 1 .. + FlagRelatedOperandCount: register operand(s)
    //
    // FlagRelatedOperandCount == 1:
    //   simple register
    //
    // FlagRelatedOperandCount > 1:
    //   register tuple/group, comma-joined here
    // ----------------------------------------------------------------
    else if (F.isRegUseKind() || F.isRegDefKind() ||
             F.isRegDefEarlyClobberKind() || F.isClobberKind()) {
      LLVM_DEBUG(dbgs() << "[expand][phase1] REGISTER group: flag=" << OpPointer
                        << " count=" << FlagRelatedOperandCount << "\n");
      if (FlagRelatedOperandCount == 1) {
        StringifiedOp = stringifyOperand(MI.getOperand(OpPointer + 1), TRI);
      } else {
        for (unsigned K = 0; K < FlagRelatedOperandCount; ++K) {
          if (K) {
            StringifiedOp += ",";
          }
          StringifiedOp +=
              stringifyOperand(MI.getOperand(OpPointer + 1 + K), TRI);
        }
      }
    }

    // ----------------------------------------------------------------
    // Possibility 3: IMMEDIATE operand.
    //
    // Slot layout:
    //   OpPointer     : Flag
    //   OpPointer + 1 : immediate or symbol-like value
    // ----------------------------------------------------------------
    else if (F.isImmKind()) {
      LLVM_DEBUG(dbgs() << "[expand][phase1] IMMEDIATE group: value index="
                        << (OpPointer + 1) << "\n");
      const MachineOperand &ValMO = MI.getOperand(OpPointer + 1);
      StringifiedOp = stringifyOperand(ValMO, TRI);
    }

    // ----------------------------------------------------------------
    // Possibility 4: FUNCTION / SYMBOLIC operand ("X" or "s" constraint).
    //
    // Slot layout:
    //   OpPointer     : Flag (kind = Func, FlagRelatedOperandCount = 1)
    //   OpPointer + 1 : the symbol, usually a Global, sometimes Symbol or
    //              BlockAddress.
    //
    // Emits the symbol's name verbatim.
    // ----------------------------------------------------------------
    else if (F.isFuncKind()) {
      LLVM_DEBUG(dbgs() << "[expand][phase1] FUNC group: value index="
                        << (OpPointer + 1) << "\n");
      const MachineOperand &ValMO = MI.getOperand(OpPointer + 1);

      if (ValMO.isGlobal()) {
        StringifiedOp = ValMO.getGlobal()->getName().str();
      } else if (ValMO.isSymbol()) {
        // External symbol with no IR-level GlobalValue.
        StringifiedOp = ValMO.getSymbolName();
      } else if (ValMO.isBlockAddress()) {
        // Block address, used by computed gotos.
        StringifiedOp =
            ValMO.getBlockAddress()->getBasicBlock()->getName().str();

        if (StringifiedOp.empty()) {
          StringifiedOp = "label";
        }
      } else {
        // Unexpected operand kind for a Func slot.
        StringifiedOp = stringifyOperand(ValMO, TRI);
      }
    }

    // ----------------------------------------------------------------
    // Possibility D: unknown
    //
    // ----------------------------------------------------------------
    else {
      if (FlagRelatedOperandCount >= 1) {
        const MachineOperand &ValMO = MI.getOperand(OpPointer + 1);
        StringifiedOp = stringifyOperand(ValMO, TRI);
      } else {
        StringifiedOp = "0";
      }
    }

    LLVM_DEBUG(dbgs() << "[expand][phase1] push OpStrings[" << OpStrings.size()
                      << "]='" << StringifiedOp << "'\n");
    OpStrings.push_back(StringifiedOp);
    OpPointer += 1 + FlagRelatedOperandCount;
    LLVM_DEBUG(dbgs() << "[expand][phase1] next OpPointer=" << OpPointer
                      << "\n");
  }
  LLVM_DEBUG(dbgs() << "[expand][phase1] total OpStrings=" << OpStrings.size()
                    << "\n");

  // ============================================================
  // PHASE 2: walk the asm string, substituting placeholders.
  // ============================================================

  std::string ExpandedAsm;
  const size_t StringLength = std::strlen(AsmStr);
  LLVM_DEBUG(dbgs() << "[expand][phase2] StringLength=" << StringLength
                    << "\n");

  for (size_t StringIndex = 0; StringIndex < StringLength;) {
    // ----------------------------------------------------------------
    // Case 1: "$$" means escaped dollar sign.
    //
    // Example:
    //   "li $0, $$5"
    //
    // becomes:
    //   "li a0, $5"
    // ----------------------------------------------------------------
    if (AsmStr[StringIndex] == '$' && StringIndex + 1 < StringLength &&
        AsmStr[StringIndex + 1] == '$') {
      ExpandedAsm += '$';
      StringIndex += 2;
      continue;
    }

    // ----------------------------------------------------------------
    // Case 2: "$" may start an inline-asm operand placeholder.
    //
    // Supported forms:
    //   $0
    //   $12
    //   ${0}
    //   ${3:z}
    //   ${4:z}
    // ----------------------------------------------------------------
    if (AsmStr[StringIndex] == '$' && StringIndex + 1 < StringLength) {

      size_t ScanIndex = StringIndex + 1;
      bool HasBraces = false;
      char OperandModifier = '\0';

      // Brace form: ${3:z}
      if (AsmStr[ScanIndex] == '{') {
        HasBraces = true;
        ++ScanIndex;
      }

      // Read the operand number.
      //
      // Example:
      //   "$12"    -> OperandNumberText = "12"
      //   "${3:z}" -> OperandNumberText = "3"
      size_t OperandNumberStart = ScanIndex;

      while (ScanIndex < StringLength &&
             std::isdigit(static_cast<unsigned char>(AsmStr[ScanIndex]))) {
        ++ScanIndex;
      }

      // No digits after "$" or "${" means this is not a placeholder.
      //
      // Example:
      //   "$x"
      //
      // Copy the "$" literally.
      if (ScanIndex == OperandNumberStart) {
        ExpandedAsm += AsmStr[StringIndex];
        ++StringIndex;
        continue;
      }

      int OperandNumber = std::stoi(std::string(
          AsmStr + OperandNumberStart, ScanIndex - OperandNumberStart));
      LLVM_DEBUG(dbgs() << "[expand][phase2] placeholder at char "
                        << StringIndex << " operandNumber=" << OperandNumber
                        << " hasBraces=" << HasBraces << "\n");

      // Read optional modifier.
      //
      // Example:
      //   "${3:z}"
      //
      // OperandModifier becomes 'z'.
      if (HasBraces && ScanIndex < StringLength && AsmStr[ScanIndex] == ':') {
        ++ScanIndex; // skip ':'

        if (ScanIndex < StringLength && AsmStr[ScanIndex] != '}')
          OperandModifier = AsmStr[ScanIndex];

        // Skip the rest of the modifier until closing brace.
        while (ScanIndex < StringLength && AsmStr[ScanIndex] != '}') {
          ++ScanIndex;
        }
      }

      // Consume closing brace.
      if (HasBraces && ScanIndex < StringLength && AsmStr[ScanIndex] == '}') {
        ++ScanIndex;
      }

      // Substitute operand string.
      if (OperandNumber >= 0 &&
          OperandNumber < static_cast<int>(OpStrings.size())) {
        const std::string &OperandText = OpStrings[OperandNumber];
        LLVM_DEBUG(dbgs() << "[expand][phase2] substituting operand "
                          << OperandNumber << " modifier='" << OperandModifier
                          << "' text='" << OperandText << "'\n");

        // RISC-V z modifier:
        //
        //   ${3:z}
        //
        // If operand 3 stringifies to immediate "0", print the zero register.
        //
        // Example:
        //   bne $0, ${3:z}, 1f
        //
        // If:
        //   OpStrings[0] = "a2"
        //   OpStrings[3] = "0"
        //
        // Output:
        //   bne a2, zero, 1f
        if (OperandModifier == 'z' && OperandText == "0") {
          ExpandedAsm += "zero";
        } else {
          ExpandedAsm += OperandText;
        }

      } else {
        // Bad placeholder index. Avoid crashing.
        LLVM_DEBUG(dbgs() << "[expand][phase2] BAD placeholder index "
                          << OperandNumber << " OpStrings.size="
                          << OpStrings.size() << ", substituting 0\n");
        ExpandedAsm += "0";
      }

      StringIndex = ScanIndex;
      continue;
    }

    // ----------------------------------------------------------------
    // Case 3: normal character.
    //
    // Preserve normal instruction text, labels, tabs, and newlines.
    // ----------------------------------------------------------------
    ExpandedAsm += AsmStr[StringIndex];
    ++StringIndex;
  }

  LLVM_DEBUG(dbgs() << "[expand] final ExpandedAsm:\n" << ExpandedAsm << "\n");
  LLVM_DEBUG(dbgs() << "[expand] EXIT expandInlineAsm\n");
  return ExpandedAsm;
}

/*--------------------------------------------------------------------------*/
/* inlineAsmToMachineInstrsRewireCFG:
   Lowers a fully-expanded inline asm string into real MachineInstrs and
   rewires the MachineFunction CFG to match the emitted control flow.
   - AsmStr: Expanded asm text (one instruction per line, placeholders already
             substituted by expandInlineAsm).
   - MBB:    The MachineBasicBlock that originally contained the INLINEASM MI.
   - MBBI:   Iterator pointing at the INLINEASM MI inside MBB.
   - TII:    Target instruction info used with BuildMI.

   Steps:
     1. Splits AsmStr on newlines and calls buildInlineAsmLineRecords() for
        each non-empty line to populate InlineAsmInstructionsRecord.
     2. Returns early (leaving INLINEASM untouched) if no records were parsed
        or if the record set does not contain both an LR and an SC instruction.
     3. Detects whether the LR/SC pair is separated by a conditional branch
        (IsConditional). When conditional, three new MBBs are created:
          LR_MBB  - holds the LR instruction and the value-comparison branch.
          SC_MBB  - holds the SC instruction and the retry branch.
          TailMBB - receives everything that followed INLINEASM in MBB.
        When unconditional (simple retry loop), only TailMBB is created and
        the branch target loops back to MBB itself.
     4. Copies original MBB live-ins and inline-asm physical registers as
        live-ins to each new MBB so register liveness remains correct after
        the split.
     5. Splices instructions that followed INLINEASM into TailMBB, transfers
        MBB's original CFG successors to TailMBB, and erases the INLINEASM MI.
     6. Emits each InlineAsmLineInstrRecord into the appropriate MBB via
        BuildMI, resolving branch target MBBs from CFG context rather than
        from asm label names.
     7. Adds CFG successor edges:
          Conditional:   MBB->LR_MBB, LR_MBB->SC_MBB, LR_MBB->TailMBB,
                         SC_MBB->LR_MBB, SC_MBB->TailMBB.
          Unconditional: MBB->MBB (retry self-loop), MBB->TailMBB. */
void RISCVExpandINLINEASM::inlineAsmToMachineInstrsRewireCFG(
    StringRef AsmStr, MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
    const TargetInstrInfo *TII) {

  LLVM_DEBUG(dbgs() << "\n[rewire] ENTER inlineAsmToMachineInstrsRewireCFG\n");
  LLVM_DEBUG(dbgs() << "[rewire] source MBB #" << MBB.getNumber()
                    << " size=" << MBB.size() << " succs=" << MBB.succ_size()
                    << " TII=" << TII << "\n");
  LLVM_DEBUG(dbgs() << "[rewire] AsmStr length=" << AsmStr.size() << "\n");
  LLVM_DEBUG(dbgs() << "[rewire] AsmStr:\n" << AsmStr << "\n");

  InlineAsmInstructionsRecord.clear();

  LLVM_DEBUG(dbgs() << "[rewire] Cleared InlineAsmInstructionsRecord\n");

  SmallVector<StringRef, 16> Lines;
  AsmStr.split(Lines, '\n');

  LLVM_DEBUG(dbgs() << "[rewire] split line count=" << Lines.size() << "\n");

  for (StringRef Line : Lines) {
    LLVM_DEBUG(dbgs() << "[rewire] raw line='" << Line << "'\n");

    Line = Line.trim();

    LLVM_DEBUG(dbgs() << "[rewire] trimmed line='" << Line << "'\n");

    if (Line.empty()) {
      LLVM_DEBUG(dbgs() << "[rewire] empty line, skip\n");
      continue;
    }

    LLVM_DEBUG(
        dbgs() << "[rewire] About to call buildInlineAsmLineRecords()\n");
    buildInlineAsmLineRecords(Line);
    LLVM_DEBUG(
        dbgs() << "[rewire] returned from buildInlineAsmLineRecords(), records="
               << InlineAsmInstructionsRecord.size() << "\n");
  }

  if (InlineAsmInstructionsRecord.empty()) {
    LLVM_DEBUG(
        dbgs()
        << "[rewire][unsupported] no supported inline asm records were parsed; "
        << "leaving original INLINEASM unchanged\n");
    return;
  }

  bool HasLR = false;
  bool HasSC = false;
  bool IsConditional = false;

  for (unsigned I = 0; I < InlineAsmInstructionsRecord.size(); ++I) {
    const InlineAsmLineInstrRecord &Record = InlineAsmInstructionsRecord[I];

    if (lrsc::isLR(Record.Opc)) {
      HasLR = true;

      unsigned J = I;
      while (J + 1 < InlineAsmInstructionsRecord.size()) {
        const InlineAsmLineInstrRecord &NextRecord =
            InlineAsmInstructionsRecord[J + 1];

        if (isBranchOpc(NextRecord.Opc)) {
          IsConditional = true;
          break;
        } else if (lrsc::isSC(NextRecord.Opc)) {
          IsConditional = false;
          break;
        }

        ++J;
      }
    }

    if (lrsc::isSC(Record.Opc))
      HasSC = true;
  }

  LLVM_DEBUG(dbgs() << " HasLR=" << HasLR << " HasSC=" << HasSC << "\n");

  if (!HasLR || !HasSC) {
    LLVM_DEBUG(dbgs() << "[rewire][unsupported] parsed records do not contain "
                         "both LR and SC; "
                      << "leaving original INLINEASM unchanged\n");
    return;
  }

  MachineFunction *MF = MBB.getParent();
  DebugLoc DL = MBBI->getDebugLoc();
  const BasicBlock *LLVM_BB = MBB.getBasicBlock();
  const TargetRegisterInfo *TRI = MF->getSubtarget().getRegisterInfo();

  LLVM_DEBUG(dbgs() << "[rewire] MF='" << MF->getName()
                    << "' LLVM_BB=" << LLVM_BB << " TRI=" << TRI << "\n");

  // ------------------------------------------------------------
  // Collect physical registers used by instructions AFTER INLINEASM.
  //
  // These regs may not be live-ins of the original MBB because they may
  // have been defined earlier in the same MBB before the INLINEASM.
  //
  // Example before split:
  //
  //   bb.old:
  //     $x10 = ...
  //     INLINEASM
  //     $x19 = ADDI $x10, 0
  //
  // After split, $x10 crosses new MBB boundaries, so it must be live-in
  // to LR_MBB, SC_MBB, and TailMBB.
  // ------------------------------------------------------------
  SmallVector<Register, 16> TailUsedPhysRegs;

  for (auto It = std::next(MBBI), End = MBB.end(); It != End; ++It) {
    LLVM_DEBUG({
      dbgs() << "[rewire] scanning tail MI for physreg uses: ";
      It->dump();
    });

    for (const MachineOperand &MO : It->operands()) {
      if (!MO.isReg())
        continue;

      Register R = MO.getReg();

      if (!R)
        continue;

      if (!R.isPhysical())
        continue;

      if (MO.isDef())
        continue;

      if (llvm::is_contained(TailUsedPhysRegs, R))
        continue;

      TailUsedPhysRegs.push_back(R);

      LLVM_DEBUG(
          dbgs() << "[rewire] tail instruction after INLINEASM uses physreg: "
                 << printReg(R, TRI) << "\n");
    }
  }

  // Create three new blocks: LR, SC, and Tail.
  MachineBasicBlock *LR_MBB = nullptr;
  MachineBasicBlock *SC_MBB = nullptr;
  MachineBasicBlock *TailMBB = MF->CreateMachineBasicBlock(LLVM_BB);

  MachineFunction::iterator InsertPt = std::next(MBB.getIterator());

  if (IsConditional) {
    LR_MBB = MF->CreateMachineBasicBlock(LLVM_BB);
    SC_MBB = MF->CreateMachineBasicBlock(LLVM_BB);

    MF->insert(InsertPt, LR_MBB);
    MF->insert(std::next(LR_MBB->getIterator()), SC_MBB);
    MF->insert(std::next(SC_MBB->getIterator()), TailMBB);
  } else {
    MF->insert(InsertPt, TailMBB);
  }

  LLVM_DEBUG(dbgs() << "[rewire] created LR_MBB=" << LR_MBB
                    << " SC_MBB=" << SC_MBB << " TailMBB=" << TailMBB << "\n");

  // ------------------------------------------------------------
  // Copy original live-ins manually.
  // ------------------------------------------------------------
  for (const MachineBasicBlock::RegisterMaskPair &LI : MBB.liveins()) {
    if (IsConditional) {
      if (!LR_MBB->isLiveIn(LI.PhysReg)) {
        LR_MBB->addLiveIn(LI.PhysReg, LI.LaneMask);
      }

      if (!SC_MBB->isLiveIn(LI.PhysReg)) {
        SC_MBB->addLiveIn(LI.PhysReg, LI.LaneMask);
      }
    }

    if (!TailMBB->isLiveIn(LI.PhysReg))
      TailMBB->addLiveIn(LI.PhysReg, LI.LaneMask);

    LLVM_DEBUG(dbgs() << "[rewire] copied original live-in to new blocks: "
                      << printReg(LI.PhysReg, TRI) << "\n");
  }

  // ------------------------------------------------------------
  // Add parsed inline-asm physical registers as live-ins conservatively.
  // ------------------------------------------------------------
  for (const InlineAsmLineInstrRecord &Record : InlineAsmInstructionsRecord) {
    for (const InlineAsmOperand &Op : Record.Operands) {
      LLVM_DEBUG({
        dbgs() << "[rewire][livein-check] Op.Reg=" << printReg(Op.Reg, TRI)
               << " Kind=" << Op.Kind << "\n";
      });
      if ((Op.Kind == InlineAsmOperand::OK_Reg ||
           Op.Kind == InlineAsmOperand::OK_DefReg) &&
          Op.Reg.isPhysical()) {

        if (IsConditional) {
          if (!LR_MBB->isLiveIn(Op.Reg)) {
            LR_MBB->addLiveIn(Op.Reg);
            LLVM_DEBUG(dbgs()
                       << "[rewire] added parsed physreg live-in to LR_MBB: "
                       << printReg(Op.Reg, TRI) << "\n");
          }

          if (!SC_MBB->isLiveIn(Op.Reg)) {
            SC_MBB->addLiveIn(Op.Reg);
            LLVM_DEBUG(dbgs()
                       << "[rewire] added parsed physreg live-in to SC_MBB: "
                       << printReg(Op.Reg, TRI) << "\n");
          }
        }

        if (!MBB.isLiveIn(Op.Reg)) {
          MBB.addLiveIn(Op.Reg);
          LLVM_DEBUG(
              dbgs()
              << "[rewire] added parsed physreg live-in to original MBB: "
              << printReg(Op.Reg, TRI) << "\n");
        }

        if (!TailMBB->isLiveIn(Op.Reg))
          TailMBB->addLiveIn(Op.Reg);

        LLVM_DEBUG(
            dbgs() << "[rewire] added parsed inline-asm physreg live-in: "
                   << printReg(Op.Reg, TRI) << "\n");
      }
    }
  }

  // ------------------------------------------------------------
  // Add physical registers used by moved tail instructions as live-ins.
  // This should catch cases like:
  //
  //   $x19 = ADDI $x10, 0
  //
  // where $x10 was defined before the INLINEASM in the original MBB.
  // ------------------------------------------------------------
  for (Register R : TailUsedPhysRegs) {
    if (IsConditional) {
      if (!LR_MBB->isLiveIn(R)) {
        LR_MBB->addLiveIn(R);
      }

      if (!SC_MBB->isLiveIn(R)) {
        SC_MBB->addLiveIn(R);
      }
    }

    if (!TailMBB->isLiveIn(R))
      TailMBB->addLiveIn(R);

    LLVM_DEBUG(
        dbgs() << "[rewire] added tail-used physreg live-in to LR/SC/Tail: "
               << printReg(R, TRI) << "\n");
  }

  // Move everything AFTER the INLINEASM into TailMBB.
  // This handles instructions after the inline asm that were originally
  // in the same MBB.
  LLVM_DEBUG(dbgs() << "[rewire] About to splice instructions after INLINEASM "
                       "into TailMBB\n");

  TailMBB->splice(TailMBB->end(), &MBB, std::next(MBBI), MBB.end());

  LLVM_DEBUG(
      dbgs() << "[rewire] spliced instructions after INLINEASM into TailMBB\n");

  // TailMBB inherits MBB's original successors.
  LLVM_DEBUG(dbgs() << "[rewire] About to transfer successors from original "
                       "MBB to TailMBB\n");

  TailMBB->transferSuccessorsAndUpdatePHIs(&MBB);

  LLVM_DEBUG(
      dbgs()
      << "[rewire] transferred original successors from MBB to TailMBB\n");

  // Erase the original INLINEASM. Everything before it stays in MBB.
  LLVM_DEBUG(dbgs() << "[rewire] About to erase original INLINEASM\n");

  MBB.erase(MBBI);

  LLVM_DEBUG(dbgs() << "[rewire] erased original INLINEASM\n");
  if (IsConditional) {
    // Original MBB now branches to LR_MBB.
    LLVM_DEBUG(
        dbgs()
        << "[rewire] About to insert PseudoBR from original MBB to LR_MBB\n");

    BuildMI(&MBB, DebugLoc(), TII->get(RISCV::PseudoBR)).addMBB(LR_MBB);

    LLVM_DEBUG(
        dbgs() << "[rewire] inserted PseudoBR from original MBB to LR_MBB\n");

    // Now emit parsed instructions into LR_MBB, SC_MBB, or TailMBB.
  }

  bool SeenSC = false;

  for (const InlineAsmLineInstrRecord &Record : InlineAsmInstructionsRecord) {
    if (lrsc::isSC(Record.Opc))
      SeenSC = true;

    MachineBasicBlock *DstMBB = nullptr;

    if (IsConditional) {
      DstMBB = SeenSC ? SC_MBB : LR_MBB;
    } else {
      DstMBB = &MBB;
    }

    if (Record.Opc == RISCV::FENCE) {
      LLVM_DEBUG(dbgs() << "[rewire] routing FENCE to beginning of TailMBB\n");
      DstMBB = TailMBB;
    }

    LLVM_DEBUG(dbgs() << "[rewire] About to BuildMI opcode=" << Record.Opc
                      << " DstMBB #" << DstMBB->getNumber()
                      << " SeenSC=" << SeenSC << "\n");

    MachineInstrBuilder MIB =
        (Record.Opc == RISCV::FENCE)
            ? BuildMI(*TailMBB, TailMBB->begin(), DL, TII->get(Record.Opc))
            : BuildMI(*DstMBB, DstMBB->end(), DL, TII->get(Record.Opc));

    LLVM_DEBUG(dbgs() << "[rewire] About to add operands for opcode="
                      << Record.Opc << "\n");

    for (const InlineAsmOperand &Op : Record.Operands) {
      switch (Op.Kind) {
      case InlineAsmOperand::OK_Reg:
        MIB.addReg(Op.Reg);
        LLVM_DEBUG(dbgs() << "[rewire]   addReg " << printReg(Op.Reg, TRI)
                          << "\n");
        break;

      case InlineAsmOperand::OK_DefReg: {

        unsigned Flags = RegState::Define;

        MIB.addReg(Op.Reg, Flags);

        LLVM_DEBUG(dbgs() << "[rewire]   addDefReg " << printReg(Op.Reg, TRI)
                          << " flags=" << Flags << "\n");
        break;
      }

      case InlineAsmOperand::OK_Imm:
        MIB.addImm(Op.Imm);
        LLVM_DEBUG(dbgs() << "[rewire]   addImm " << Op.Imm << "\n");
        break;
      }
    }

    if (isBranchOpc(Record.Opc)) {
      if (IsConditional) {
        if (DstMBB == LR_MBB) {
          LLVM_DEBUG(dbgs() << "[rewire] adding branch target TailMBB\n");
          MIB.addMBB(TailMBB);
        } else {
          LLVM_DEBUG(dbgs() << "[rewire] adding branch target LR_MBB\n");
          MIB.addMBB(LR_MBB);
        }
      } else {
        LLVM_DEBUG(dbgs() << "[rewire] adding branch target MBB for "
                             "unconditional LR retry loop\n");
        MIB.addMBB(&MBB);
      }
    }

    LLVM_DEBUG({
      dbgs() << "[rewire] Built MI: ";
      MIB->dump();
    });

    LLVM_DEBUG({
      const MCInstrDesc &Desc = TII->get(Record.Opc);
      dbgs() << "[rewire] opcode=" << Record.Opc
             << " descNumOperands=" << Desc.getNumOperands()
             << " actualNumOperands=" << MIB->getNumOperands() << "\n";
    });
  }

  // Wire CFG edges:
  //
  //   MBB    -> LR_MBB
  //   LR_MBB -> SC_MBB      fallthrough
  //   LR_MBB -> TailMBB     bne/beq target
  //   SC_MBB -> LR_MBB      bnez retry
  //   SC_MBB -> TailMBB     fallthrough success
  //
  LLVM_DEBUG(dbgs() << "[rewire] About to add CFG successors\n");

  if (IsConditional) {
    MBB.addSuccessor(LR_MBB);
    LLVM_DEBUG(dbgs() << "[rewire] Added MBB -> LR_MBB\n");

    LR_MBB->addSuccessor(SC_MBB);
    LLVM_DEBUG(dbgs() << "[rewire] Added LR_MBB -> SC_MBB\n");

    LR_MBB->addSuccessor(TailMBB);
    LLVM_DEBUG(dbgs() << "[rewire] Added LR_MBB -> TailMBB\n");

    SC_MBB->addSuccessor(LR_MBB);
    LLVM_DEBUG(dbgs() << "[rewire] Added SC_MBB -> LR_MBB\n");

    SC_MBB->addSuccessor(TailMBB);
    LLVM_DEBUG(dbgs() << "[rewire] Added SC_MBB -> TailMBB\n");

    LLVM_DEBUG({
      dbgs() << "[rewire] Final original MBB dump:\n";
      MBB.dump();

      dbgs() << "[rewire] Final LR_MBB dump:\n";
      LR_MBB->dump();

      dbgs() << "[rewire] Final SC_MBB dump:\n";
      SC_MBB->dump();

      dbgs() << "[rewire] Final TailMBB dump:\n";
      TailMBB->dump();
    });
  } else {

    MBB.addSuccessor(&MBB);
    LLVM_DEBUG(dbgs() << "[rewire] Added MBB -> MBB\n");

    MBB.addSuccessor(TailMBB);
    LLVM_DEBUG(dbgs() << "[rewire] Added MBB -> TailMBB\n");
  }

  LLVM_DEBUG(dbgs() << "[rewire] EXIT inlineAsmToMachineInstrsRewireCFG\n");
  LLVM_DEBUG({
    dbgs() << "[rewire] Final original MBB liveins:";
    for (const MachineBasicBlock::RegisterMaskPair &LI : MBB.liveins())
      dbgs() << " " << printReg(LI.PhysReg, TRI);
    dbgs() << "\n";

    dbgs() << "[rewire] Final TailMBB liveins:";
    for (const MachineBasicBlock::RegisterMaskPair &LI : TailMBB->liveins())
      dbgs() << " " << printReg(LI.PhysReg, TRI);
    dbgs() << "\n";

    if (IsConditional) {
      dbgs() << "[rewire] Final LR_MBB liveins:";
      for (const MachineBasicBlock::RegisterMaskPair &LI : LR_MBB->liveins())
        dbgs() << " " << printReg(LI.PhysReg, TRI);
      dbgs() << "\n";

      dbgs() << "[rewire] Final SC_MBB liveins:";
      for (const MachineBasicBlock::RegisterMaskPair &LI : SC_MBB->liveins())
        dbgs() << " " << printReg(LI.PhysReg, TRI);
      dbgs() << "\n";
    }
  });
}

/*--------------------------------------------------------------------------*/
/* buildInlineAsmLineRecords:
   Parses a single line of expanded inline assembly text and appends a
   corresponding InlineAsmLineInstrRecord to InlineAsmInstructionsRecord.
   - Line: One trimmed line of asm text (comments and leading labels already
           removed by the caller's split loop, though this function also
           strips them defensively).

   Processing steps:
     1. Strips any trailing '#' comment from the line.
     2. Strips a leading numeric local label or dot-local label that shares
        the line with an instruction (e.g. "0: lr.w a0, (a1)").
     3. Skips pure label-only lines (e.g. ".Ltmp0:" or "1:").
     4. Splits the line into a mnemonic and the remaining operand text.
     5. Calls mnemonicToOpcode() to map the mnemonic to a RISC-V opcode; if
        the mnemonic is unrecognised, returns without adding a record.
     6. Splits the operand text on commas and calls parseOperand() for each
        token, skipping branch-target tokens (local labels / Nf/Nb refs)
        because those are resolved from CFG context by the caller.
     7. For zero-register branches (bnez / beqz / bltz), synthesizes an
        explicit X0 operand after the first source register so BuildMI
        receives the full two-source-register form expected by BNE/BEQ/BLT.
     8. Pushes the completed record onto InlineAsmInstructionsRecord. */
void RISCVExpandINLINEASM::buildInlineAsmLineRecords(StringRef Line) {
  LLVM_DEBUG(dbgs() << "\n[record] ENTER buildInlineAsmLineRecords line='"
                    << Line << "'\n");

  // Strip trailing comment.
  if (auto H = Line.find('#'); H != StringRef::npos)
    Line = Line.substr(0, H);

  Line = Line.trim();

  LLVM_DEBUG(dbgs() << "[record] after strip/trim line='" << Line << "'\n");

  if (Line.empty()) {
    LLVM_DEBUG(dbgs() << "[record] empty line, return\n");
    return;
  }

  // Strip leading local asm label when label and instruction are on same line.
  // Example:
  //   "0:      lr.w X11, (X10)"
  // becomes:
  //   "lr.w X11, (X10)"
  size_t Colon = Line.find(':');
  if (Colon != StringRef::npos) {
    StringRef MaybeLabel = Line.substr(0, Colon).trim();

    bool IsNumericLocalLabel = !MaybeLabel.empty();
    for (char C : MaybeLabel) {
      if (!std::isdigit(static_cast<unsigned char>(C))) {
        IsNumericLocalLabel = false;
        break;
      }
    }

    bool IsDotLocalLabel = MaybeLabel.starts_with(".");

    if (IsNumericLocalLabel || IsDotLocalLabel) {
      LLVM_DEBUG(dbgs() << "[record] stripping leading label '" << MaybeLabel
                        << "' from line: '" << Line << "'\n");

      Line = Line.substr(Colon + 1).trim();

      LLVM_DEBUG(dbgs() << "[record] line after label strip: '" << Line
                        << "'\n");

      if (Line.empty()) {
        LLVM_DEBUG(dbgs() << "[record] label-only line after strip, return\n");
        return;
      }
    }
  }

  // Skip pure label lines like ".Ltmp0:" or "0:".
  if (Line.ends_with(":") &&
      Line.drop_back().find_first_of(" \t,") == StringRef::npos) {
    LLVM_DEBUG(dbgs() << "[record] pure label line, skip: '" << Line << "'\n");
    return;
  }

  // Split mnemonic from rest.
  size_t WS = Line.find_first_of(" \t");
  StringRef Mnemonic = (WS == StringRef::npos) ? Line : Line.substr(0, WS);
  StringRef Rest =
      (WS == StringRef::npos) ? StringRef() : Line.substr(WS).ltrim();
  LLVM_DEBUG(dbgs() << "[record] Mnemonic='" << Mnemonic << "' Rest='" << Rest
                    << "'\n");

  InlineAsmLineInstrRecord Rec;
  LLVM_DEBUG(dbgs() << "[record] About to map mnemonic to opcode\n");
  Rec.Opc = mnemonicToOpcode(Mnemonic);
  LLVM_DEBUG(dbgs() << "[record] opcode=" << Rec.Opc << "\n");
  if (Rec.Opc == 0) {
    LLVM_DEBUG(
        dbgs() << "[record][unsupported] unsupported inline asm instruction: '"
               << Mnemonic << "' from line: '" << Line << "'\n");
    LLVM_DEBUG(
        dbgs() << "[record][unsupported] returning without adding a record\n");
    return;
  }

  bool IsZeroBranch =
      (Mnemonic == "bnez" || Mnemonic == "beqz" || Mnemonic == "bltz");

  SmallVector<StringRef, 4> Toks;
  Rest.split(Toks, ',');
  LLVM_DEBUG(dbgs() << "[record] token count=" << Toks.size() << "\n");

  for (unsigned I = 0; I < Toks.size(); ++I) {
    StringRef Tok = Toks[I].trim();
    LLVM_DEBUG(dbgs() << "[record] token #" << I << " raw='" << Toks[I]
                      << "' trimmed='" << Tok << "'\n");
    if (Tok.empty())
      continue;

    // Drop the branch target operand — caller resolves it from CFG context.
    if (isBranchMnemonic(Mnemonic) && isBranchTargetTok(Tok))
      continue;

    LLVM_DEBUG(dbgs() << "[record] About to parse operand token='" << Tok
                      << "' opIdx=" << I << "\n");
    Rec.Operands.push_back(parseOperand(Tok, Mnemonic, I));
    LLVM_DEBUG(dbgs() << "[record] Rec operands now=" << Rec.Operands.size()
                      << "\n");

    // For bnez/beqz: synthesize the implicit zero register after the first op.
    if (IsZeroBranch && I == 0) {
      InlineAsmOperand Z;
      Z.Kind = InlineAsmOperand::OK_Reg;
      Z.Reg = RISCV::X0;
      Z.Imm = 0;
      Rec.Operands.push_back(Z);
      LLVM_DEBUG(
          dbgs() << "[record] Synthesized zero operand for zero-branch\n");
    }
  }

  LLVM_DEBUG(dbgs() << "[record] About to push record opcode=" << Rec.Opc
                    << " operands=" << Rec.Operands.size() << "\n");
  InlineAsmInstructionsRecord.push_back(std::move(Rec));
  LLVM_DEBUG(dbgs() << "[record] records total="
                    << InlineAsmInstructionsRecord.size() << "\n");
  LLVM_DEBUG(dbgs() << "[record] EXIT buildInlineAsmLineRecords\n");
}

/*--------------------------------------------------------------------------*/
/* parseOperand:
   Parses a single comma-separated operand token from an expanded inline asm
   line and returns the corresponding InlineAsmOperand.
   - Tok:      Trimmed text of the operand (e.g. "a0", "(a1)", "0", "rw").
   - Mnemonic: The instruction mnemonic on the same line; used to determine
               whether the operand is a definition (output) register and to
               detect fence ordering strings.
   - OpIdx:    Zero-based position of this operand in the instruction's
               operand list; operand 0 of lr.*\/sc.*\/ sub is treated as a
               definition register (OK_DefReg).

   Recognised token forms (checked in order):
     1. "(reg)"   - Memory address form used by LR/SC; strips the parentheses
                    and calls parseRegName() on the inner text. Returns
                    OK_Reg. Falls back to OK_Imm=0 if the register is invalid.
     2. fence arg - If mnemonic is "fence", encodes the ordering string
                    ("r", "w", "rw", "iorw", etc.) as a bitmask immediate via
                    encodeFenceArg() and returns OK_Imm.
     3. Numeric   - Decimal or hex integer literal; returns OK_Imm.
     4. Register  - Calls parseRegName(); returns OK_DefReg for OpIdx==0 on
                    lr.*\/sc.*\/sub, OK_Reg otherwise. Returns OK_Imm=0 if the
                    register name is unrecognised. */
InlineAsmOperand RISCVExpandINLINEASM::parseOperand(StringRef Tok,
                                                    StringRef Mnemonic,
                                                    unsigned OpIdx) {
  LLVM_DEBUG(dbgs() << "[parseOperand] ENTER Tok='" << Tok << "' Mnemonic='"
                    << Mnemonic << "' OpIdx=" << OpIdx << "\n");
  InlineAsmOperand Op;
  Op.Imm = 0;
  Op.Reg = Register();

  // Memory form "(reg)" — used by lr/sc for the address.
  if (Tok.starts_with("(") && Tok.ends_with(")")) {
    Op.Kind = InlineAsmOperand::OK_Reg;
    LLVM_DEBUG(dbgs() << "[parseOperand] memory operand, inner='"
                      << Tok.drop_front().drop_back().trim() << "'\n");
    Op.Reg = parseRegName(Tok.drop_front().drop_back().trim());
    LLVM_DEBUG(dbgs() << "[parseOperand] memory reg=" << Op.Reg << "\n");
    if (!Op.Reg.isValid()) {
      LLVM_DEBUG(
          dbgs()
          << "[parseOperand][unsupported] bad register in memory operand: '"
          << Tok << "' for mnemonic '" << Mnemonic << "'\n");
      Op.Kind = InlineAsmOperand::OK_Imm;
      Op.Imm = 0;
      return Op;
    }
    return Op;
  }

  // Fence ordering: "rw", "r", "w", "iorw", ... encoded as imm.
  if (Mnemonic == "fence") {
    LLVM_DEBUG(dbgs() << "[parseOperand] fence operand\n");
    Op.Kind = InlineAsmOperand::OK_Imm;
    Op.Imm = encodeFenceArg(Tok);
    LLVM_DEBUG(dbgs() << "[parseOperand] fence encoded imm=" << Op.Imm << "\n");
    return Op;
  }

  // Numeric immediate.
  int64_t ImmVal;
  if (!Tok.getAsInteger(0, ImmVal)) {
    LLVM_DEBUG(dbgs() << "[parseOperand] numeric immediate=" << ImmVal << "\n");
    Op.Kind = InlineAsmOperand::OK_Imm;
    Op.Imm = ImmVal;
    return Op;
  }

  // Register name.
  LLVM_DEBUG(dbgs() << "[parseOperand] About to parse register token='" << Tok
                    << "'\n");
  Register R = parseRegName(Tok);
  LLVM_DEBUG(dbgs() << "[parseOperand] parseRegName returned reg=" << R
                    << " valid=" << R.isValid() << "\n");
  if (!R.isValid()) {
    LLVM_DEBUG(
        dbgs() << "[parseOperand][unsupported] unrecognized operand token: '"
               << Tok << "' for mnemonic '" << Mnemonic << "'\n");
    Op.Kind = InlineAsmOperand::OK_Imm;
    Op.Imm = 0;
    return Op;
  }

  bool IsDef =
      (OpIdx == 0) && (Mnemonic.starts_with("lr.") ||
                       Mnemonic.starts_with("sc.") || Mnemonic == "sub");
  Op.Kind = IsDef ? InlineAsmOperand::OK_DefReg : InlineAsmOperand::OK_Reg;
  Op.Reg = R;
  LLVM_DEBUG(dbgs() << "[parseOperand] returning kind=" << Op.Kind
                    << " reg=" << Op.Reg << " imm=" << Op.Imm << "\n");
  return Op;
}

/*--------------------------------------------------------------------------*/
/* isBranchOpc:
   Returns true if the given RISC-V opcode is a conditional branch that this
   pass knows how to route to a MachineBasicBlock target.
   - Opc: A RISC-V MC opcode value.
   Currently recognised opcodes: BNE, BEQ, BLT.
   Used to decide whether an InlineAsmLineInstrRecord needs a MBB operand
   appended during CFG rewiring, and to detect whether the LR/SC sequence
   contains a conditional path between the LR and SC instructions. */
bool RISCVExpandINLINEASM::isBranchOpc(unsigned Opc) {
  LLVM_DEBUG(dbgs() << "[isBranchOpc] Opc=" << Opc << "\n");
  return Opc == RISCV::BNE || Opc == RISCV::BEQ || Opc == RISCV::BLT;
}

/*--------------------------------------------------------------------------*/
/* isBranchMnemonic:
   Returns true if the given mnemonic string is any conditional branch
   instruction recognised by this pass.
   - M: Lowercase mnemonic text (e.g. "bne", "bnez", "bltz").
   Used during operand parsing to decide whether a comma-separated token
   should be tested as a branch-target label (and therefore skipped) rather
   than parsed as a register or immediate operand. */
bool RISCVExpandINLINEASM::isBranchMnemonic(StringRef M) {
  LLVM_DEBUG(dbgs() << "[isBranchMnemonic] M='" << M << "'\n");
  return M == "bne" || M == "beq" || M == "bnez" || M == "beqz" || M == "blt" ||
         M == "bltz" || M == "bge" || M == "bltu" || M == "bgeu";
}

/*--------------------------------------------------------------------------*/
/* isBranchTargetTok:
   Returns true if the given operand token looks like an inline-asm branch
   target that should be skipped during operand parsing.
   - Tok: A trimmed comma-separated token from an asm line.
   Recognised forms:
     - Tokens starting with '.' (dot-local labels, e.g. ".Lretry").
     - Two-character GNU numeric local label references of the form "Nf" or
       "Nb" where N is a single digit (e.g. "1f", "2b").
   These targets are resolved to MachineBasicBlock pointers by the CFG
   rewiring logic rather than being emitted as literal operands. */
bool RISCVExpandINLINEASM::isBranchTargetTok(StringRef Tok) {
  LLVM_DEBUG(dbgs() << "[isBranchTargetTok] Tok='" << Tok << "'\n");
  if (Tok.starts_with("."))
    return true;
  if (Tok.size() == 2 && isdigit((unsigned char)Tok[0]) &&
      (Tok[1] == 'b' || Tok[1] == 'f'))
    return true;
  return false;
}

/*--------------------------------------------------------------------------*/
/* mnemonicToOpcode:
   Maps a lowercase RISC-V instruction mnemonic string to its corresponding
   RISC-V MC opcode integer.
   - M: Lowercase mnemonic text (e.g. "lr.w", "sc.d.aqrl", "bne", "fence").
   Covers all LR/SC variants (lr.w, lr.w.aq, lr.w.rl, lr.w.aqrl, lr.d and
   their ordering suffixes), SC equivalents, SUB, the conditional branches
   BNE/BEQ/BLT and their zero-register pseudo forms (bnez/beqz/bltz), and
   FENCE.
   Returns 0 for any unrecognised mnemonic, which causes the caller
   (buildInlineAsmLineRecords) to discard the line without adding a record. */
unsigned RISCVExpandINLINEASM::mnemonicToOpcode(StringRef M) {
  LLVM_DEBUG(dbgs() << "[mnemonicToOpcode] M='" << M << "'\n");
  return StringSwitch<unsigned>(M)
      .Case("lr.w", RISCV::LR_W)
      .Case("lr.w.aq", RISCV::LR_W_AQ)
      .Case("lr.w.rl", RISCV::LR_W_RL)
      .Case("lr.w.aqrl", RISCV::LR_W_AQRL)
      .Case("lr.d", RISCV::LR_D)
      .Case("lr.d.aq", RISCV::LR_D_AQ)
      .Case("lr.d.rl", RISCV::LR_D_RL)
      .Case("lr.d.aqrl", RISCV::LR_D_AQRL)
      .Case("sc.w", RISCV::SC_W)
      .Case("sc.w.aq", RISCV::SC_W_AQ)
      .Case("sc.w.rl", RISCV::SC_W_RL)
      .Case("sc.w.aqrl", RISCV::SC_W_AQRL)
      .Case("sc.d", RISCV::SC_D)
      .Case("sc.d.aq", RISCV::SC_D_AQ)
      .Case("sc.d.rl", RISCV::SC_D_RL)
      .Case("sc.d.aqrl", RISCV::SC_D_AQRL)
      .Case("sub", RISCV::SUB)
      .Case("bne", RISCV::BNE)
      .Case("beq", RISCV::BEQ)
      .Case("bnez", RISCV::BNE)
      .Case("beqz", RISCV::BEQ)
      .Case("bltz", RISCV::BLT)
      .Case("blt", RISCV::BLT)
      .Case("fence", RISCV::FENCE)
      .Default(0);
}

/*--------------------------------------------------------------------------*/
/* parseRegName:
   Maps a RISC-V register name string to its physical Register value.
   - N: Register name in any of the supported formats:
          Xn / xn      (ABI-neutral, e.g. "X10", "x10")
          ABI alias    (e.g. "a0", "t0", "s1", "zero", "ra", "sp")
   Covers all 32 integer registers (X0-X31) together with their ABI aliases
   (zero, ra, sp, gp, tp, t0-t6, s0/fp, s1-s11, a0-a7).
   Returns an invalid Register() for any unrecognised name string, which
   causes parseOperand() to fall back to an OK_Imm=0 operand and emit an
   [unsupported] debug message. */
Register RISCVExpandINLINEASM::parseRegName(StringRef N) {
  LLVM_DEBUG(dbgs() << "[parseRegName] N='" << N << "'\n");

  return StringSwitch<Register>(N)
      .Case("X0", RISCV::X0)
      .Case("x0", RISCV::X0)
      .Case("zero", RISCV::X0)
      .Case("X1", RISCV::X1)
      .Case("x1", RISCV::X1)
      .Case("ra", RISCV::X1)
      .Case("X2", RISCV::X2)
      .Case("x2", RISCV::X2)
      .Case("sp", RISCV::X2)
      .Case("X3", RISCV::X3)
      .Case("x3", RISCV::X3)
      .Case("gp", RISCV::X3)
      .Case("X4", RISCV::X4)
      .Case("x4", RISCV::X4)
      .Case("tp", RISCV::X4)

      .Case("X5", RISCV::X5)
      .Case("x5", RISCV::X5)
      .Case("t0", RISCV::X5)
      .Case("X6", RISCV::X6)
      .Case("x6", RISCV::X6)
      .Case("t1", RISCV::X6)
      .Case("X7", RISCV::X7)
      .Case("x7", RISCV::X7)
      .Case("t2", RISCV::X7)

      .Case("X8", RISCV::X8)
      .Case("x8", RISCV::X8)
      .Case("s0", RISCV::X8)
      .Case("fp", RISCV::X8)
      .Case("X9", RISCV::X9)
      .Case("x9", RISCV::X9)
      .Case("s1", RISCV::X9)

      .Case("X10", RISCV::X10)
      .Case("x10", RISCV::X10)
      .Case("a0", RISCV::X10)
      .Case("X11", RISCV::X11)
      .Case("x11", RISCV::X11)
      .Case("a1", RISCV::X11)
      .Case("X12", RISCV::X12)
      .Case("x12", RISCV::X12)
      .Case("a2", RISCV::X12)
      .Case("X13", RISCV::X13)
      .Case("x13", RISCV::X13)
      .Case("a3", RISCV::X13)
      .Case("X14", RISCV::X14)
      .Case("x14", RISCV::X14)
      .Case("a4", RISCV::X14)
      .Case("X15", RISCV::X15)
      .Case("x15", RISCV::X15)
      .Case("a5", RISCV::X15)
      .Case("X16", RISCV::X16)
      .Case("x16", RISCV::X16)
      .Case("a6", RISCV::X16)
      .Case("X17", RISCV::X17)
      .Case("x17", RISCV::X17)
      .Case("a7", RISCV::X17)

      .Case("X18", RISCV::X18)
      .Case("x18", RISCV::X18)
      .Case("s2", RISCV::X18)
      .Case("X19", RISCV::X19)
      .Case("x19", RISCV::X19)
      .Case("s3", RISCV::X19)
      .Case("X20", RISCV::X20)
      .Case("x20", RISCV::X20)
      .Case("s4", RISCV::X20)
      .Case("X21", RISCV::X21)
      .Case("x21", RISCV::X21)
      .Case("s5", RISCV::X21)
      .Case("X22", RISCV::X22)
      .Case("x22", RISCV::X22)
      .Case("s6", RISCV::X22)
      .Case("X23", RISCV::X23)
      .Case("x23", RISCV::X23)
      .Case("s7", RISCV::X23)
      .Case("X24", RISCV::X24)
      .Case("x24", RISCV::X24)
      .Case("s8", RISCV::X24)
      .Case("X25", RISCV::X25)
      .Case("x25", RISCV::X25)
      .Case("s9", RISCV::X25)
      .Case("X26", RISCV::X26)
      .Case("x26", RISCV::X26)
      .Case("s10", RISCV::X26)
      .Case("X27", RISCV::X27)
      .Case("x27", RISCV::X27)
      .Case("s11", RISCV::X27)

      .Case("X28", RISCV::X28)
      .Case("x28", RISCV::X28)
      .Case("t3", RISCV::X28)
      .Case("X29", RISCV::X29)
      .Case("x29", RISCV::X29)
      .Case("t4", RISCV::X29)
      .Case("X30", RISCV::X30)
      .Case("x30", RISCV::X30)
      .Case("t5", RISCV::X30)
      .Case("X31", RISCV::X31)
      .Case("x31", RISCV::X31)
      .Case("t6", RISCV::X31)

      .Default(Register());
}

/*--------------------------------------------------------------------------*/
/* encodeFenceArg:
   Encodes a RISC-V fence ordering string into the 4-bit immediate value
   expected by the FENCE instruction.
   - S: Ordering specifier string composed of the characters 'i', 'o', 'r',
        and 'w' in any combination (e.g. "rw", "iorw", "r", "w").
   Bit assignment (matching the RISC-V ISA spec):
     'i' -> bit 3 (0b1000)  predecessor/successor device input
     'o' -> bit 2 (0b0100)  predecessor/successor device output
     'r' -> bit 1 (0b0010)  memory read
     'w' -> bit 0 (0b0001)  memory write
   Unrecognised characters are silently ignored with a debug warning.
   Returns the accumulated bitmask as a signed 64-bit integer suitable for
   passing to MachineInstrBuilder::addImm(). */
int64_t RISCVExpandINLINEASM::encodeFenceArg(StringRef S) {
  LLVM_DEBUG(dbgs() << "[encodeFenceArg] S='" << S << "'\n");
  int64_t V = 0;
  for (char C : S) {
    LLVM_DEBUG(dbgs() << "[encodeFenceArg] char='" << C << "' before V=" << V
                      << "\n");
    switch (C) {
    case 'i':
      V |= 0b1000;
      break;
    case 'o':
      V |= 0b0100;
      break;
    case 'r':
      V |= 0b0010;
      break;
    case 'w':
      V |= 0b0001;
      break;
    default:
      LLVM_DEBUG(
          dbgs()
          << "[encodeFenceArg][unsupported] bad fence ordering character: '"
          << C << "' in '" << S << "'\n");
      break;
    }
  }
  return V;
}

/*--------------------------------------------------------------------------*/
/* dumpStats:
   Emits a human-readable summary of pass statistics to the given output
   stream.
   - OS: The raw_ostream to write to (typically dbgs() from the destructor).
   Prints the module name recorded during the most recent runOnMachineFunction
   call and the number of InlineAsmLineInstrRecords that were parsed and
   stored in InlineAsmInstructionsRecord.
   Called unconditionally from the destructor when the
   -dump-expand-inline-asm-stat command-line flag is enabled. */
void RISCVExpandINLINEASM::dumpStats(raw_ostream &OS) {
  OS << "RISCVExpandINLINEASM stats for module " << ModuleName << "\n";
  OS << "  expanded records: " << InlineAsmInstructionsRecord.size() << "\n";
}

/*--------------------------------------------------------------------------*/
/* copyLiveInsManually:
   Copies all live-in register/lane-mask pairs from a source MachineBasicBlock
   to a destination MachineBasicBlock, skipping any register that is already
   marked as live-in in the destination.
   - Dst: The MachineBasicBlock to receive the copied live-ins.
   - Src: The MachineBasicBlock whose live-in set is iterated.
   Used to propagate liveness information to newly created MBBs (LR_MBB,
   SC_MBB, TailMBB) after the original MBB is split around the INLINEASM
   instruction, ensuring that registers live before the split remain visible
   to register-allocation and verification passes in the new blocks. */
void RISCVExpandINLINEASM::copyLiveInsManually(MachineBasicBlock *Dst,
                                               const MachineBasicBlock &Src) {
  for (const MachineBasicBlock::RegisterMaskPair &LI : Src.liveins()) {
    if (!Dst->isLiveIn(LI.PhysReg))
      Dst->addLiveIn(LI.PhysReg, LI.LaneMask);
  }
}