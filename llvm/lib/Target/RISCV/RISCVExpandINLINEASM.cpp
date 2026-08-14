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
/* Define the public pass name and the debug channel used by LLVM diagnostics. */
#define RISCV_EXPAND_INLINE_ASM_NAME "RISC-V Expand Inline Assembly "
#define DEBUG_TYPE "riscv-expand-inline-asm"

/* Include the RISC-V-specific helpers and target interfaces used by the pass. */
#include "LRSCCountUtils.hpp"
#include "RISCV.h"
#include "RISCVInstrInfo.h"
#include "RISCVTargetMachine.h"
/* Include LLVM containers, machine-code APIs, IR types, and support utilities. */
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
/* Include the C++ standard-library facilities used by the implementation. */
#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

/* Make LLVM and LR/SC utility names directly available in this translation unit. */
using namespace llvm;
using namespace utils;

/* Control whether the pass emits its collected inline-assembly statistics. */
cl::opt<bool> RISCVEXPANDINLINEASMEMIT(
    "dump-expand-inline-asm-stat", cl::Hidden,
    cl::desc("The stat emission control is used for enabling and disabling the "
             "emission of instruction stats for inline assembly blocks. "),
    cl::init(false));

/* Associate an inline-assembly label name with its parsed instruction index. */
struct InlineAsmLabelRecord {
  std::string Name;
  int RecordIndex = 0;
};
/* Represent one parsed register or immediate operand and its emission role. */
struct InlineAsmOperand {
  enum OperandKind { OK_Reg, OK_DefReg, OK_Imm, OK_Invalid };

  OperandKind Kind = OK_Invalid;
  Register Reg = Register();
  int64_t Imm = 0;
};

/* Store the opcode, operands, and optional branch target for one asm line. */
struct InlineAsmLineInstrRecord {

  unsigned Opc = 0;
  SmallVector<InlineAsmOperand, 4> Operands;
  std::string BranchTarget;
};
/* Keep the pass implementation private to this translation unit. */
namespace {
/* Declare the MachineFunction pass and its parsing and CFG-rewriting helpers. */
class RISCVExpandINLINEASM : public MachineFunctionPass {
public:
  /* Hold the legacy pass identifier required by LLVM's pass manager. */
  static char ID;

  /* Retain the current module name for optional statistics output. */
  static std::string ModuleName;

/*--------------------------------------------------------------------------*/
/* RISCVExpandINLINEASM:
   Constructs the pass with the legacy MachineFunctionPass identifier.
   - Registers this instance under the static pass ID used by LLVM. */
  RISCVExpandINLINEASM() : MachineFunctionPass(ID) {}

  ~RISCVExpandINLINEASM();

  std::string expandInlineAsm(const MachineInstr &MI, const char *AsmStr,
                              const TargetRegisterInfo *TRI);

  bool isBranchMnemonic(StringRef M);

  bool hasDefOperand(StringRef Mnemonic);



  int resolveBranchTarget(StringRef Target,
                          int BranchRecordIndex) const;

  bool isConditionalBranchOpc(unsigned Opc);

  bool isUnconditionalBranchOpc(unsigned Opc);

  bool isBranchOpc(unsigned Opc);

  Register parseRegName(StringRef N);

  int64_t encodeFenceArg(StringRef S);

  InlineAsmOperand parseOperand(StringRef Tok, StringRef Mnemonic,
                              unsigned OpIdx);

  unsigned mnemonicToOpcode(StringRef M);

  bool runOnMachineFunction(MachineFunction &MF) override;

/*--------------------------------------------------------------------------*/
/* getPassName:
   Returns the human-readable name used to identify this pass.
   - Supplies the shared RISCV_EXPAND_INLINE_ASM_NAME string. */
  StringRef getPassName() const override {
    return RISCV_EXPAND_INLINE_ASM_NAME;
  }

  void dumpStats(raw_ostream &OS);

  bool inlineAsmToMachineInstrsRewireCFG(StringRef AsmStr,
                                         MachineBasicBlock &MBB,
                                         MachineBasicBlock::iterator MBBI,
                                         const TargetInstrInfo *TII);

  void buildInlineAsmLineRecords(StringRef Line);

private:
  /* Accumulate the parsed instructions and labels for the current asm block. */
  SmallVector<InlineAsmLineInstrRecord, 6> InlineAsmInstructionsRecords;
  SmallVector<InlineAsmLabelRecord, 8> InlineAsmLabels;
  /* Record whether any unsupported syntax invalidated the current parse. */
  bool InlineAsmParsingFailed = false;
}; // end class RISCVExpandINLINEASM

} // end anonymous namespace

/* Define the pass identifier and initialize the stored module name. */
char RISCVExpandINLINEASM::ID = 0;
std::string RISCVExpandINLINEASM::ModuleName = "";

/* Register the pass with LLVM's legacy pass initialization infrastructure. */
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
   (inner break) and returns whether at least one supported INLINEASM was
   successfully expanded. */
bool RISCVExpandINLINEASM::runOnMachineFunction(MachineFunction &MF) {
  /* Track whether expanding any INLINEASM changes this MachineFunction. */
  bool Changed = false;
  /* Report the function and its initial machine-block state for debugging. */
  LLVM_DEBUG(
      dbgs() << "\n=== RISCVExpandINLINEASM runOnMachineFunction ENTER ===\n");
  LLVM_DEBUG(dbgs() << "[run] Function: " << MF.getName() << "\n");
  LLVM_DEBUG(dbgs() << "[run] Initial MBB count: " << MF.size() << "\n");

  /* Capture the parent module name used by the optional statistics report. */
  llvm::Module *m = MF.getFunction().getParent();
  RISCVExpandINLINEASM::ModuleName = m->getModuleIdentifier();
  LLVM_DEBUG(dbgs() << "[run] Module: " << RISCVExpandINLINEASM::ModuleName
                    << "\n");
  /* Visit each machine basic block that may contain opaque inline assembly. */
  for (auto &MBB : MF) {
    LLVM_DEBUG(dbgs() << "[run] Visiting MBB #" << MBB.getNumber() << " size="
                      << MBB.size() << " succs=" << MBB.succ_size() << "\n");
    /* Obtain target interfaces needed for register names and instruction emission. */
    const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
    const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
    LLVM_DEBUG(dbgs() << "[run] TRI=" << TRI << " TII=" << TII << "\n");
    /* Scan the current block until an INLINEASM MachineInstr is found. */
    for (auto MBBI = MBB.begin(), E = MBB.end(); MBBI != E; ++MBBI) {
      LLVM_DEBUG(dbgs() << "[run]   MI opcode=" << MBBI->getOpcode()
                        << " numOperands=" << MBBI->getNumOperands()
                        << " isInlineAsm=" << MBBI->isInlineAsm() << "\n");
      if (MBBI->isInlineAsm()) {
        /* Preserve the instruction location and inspect the raw INLINEASM operand. */
        LLVM_DEBUG(dbgs() << "[run] >>> Found INLINEASM in MBB #"
                          << MBB.getNumber() << "\n");
        LLVM_DEBUG(dbgs() << "[run] INLINEASM MachineInstr dump follows:\n");
        LLVM_DEBUG(MBBI->dump());
        DebugLoc DL = MBBI->getDebugLoc();
        LLVM_DEBUG(dbgs() << "[run] DebugLoc valid=" << bool(DL) << "\n");
        // Process inline assembly instruction
        LLVM_DEBUG( dbgs() << "[run] About to read asm string from operand 0. numOperands=" << MBBI->getNumOperands() << "\n");
        const char *AsmStr = MBBI->getOperand(0).getSymbolName();
        LLVM_DEBUG(dbgs() << "Found inline assembly: " << AsmStr << "\n");
        /* Expand numeric placeholders into concrete register or immediate text. */
        LLVM_DEBUG(dbgs() << "[run] About to call expandInlineAsm()\n");
        std::string ExpandedAssemblyStr = expandInlineAsm(*MBBI, AsmStr, TRI);
        LLVM_DEBUG( dbgs() << "[run] Returned from expandInlineAsm(), expanded length=" << ExpandedAssemblyStr.size() << "\n");
        LLVM_DEBUG(dbgs() << "[run] Expanded asm:\n" << ExpandedAssemblyStr << "\n");

        llvm::StringRef ExpandedAssemblyStrRef(ExpandedAssemblyStr);

        /* Lower the expanded instructions and merge the resulting CFG change. */
        LLVM_DEBUG( dbgs() << "[run] About to call inlineAsmToMachineInstrsRewireCFG()\n");
        Changed |= inlineAsmToMachineInstrsRewireCFG(ExpandedAssemblyStrRef, MBB, MBBI, TII);
        LLVM_DEBUG(
            dbgs()
            << "[run] Returned from inlineAsmToMachineInstrsRewireCFG()\n");

        /* Process at most one INLINEASM instruction from this basic block. */
        break;
      }

    } // end inner loop over instructions in basic block

  } // end outer loop over MachineBasicBlocks
  LLVM_DEBUG(dbgs() << "[run] Final MBB count: " << MF.size() << "\n");
  LLVM_DEBUG(
      dbgs() << "=== RISCVExpandINLINEASM runOnMachineFunction EXIT ===\n");
  /* Tell the pass manager whether a supported INLINEASM block was expanded. */
  return Changed;
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
            AsmStr[ScanIndex] >= '0' &&
            AsmStr[ScanIndex] <= '9') {
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

      size_t OperandNumber = std::stoul(std::string(
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
      if (OperandNumber < OpStrings.size()) {
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
        each non-empty line to populate InlineAsmInstructionsRecords.
     2. Returns early (leaving INLINEASM untouched) if no records were parsed
        or if the record set does not contain both an LR and an SC instruction.
    3. Resolves every recorded branch target to an instruction-record index.
    4. Builds the set of instruction-record boundaries where MachineBasicBlocks
      must begin, including branch targets and branch fallthrough boundaries.
    5. Creates MachineBasicBlocks for those instruction regions and maps every
      instruction-record index to its containing MachineBasicBlock.
    6. Moves instructions following the INLINEASM into TailMBB and maps the
      record index after the final inline-asm instruction to TailMBB.
    7. Emits each parsed instruction into the MachineBasicBlock associated with
      its instruction-record index.
    8. Resolves each branch target record index to its MachineBasicBlock and adds
      that block as the branch operand.
    9. Builds CFG successor edges from the resolved branch targets and
      fallthrough record boundaries.*/
bool RISCVExpandINLINEASM::inlineAsmToMachineInstrsRewireCFG(
    StringRef AsmStr, MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
    const TargetInstrInfo *TII) {
  
  LLVM_DEBUG(dbgs() << "\n[rewire] ENTER inlineAsmToMachineInstrsRewireCFG\n");
  LLVM_DEBUG(dbgs() << "[rewire] source MBB #" << MBB.getNumber()
                    << " size=" << MBB.size() << " succs=" << MBB.succ_size()
                    << " TII=" << TII << "\n");
  LLVM_DEBUG(dbgs() << "[rewire] AsmStr length=" << AsmStr.size() << "\n");
  LLVM_DEBUG(dbgs() << "[rewire] AsmStr:\n" << AsmStr << "\n");

  /* Reset all state retained from the previously processed INLINEASM block. */
  InlineAsmInstructionsRecords.clear();
  InlineAsmLabels.clear();
  InlineAsmParsingFailed = false;

  LLVM_DEBUG(dbgs() << "[rewire] Cleared InlineAsmInstructionsRecords\n");

  /* Split the expanded template into individual lines for instruction parsing. */
  SmallVector<StringRef, 16> Lines;
  AsmStr.split(Lines, '\n');

  LLVM_DEBUG(dbgs() << "[rewire] split line count=" << Lines.size() << "\n");

  /* Parse every non-empty line while preserving the original asm on failure. */
  for (StringRef Line : Lines) {
    LLVM_DEBUG(dbgs() << "[rewire] raw line='" << Line << "'\n");

    Line = Line.trim();

    LLVM_DEBUG(dbgs() << "[rewire] trimmed line='" << Line << "'\n");

    if (Line.empty()) {
      LLVM_DEBUG(dbgs() << "[rewire] empty line, skip\n");
      continue;
    }

    LLVM_DEBUG(dbgs() << "[rewire] About to call buildInlineAsmLineRecords()\n");
    buildInlineAsmLineRecords(Line);
    if (InlineAsmParsingFailed) {
      LLVM_DEBUG(dbgs() << "[rewire] parsing failed; preserving original INLINEASM\n");

      InlineAsmInstructionsRecords.clear();
      return false;
    }
    LLVM_DEBUG(dbgs() << "[rewire] returned from buildInlineAsmLineRecords(), records=" << InlineAsmInstructionsRecords.size() << "\n");
  }

  /* Leave the opaque INLINEASM untouched when no supported records were found. */
  if (InlineAsmInstructionsRecords.empty()) {
    LLVM_DEBUG(dbgs() << "[rewire][unsupported] no supported inline asm records were parsed; " << "leaving original INLINEASM unchanged\n");
    return false;
  }
  /* Cache the parsed record count used by all following index calculations. */
  int NumRecords = InlineAsmInstructionsRecords.size();
  /* Resolve and collect each unique instruction index targeted by a branch. */
  //collect the unique targeted record boundaries
  SmallVector<int, 8> TargetRecordIndices;
  for (int I = 0; I < NumRecords; ++I) {
    const InlineAsmLineInstrRecord &Record =
        InlineAsmInstructionsRecords[I];

    if (!isBranchOpc(Record.Opc)) {
      continue;
    }
      

    int TargetRecordIndex = resolveBranchTarget(Record.BranchTarget, I);

    if (TargetRecordIndex < 0) {

      LLVM_DEBUG(dbgs() << "[rewire][unsupported] could not resolve branch target '"
                << Record.BranchTarget << "' for instruction record " << I
                << "; preserving original INLINEASM\n");

      InlineAsmParsingFailed = true;
      InlineAsmInstructionsRecords.clear();
      InlineAsmLabels.clear();
      return false;
    }

    if (!llvm::is_contained(TargetRecordIndices, TargetRecordIndex))
      TargetRecordIndices.push_back(TargetRecordIndex);

    LLVM_DEBUG(
        dbgs() << "[rewire] resolved branch target '" << Record.BranchTarget
              << "' from instruction record " << I
              << " to instruction record " << TargetRecordIndex << "\n");
  }
  /* Order branch-target boundaries before constructing block intervals. */
  llvm::sort(TargetRecordIndices);

  //collect all block-start record indices
  /*
  
  TargetRecordIndices= where branches jump

  BlockStartRecordIndices = where MBBs need to start = branch targets:
        + fallthrough-after-branch boundaries
        + beginning of inline asm
        + final record index
  */
  /* Seed block starts with branch targets and the inline-assembly entry. */
  SmallVector<int, 8> BlockStartRecordIndices = TargetRecordIndices;

  if (!llvm::is_contained(BlockStartRecordIndices, 0))
    BlockStartRecordIndices.push_back(0);

  

  /* Add the instruction after every branch as a possible fallthrough block. */
  for (int I = 0; I < NumRecords; ++I) {
    const InlineAsmLineInstrRecord &Record = InlineAsmInstructionsRecords[I];

    if (!isBranchOpc(Record.Opc)){
      continue;
    }
      

    int FallthroughIndex = I + 1;

    if (!llvm::is_contained(BlockStartRecordIndices, FallthroughIndex))
      BlockStartRecordIndices.push_back(FallthroughIndex);
  }

  /* Add the end sentinel that maps the post-inline-asm path to TailMBB. */
  if (!llvm::is_contained(BlockStartRecordIndices, NumRecords))
    BlockStartRecordIndices.push_back(NumRecords);

  llvm::sort(BlockStartRecordIndices);

  /* Confirm that the parsed block contains the LR/SC pair this pass handles. */
  bool HasLR = false;
  bool HasSC = false;

  for (const InlineAsmLineInstrRecord &Record : InlineAsmInstructionsRecords) {
    if (lrsc::isLR(Record.Opc))
      HasLR = true;

    if (lrsc::isSC(Record.Opc))
      HasSC = true;
  }

  LLVM_DEBUG(dbgs() << " HasLR=" << HasLR << " HasSC=" << HasSC << "\n");

  if (!HasLR || !HasSC) {
    LLVM_DEBUG(dbgs() << "[rewire][unsupported] parsed records do not contain "
                         "both LR and SC; "
                      << "leaving original INLINEASM unchanged\n");
    return false;
  }

  /* Gather the parent function, source location, IR block, and register info. */
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
  /*
  For block starts : {0, 2, 4}
  -> record 0 -> MBB A
     record 1 -> MBB A

     record 2 -> MBB B
     record 3 -> MBB B

     record 4 -> TailMBB
  */
  /* Create the record-to-block map and retain every generated inline-asm block. */
  // map each record index to the MBB that will contain it after the split
  std::map<int, MachineBasicBlock *> RecordIndexToMBB;
  SmallVector<MachineBasicBlock *, 8> InlineAsmMBBs;

  MachineFunction::iterator InsertPt = std::next(MBB.getIterator());

  /* Create one MachineBasicBlock for each interval between block boundaries. */
  for (size_t BlockIndex = 0; BlockIndex + 1 < BlockStartRecordIndices.size(); ++BlockIndex) {

    int StartRecordIndex = BlockStartRecordIndices[BlockIndex];
    int EndRecordIndex = BlockStartRecordIndices[BlockIndex + 1];

    MachineBasicBlock *NewMBB = MF->CreateMachineBasicBlock(LLVM_BB);
    // insert immediately after MBB
    MF->insert(InsertPt, NewMBB);
    InsertPt = std::next(NewMBB->getIterator());

    InlineAsmMBBs.push_back(NewMBB);

    for (int RecordIndex = StartRecordIndex; RecordIndex < EndRecordIndex;++RecordIndex) {
      RecordIndexToMBB[RecordIndex] = NewMBB;
    }
  }

  /* Create the tail block that receives instructions following the INLINEASM. */
  MachineBasicBlock *TailMBB = MF->CreateMachineBasicBlock(LLVM_BB);

  MF->insert(InsertPt, TailMBB);

  RecordIndexToMBB[NumRecords] = TailMBB;

  // ------------------------------------------------------------
  // Copy original live-ins manually.
  // ------------------------------------------------------------
  for (const MachineBasicBlock::RegisterMaskPair &LI : MBB.liveins()) {

    for (MachineBasicBlock *InlineAsmMBB : InlineAsmMBBs) {
      if (!InlineAsmMBB->isLiveIn(LI.PhysReg))
        InlineAsmMBB->addLiveIn(LI.PhysReg, LI.LaneMask);
    }

    if (!TailMBB->isLiveIn(LI.PhysReg))
      TailMBB->addLiveIn(LI.PhysReg, LI.LaneMask);

    LLVM_DEBUG(dbgs() << "[rewire] copied original live-in to generated blocks: "
                      << printReg(LI.PhysReg, TRI) << "\n");
  }

  // ------------------------------------------------------------
  // Add parsed inline-asm physical input registers as live-ins conservatively.
  // ------------------------------------------------------------
  for (const InlineAsmLineInstrRecord &Record : InlineAsmInstructionsRecords) {
    for (const InlineAsmOperand &Op : Record.Operands) {
      LLVM_DEBUG({dbgs() << "[rewire][livein-check] Op.Reg=" << printReg(Op.Reg, TRI)
              << " Kind=" << Op.Kind << "\n";});

      if (Op.Kind == InlineAsmOperand::OK_Reg && Op.Reg.isPhysical()) {

        for (MachineBasicBlock *InlineAsmMBB : InlineAsmMBBs) {
          if (!InlineAsmMBB->isLiveIn(Op.Reg)){
            InlineAsmMBB->addLiveIn(Op.Reg);
          }
        }

        if (!TailMBB->isLiveIn(Op.Reg)){
          TailMBB->addLiveIn(Op.Reg);
        }

        LLVM_DEBUG(dbgs() << "[rewire] added parsed inline-asm physreg live-in: "
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

    for (MachineBasicBlock *InlineAsmMBB : InlineAsmMBBs) {
      if (!InlineAsmMBB->isLiveIn(R))
        InlineAsmMBB->addLiveIn(R);
    }

    if (!TailMBB->isLiveIn(R))
      TailMBB->addLiveIn(R);

    LLVM_DEBUG(
        dbgs() << "[rewire] added tail-used physreg live-in to generated blocks: "
              << printReg(R, TRI) << "\n");
  }

  // Move everything AFTER the INLINEASM into TailMBB.
  // This handles instructions after the inline asm that were originally
  // in the same MBB.
  LLVM_DEBUG(dbgs() << "[rewire] About to splice instructions after INLINEASM "
                       "into TailMBB\n");

  /* Move the original post-INLINEASM instruction range into the new tail. */
  TailMBB->splice(TailMBB->end(), &MBB, std::next(MBBI), MBB.end());

  LLVM_DEBUG(
      dbgs() << "[rewire] spliced instructions after INLINEASM into TailMBB\n");

  // TailMBB inherits MBB's original successors.
  LLVM_DEBUG(dbgs() << "[rewire] About to transfer successors from original "
                       "MBB to TailMBB\n");

  /* Transfer the old outgoing edges and repair successor PHI operands. */
  TailMBB->transferSuccessorsAndUpdatePHIs(&MBB);

  LLVM_DEBUG(
      dbgs()
      << "[rewire] transferred original successors from MBB to TailMBB\n");

  // Erase the original INLINEASM. Everything before it stays in MBB.
  LLVM_DEBUG(dbgs() << "[rewire] About to erase original INLINEASM\n");

  /* Remove the opaque instruction now that its replacement blocks exist. */
  MBB.erase(MBBI);

  LLVM_DEBUG(dbgs() << "[rewire] erased original INLINEASM\n");
  
  // Original MBB now branches to the first inline-asm block.
  /* Look up the generated block containing the first parsed instruction. */
  MachineBasicBlock *EntryMBB = RecordIndexToMBB[0];

  LLVM_DEBUG(dbgs()
      << "[rewire] About to insert PseudoBR from original MBB to entry MBB #"
      << EntryMBB->getNumber() << "\n");

  /* Emit an explicit jump from the original block into the expanded sequence. */
  BuildMI(&MBB, DebugLoc(), TII->get(RISCV::PseudoBR)).addMBB(EntryMBB);

  LLVM_DEBUG(dbgs() << "[rewire] inserted PseudoBR from original MBB to entry MBB #"
            << EntryMBB->getNumber() << "\n");

  /* Materialize each parsed instruction in the block assigned to its record. */
  for (int I = 0; I < NumRecords; ++I) {
    const InlineAsmLineInstrRecord &Record =
        InlineAsmInstructionsRecords[I];

    MachineBasicBlock *DstMBB = RecordIndexToMBB[I];



    LLVM_DEBUG(dbgs() << "[rewire] About to BuildMI record=" << I
                  << " opcode=" << Record.Opc
                  << " DstMBB #" << DstMBB->getNumber() << "\n");

    MachineInstrBuilder MIB = BuildMI(*DstMBB, DstMBB->end(), DL, TII->get(Record.Opc));

    LLVM_DEBUG(dbgs() << "[rewire] About to add operands for opcode="
                      << Record.Opc << "\n");

    /* Translate parsed operand kinds into MachineInstrBuilder operands. */
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
        if(!isBranchOpc(Record.Opc)){
          LLVM_DEBUG(dbgs() << "[isBranchOpc] Returning false for isBranch" << Record.Opc << "\n");
          MIB.addImm(Op.Imm);
          LLVM_DEBUG(dbgs() << "[rewire]   addImm " << Op.Imm << "\n");

        }
        break;
      case InlineAsmOperand::OK_Invalid:
        llvm_unreachable("invalid inline asm operand reached emission");
        
      }
    }

    /* Resolve a recorded branch label and append its MachineBasicBlock operand. */
    if (isBranchOpc(Record.Opc)) {
      int TargetRecordIndex =
          resolveBranchTarget(Record.BranchTarget, I);

      MachineBasicBlock *TargetMBB =
          RecordIndexToMBB[TargetRecordIndex];

      LLVM_DEBUG(
          dbgs() << "[rewire] branch '" << Record.BranchTarget
                << "' from record " << I
                << " targets record " << TargetRecordIndex
                << " / MBB #" << TargetMBB->getNumber() << "\n");

      MIB.addMBB(TargetMBB);
    }

    /* Dump the completed instruction and compare descriptor operand counts. */
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

  /* Add the CFG edge corresponding to the explicit entry jump. */
  // The original MBB enters the first inline-asm block.
  // MachineBasicBlock *EntryMBB = RecordIndexToMBB[0];
  MBB.addSuccessor(EntryMBB);

  LLVM_DEBUG(
      dbgs() << "[rewire] Added original MBB -> inline-asm entry MBB #"
            << EntryMBB->getNumber() << "\n");

  /* Derive each generated block's branch-target and fallthrough successors. */
  for (size_t BlockIndex = 0; BlockIndex + 1 < BlockStartRecordIndices.size(); ++BlockIndex) {

    int StartRecordIndex = BlockStartRecordIndices[BlockIndex];
    int EndRecordIndex = BlockStartRecordIndices[BlockIndex + 1];

    MachineBasicBlock *SrcMBB = RecordIndexToMBB[StartRecordIndex];

    MachineBasicBlock *FallthroughMBB = RecordIndexToMBB[EndRecordIndex];

    int LastRecordIndex = EndRecordIndex - 1;

    const InlineAsmLineInstrRecord &LastRecord = InlineAsmInstructionsRecords[LastRecordIndex];

    if (isConditionalBranchOpc(LastRecord.Opc)) {
      int TargetRecordIndex = resolveBranchTarget(LastRecord.BranchTarget, LastRecordIndex);

      MachineBasicBlock *TargetMBB = RecordIndexToMBB[TargetRecordIndex];

      SrcMBB->addSuccessor(TargetMBB);

      LLVM_DEBUG(dbgs() << "[rewire] Added conditional branch successor MBB #"
                        << SrcMBB->getNumber()
                        << " -> MBB #" << TargetMBB->getNumber() << "\n");

      if (TargetMBB != FallthroughMBB) {
        SrcMBB->addSuccessor(FallthroughMBB);

        LLVM_DEBUG(dbgs() << "[rewire] Added conditional fallthrough successor MBB #"
                          << SrcMBB->getNumber()
                          << " -> MBB #" << FallthroughMBB->getNumber() << "\n");
      }
    } else if (isUnconditionalBranchOpc(LastRecord.Opc)) {
      int TargetRecordIndex = resolveBranchTarget(LastRecord.BranchTarget, LastRecordIndex);

      MachineBasicBlock *TargetMBB = RecordIndexToMBB[TargetRecordIndex];

      SrcMBB->addSuccessor(TargetMBB);

      LLVM_DEBUG(dbgs() << "[rewire] Added unconditional branch successor MBB #"
                        << SrcMBB->getNumber()
                        << " -> MBB #" << TargetMBB->getNumber() << "\n");
    } else {
      SrcMBB->addSuccessor(FallthroughMBB);

      LLVM_DEBUG(dbgs() << "[rewire] Added fallthrough successor MBB #"
                        << SrcMBB->getNumber()
                        << " -> MBB #" << FallthroughMBB->getNumber() << "\n");
    }
}

  /* Report final live-in sets for the source, generated, and tail blocks. */
  LLVM_DEBUG({
    dbgs() << "[rewire] Final original MBB liveins:";
    for (const MachineBasicBlock::RegisterMaskPair &LI : MBB.liveins())
      dbgs() << " " << printReg(LI.PhysReg, TRI);
    dbgs() << "\n";

    for (MachineBasicBlock *InlineAsmMBB : InlineAsmMBBs) {
      dbgs() << "[rewire] Final inline-asm MBB #"
            << InlineAsmMBB->getNumber() << " liveins:";

      for (const MachineBasicBlock::RegisterMaskPair &LI :
          InlineAsmMBB->liveins())
        dbgs() << " " << printReg(LI.PhysReg, TRI);

      dbgs() << "\n";
    }

    dbgs() << "[rewire] Final TailMBB liveins:";
    for (const MachineBasicBlock::RegisterMaskPair &LI : TailMBB->liveins())
      dbgs() << " " << printReg(LI.PhysReg, TRI);
    dbgs() << "\n";
  });
  /* Report that the INLINEASM and its CFG were successfully replaced. */
  return true;
}

/*--------------------------------------------------------------------------*/
/* buildInlineAsmLineRecords:
   Parses a single line of expanded inline assembly text and appends a
   corresponding InlineAsmLineInstrRecord to InlineAsmInstructionsRecords.
   - Line: One line of asm text that may still contain a trailing comment or a
           leading label; this function strips and records those parts.

   Processing steps:
     1. Strips any trailing '#' comment from the line.
     2. Strips a leading numeric local label or dot-local label that shares
        the line with an instruction (e.g. "0: lr.w a0, (a1)").
     3. Skips pure label-only lines (e.g. ".Ltmp0:" or "1:").
     4. Splits the line into a mnemonic and the remaining operand text.
     5. Calls mnemonicToOpcode() to map the mnemonic to a RISC-V opcode; if
        the mnemonic is unrecognised, marks the complete parse as failed.
     6. Splits the operand text on commas and calls parseOperand() for each
        token, skipping branch-target tokens (local labels / Nf/Nb refs)
        because those are resolved from CFG context by the caller.
     7. For zero-register branches (bnez / beqz / bltz), synthesizes an
        explicit X0 operand after the first source register so BuildMI
        receives the full two-source-register form expected by BNE/BEQ/BLT.
     8. Pushes the completed record onto InlineAsmInstructionsRecords. */
void RISCVExpandINLINEASM::buildInlineAsmLineRecords(StringRef Line) {
  LLVM_DEBUG(dbgs() << "\n[record] ENTER buildInlineAsmLineRecords line='"
                    << Line << "'\n");

  /* Remove a trailing assembly comment and normalize surrounding whitespace. */
  // Strip trailing comment.
  if (auto H = Line.find('#'); H != StringRef::npos)
    Line = Line.substr(0, H);

  Line = Line.trim();

  LLVM_DEBUG(dbgs() << "[record] after strip/trim line='" << Line << "'\n");

  if (Line.empty()) {
    LLVM_DEBUG(dbgs() << "[record] empty line, return\n");
    return;
  }

  
  /* Find a label separator while ignoring colons inside braced expressions. */
  size_t Colon = StringRef::npos;
  unsigned BraceDepth = 0;

  for (size_t I = 0; I < Line.size(); ++I) {
    if (Line[I] == '{') {
      ++BraceDepth;
    } else if (Line[I] == '}') {
      if (BraceDepth != 0)
        --BraceDepth;
    } else if (Line[I] == ':' && BraceDepth == 0) {
      Colon = I;
      break;
    }
  }

  /* Record a leading label at the index of the instruction that follows it. */
  if (Colon != StringRef::npos) {
    StringRef MaybeLabel = Line.substr(0, Colon).trim();

    bool IsLabel =
        !MaybeLabel.empty() &&
        MaybeLabel.find_first_of(" \t,") == StringRef::npos;

    if (IsLabel) {
      InlineAsmLabelRecord Label;
      Label.Name = MaybeLabel.str();

      // This label points to the next instruction record.
      Label.RecordIndex = InlineAsmInstructionsRecords.size();

      InlineAsmLabels.push_back(std::move(Label));

      LLVM_DEBUG(
          dbgs() << "[record] recorded label '" << MaybeLabel
                << "' at record index "
                << InlineAsmInstructionsRecords.size() << "\n");

      Line = Line.substr(Colon + 1).trim();

      if (Line.empty())
        return;
    }
  }

  /* Ignore a remaining line that consists only of a label definition. */
  // Skip pure label lines like ".Ltmp0:" or "0:".
  if (Line.ends_with(":") &&
      Line.drop_back().find_first_of(" \t,") == StringRef::npos) {
    LLVM_DEBUG(dbgs() << "[record] pure label line, skip: '" << Line << "'\n");
    return;
  }

  /* Separate the mnemonic from the comma-delimited operand text. */
  // Split mnemonic from rest.
  size_t WS = Line.find_first_of(" \t");
  StringRef Mnemonic = (WS == StringRef::npos) ? Line : Line.substr(0, WS);
  StringRef Rest =
      (WS == StringRef::npos) ? StringRef() : Line.substr(WS).ltrim();
  LLVM_DEBUG(dbgs() << "[record] Mnemonic='" << Mnemonic << "' Rest='" << Rest
                    << "'\n");

  /* Convert the mnemonic and fail the complete parse if it is unsupported. */
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
    InlineAsmParsingFailed = true;
    return;
  }

  /* Detect aliases whose second source operand is implicitly the zero register. */
  bool IsZeroBranch =
      (Mnemonic == "bnez" || Mnemonic == "beqz" || Mnemonic == "bltz");
  LLVM_DEBUG(
    dbgs() << "[record] IsZeroBranch=" << IsZeroBranch << "\n");    

  /* Split the remaining text into the instruction's individual operands. */
  SmallVector<StringRef, 4> Toks;
  Rest.split(Toks, ',');
  LLVM_DEBUG(dbgs() << "[record] token count=" << Toks.size() << "\n");

  /* Supply the implicit return-address definition used by one-operand jal. */
  if (Mnemonic == "jal" && Toks.size() == 1) {
    InlineAsmOperand Dst;
    Dst.Kind = InlineAsmOperand::OK_DefReg;
    Dst.Reg = RISCV::X1;
    Rec.Operands.push_back(Dst);
  }
  /* Parse each explicit operand or capture the final branch-target token. */
  for (unsigned I = 0; I < Toks.size(); ++I) {
    StringRef Tok = Toks[I].trim();
    LLVM_DEBUG(dbgs() << "[record] token #" << I << " raw='" << Toks[I]
                      << "' trimmed='" << Tok << "'\n");
    if (Tok.empty())
      continue;

    /* Preserve branch labels separately so they can become MBB operands later. */
    // Drop the branch target operand — caller resolves it from CFG context.
    if (isBranchMnemonic(Mnemonic) && I == Toks.size() - 1) {
      Rec.BranchTarget = Tok.str();
      continue;
    }

    /* Identify instructions whose second token uses offset(base) addressing. */
    bool IsLoadStore =
        Mnemonic == "lb"  || Mnemonic == "lh"  ||
        Mnemonic == "lw"  || Mnemonic == "ld"  ||
        Mnemonic == "lbu" || Mnemonic == "lhu" ||
        Mnemonic == "lwu" ||
        Mnemonic == "sb"  || Mnemonic == "sh"  ||
        Mnemonic == "sw"  || Mnemonic == "sd";

    bool IsOffsetBaseOperand =
        IsLoadStore || Mnemonic == "jalr";

    /* Decompose an offset(base) token into a base-register and offset operand. */
    if (IsOffsetBaseOperand && I == 1) {
      size_t LParen = Tok.find('(');

      if (LParen == StringRef::npos || !Tok.ends_with(")")) {
        LLVM_DEBUG(
            dbgs() << "[record][unsupported] invalid load/store memory operand: '"
                  << Tok << "'\n");
        InlineAsmParsingFailed = true;
        return;
      }

      StringRef OffsetText = Tok.substr(0, LParen).trim();

      StringRef BaseText =
          Tok.substr(LParen + 1, Tok.size() - LParen - 2).trim();

      Register BaseReg = parseRegName(BaseText);

      if (!BaseReg.isValid()) {
        LLVM_DEBUG(
            dbgs() << "[record][unsupported] invalid load/store base register: '"
                  << BaseText << "'\n");
        InlineAsmParsingFailed = true;
        return;
      }

      int64_t Offset = 0;

      if (!OffsetText.empty() &&
          OffsetText.getAsInteger(0, Offset)) {
        LLVM_DEBUG(
            dbgs() << "[record][unsupported] invalid load/store offset: '"
                  << OffsetText << "'\n");
        InlineAsmParsingFailed = true;
        return;
      }

      InlineAsmOperand BaseOp;
      BaseOp.Kind = InlineAsmOperand::OK_Reg;
      BaseOp.Reg = BaseReg;

      InlineAsmOperand OffsetOp;
      OffsetOp.Kind = InlineAsmOperand::OK_Imm;
      OffsetOp.Imm = Offset;

      Rec.Operands.push_back(BaseOp);
      Rec.Operands.push_back(OffsetOp);

      LLVM_DEBUG(
          dbgs() << "[record] parsed load/store address base="
                << BaseReg << " offset=" << Offset << "\n");

      continue;
    }
    /* Parse an ordinary register, immediate, fence, or LR/SC address operand. */
    LLVM_DEBUG(dbgs() << "[record] About to parse operand token='" << Tok
                      << "' opIdx=" << I << "\n");

    InlineAsmOperand ParsedOp = parseOperand(Tok, Mnemonic, I);

    if (ParsedOp.Kind == InlineAsmOperand::OK_Invalid) {
      LLVM_DEBUG(
          dbgs() << "[record][unsupported] failed to parse operand '"
                << Tok << "' for mnemonic '" << Mnemonic << "'\n");

      InlineAsmParsingFailed = true;
      return;
    }
    Rec.Operands.push_back(ParsedOp);

    LLVM_DEBUG(dbgs() << "[record] Rec operands now="
                      << Rec.Operands.size() << "\n");

    /* Expand zero-branch aliases into the two-register form expected by BuildMI. */
    // For bnez/beqz: synthesize the implicit zero register after the first op.
    if (IsZeroBranch && I == 0) {
      InlineAsmOperand Z;
      Z.Kind = InlineAsmOperand::OK_Reg;
      Z.Reg = RISCV::X0;
      Rec.Operands.push_back(Z);
      LLVM_DEBUG(
          dbgs() << "[record] Synthesized zero operand for zero-branch\n");
    }
  }

  /* Reject a branch record whose target token was absent or not recognized. */
  if (isBranchMnemonic(Mnemonic) && Rec.BranchTarget.empty()) {
    LLVM_DEBUG(
        dbgs() << "[record][unsupported] branch has no recognized target: "
              << Line << "\n");

    InlineAsmParsingFailed = true;
    return;
  }

  /* Commit the completed instruction record to the current parse result. */
  LLVM_DEBUG(dbgs() << "[record] About to push record opcode=" << Rec.Opc
                    << " operands=" << Rec.Operands.size() << "\n");
  InlineAsmInstructionsRecords.push_back(std::move(Rec));
  LLVM_DEBUG(dbgs() << "[record] records total="
                    << InlineAsmInstructionsRecords.size() << "\n");
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
                    OK_Reg, or OK_Invalid if the register is invalid.
     2. fence arg - If mnemonic is "fence", encodes the ordering string
                    ("r", "w", "rw", "iorw", etc.) as a bitmask immediate via
                    encodeFenceArg() and returns OK_Imm.
     3. Numeric   - Decimal or hex integer literal; returns OK_Imm.
     4. Register  - Calls parseRegName(); returns OK_DefReg for operand zero of
                    a mnemonic recognized by hasDefOperand(), OK_Reg otherwise,
                    and OK_Invalid if the register name is unrecognised. */
InlineAsmOperand RISCVExpandINLINEASM::parseOperand(StringRef Tok,
                                                    StringRef Mnemonic,
                                                    unsigned OpIdx) {
  LLVM_DEBUG(dbgs() << "[parseOperand] ENTER Tok='" << Tok << "' Mnemonic='"
                    << Mnemonic << "' OpIdx=" << OpIdx << "\n");
  /* Initialize a safely invalid operand before attempting each supported form. */
  InlineAsmOperand Op;
  Op.Imm = 0;
  Op.Reg = Register();

  /* Parse the parenthesized base-register form used by LR and SC instructions. */
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
    return InlineAsmOperand();
    }
    return Op;
  }

  /* Convert a FENCE ordering token into the instruction's bitmask immediate. */
  // Fence ordering: "rw", "r", "w", "iorw", ... encoded as imm.
  if (Mnemonic == "fence") {
    LLVM_DEBUG(dbgs() << "[parseOperand] fence operand\n");
    Op.Kind = InlineAsmOperand::OK_Imm;
    Op.Imm = encodeFenceArg(Tok);
    LLVM_DEBUG(dbgs() << "[parseOperand] fence encoded imm=" << Op.Imm << "\n");
    return Op;
  }

  /* Accept a numeric literal as an immediate operand. */
  // Numeric immediate.
  int64_t ImmVal;
  if (!Tok.getAsInteger(0, ImmVal)) {
    LLVM_DEBUG(dbgs() << "[parseOperand] numeric immediate=" << ImmVal << "\n");
    Op.Kind = InlineAsmOperand::OK_Imm;
    Op.Imm = ImmVal;
    return Op;
  }

  /* Resolve a symbolic register name and reject any unknown token. */
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
    return InlineAsmOperand();
  }

  /* Mark operand zero as a definition when the mnemonic produces a register. */
  bool IsDef = OpIdx == 0 && hasDefOperand(Mnemonic);
  Op.Kind = IsDef ? InlineAsmOperand::OK_DefReg : InlineAsmOperand::OK_Reg;
  Op.Reg = R;
  LLVM_DEBUG(dbgs() << "[parseOperand] returning kind=" << Op.Kind
                    << " reg=" << Op.Reg << " imm=" << Op.Imm << "\n");
  return Op;
}
/*--------------------------------------------------------------------------*/
/* hasDefOperand:
   Reports whether the first explicit operand of a mnemonic is a definition.
   - Mnemonic: Lowercase RISC-V instruction name being parsed.
   Covers LR/SC, integer arithmetic, immediate arithmetic, loads, JAL, and
   JALR so parseOperand() can emit operand zero with OK_DefReg. */
bool RISCVExpandINLINEASM::hasDefOperand(StringRef Mnemonic) {

  /* Recognize atomic instructions whose first register receives a result. */
  return Mnemonic.starts_with("lr.") ||
      Mnemonic.starts_with("sc.") ||
      /* Recognize register-register arithmetic and comparison definitions. */
      Mnemonic == "add"  ||
      Mnemonic == "sub"  ||
      Mnemonic == "xor"  ||
      Mnemonic == "or"   ||
      Mnemonic == "and"  ||
      Mnemonic == "sll"  ||
      Mnemonic == "srl"  ||
      Mnemonic == "sra"  ||
      Mnemonic == "slt"  ||
      Mnemonic == "sltu" ||
      /* Recognize immediate arithmetic and comparison definitions. */
      Mnemonic == "addi" ||
      Mnemonic == "xori" ||
      Mnemonic == "ori"  ||
      Mnemonic == "andi" ||
      Mnemonic == "slli" ||
      Mnemonic == "srli" ||
      Mnemonic == "srai" ||
      Mnemonic == "slti" ||
      Mnemonic == "sltiu" ||
      /* Recognize load instructions whose first register receives memory data. */
      Mnemonic == "lb"  ||
      Mnemonic == "lh"  ||
      Mnemonic == "lw"  ||
      Mnemonic == "ld"  ||
      Mnemonic == "lbu" ||
      Mnemonic == "lhu" ||
      Mnemonic == "lwu" ||
      /* Recognize direct and indirect jumps that write a link register. */
      Mnemonic == "jal" ||
      Mnemonic == "jalr";
}
/*--------------------------------------------------------------------------*/
/* isBranchOpc:
   Returns true if the given RISC-V opcode is a conditional or unconditional
   direct branch that this pass can route to a MachineBasicBlock target.
   - Opc: A RISC-V MC opcode value.
   Used to decide whether an InlineAsmLineInstrRecord needs a MBB operand
   appended during CFG rewiring, and to detect whether the LR/SC sequence
   contains a conditional path between the LR and SC instructions. */
bool RISCVExpandINLINEASM::isBranchOpc(unsigned Opc) {
  /* Combine the supported conditional and unconditional direct-branch sets. */
  return isConditionalBranchOpc(Opc) ||
         isUnconditionalBranchOpc(Opc);
}
/*--------------------------------------------------------------------------*/
/* isConditionalBranchOpc:
   Reports whether an opcode is a supported conditional RISC-V branch.
   - Opc: Opcode checked against the BEQ/BNE and signed or unsigned relational
          branch families.
   Returns false for all non-conditional and unsupported opcodes. */
bool RISCVExpandINLINEASM::isConditionalBranchOpc(unsigned Opc) {
  switch (Opc) {
  case RISCV::BNE:
  case RISCV::BEQ:
  case RISCV::BLT:
  case RISCV::BLTU:
  case RISCV::BGE:
  case RISCV::BGEU:
    return true;

  default:
    return false;
  }
}
/*--------------------------------------------------------------------------*/
/* isUnconditionalBranchOpc:
   Reports whether an opcode is a supported unconditional direct branch.
   - Opc: Opcode tested for the PseudoBR and JAL forms emitted by this pass.
   JALR is excluded because its destination is a register rather than a local
   inline-assembly label resolved to a MachineBasicBlock. */
bool RISCVExpandINLINEASM::isUnconditionalBranchOpc(unsigned Opc) {
  return Opc == RISCV::PseudoBR ||
         Opc == RISCV::JAL;
}



/*--------------------------------------------------------------------------*/
/* isBranchMnemonic:
   Returns true if the mnemonic is a conditional branch or direct jump
   instruction recognised by this pass.
   - M: Lowercase mnemonic text (e.g. "bne", "bnez", "bltz").
   Used during operand parsing to decide whether a comma-separated token
   should be tested as a branch-target label (and therefore skipped) rather
   than parsed as a register or immediate operand. */
bool RISCVExpandINLINEASM::isBranchMnemonic(StringRef M) {
  /* Match conditional forms, zero-register aliases, and direct jump forms. */
  return M == "bne" || M == "beq" || M == "bnez" || M == "beqz" ||
         M == "blt" || M == "bltz" || M == "bge" ||
         M == "bltu" || M == "bgeu" ||
         M == "j" || M == "jal";
}

/*--------------------------------------------------------------------------*/
/* resolveBranchTarget:
   Resolves an inline-assembly branch target to the instruction-record index
   associated with its label.

   Numeric directional targets:
     - "0b" selects the nearest matching preceding label.
     - "1f" selects the nearest matching following label.
     -> f= forward, b=backward, N=label number.
     Format: Nf/Nb where N is one or more digits and f/b indicates
     direction. The label number is matched against the numeric local labels
     recorded in InlineAsmLabels.

   Named targets such as ".Lretry" select the matching named label.

   Returns -1 when the target cannot be resolved. */
  int RISCVExpandINLINEASM::resolveBranchTarget(
      StringRef Target, int BranchRecordIndex) const {
    /* Normalize the target and reject an empty label reference. */
    Target = Target.trim();

    if (Target.empty())
      return -1;

    /* Distinguish numeric Nf/Nb references from ordinary named labels. */
    bool IsNumericDirectional = (Target.size() >= 2) &&
        (Target.back() == 'b' || Target.back() == 'f') &&
        llvm::all_of(Target.drop_back(), [](char C) {
          return C >= '0' && C <= '9';});

    /* Resolve a named label directly without applying directional rules. */
    // Ordinary named label, such as "retry" or ".Lretry".
    if (!IsNumericDirectional) {
      for (const InlineAsmLabelRecord &Label : InlineAsmLabels) {
        if (StringRef(Label.Name) == Target)
          return Label.RecordIndex;
      }

      return -1;
    }

    /* Separate a numeric label name from its forward or backward direction. */
    char Direction = Target.back();
    StringRef LabelName = Target.drop_back();

    /* Search backward labels from newest to oldest for the nearest match. */
    if (Direction == 'b') {
      for (auto It = InlineAsmLabels.rbegin(),
                End = InlineAsmLabels.rend();
          It != End; ++It) {
        if (StringRef(It->Name) == LabelName &&
            It->RecordIndex <= BranchRecordIndex)
          return It->RecordIndex;
      }

      return -1;
    }

    /* Search forward labels in source order for the first following match. */
    for (const InlineAsmLabelRecord &Label : InlineAsmLabels) {
      if (StringRef(Label.Name) == LabelName &&
          Label.RecordIndex > BranchRecordIndex)
        return Label.RecordIndex;
    }

    return -1;
  }

/*--------------------------------------------------------------------------*/
/* mnemonicToOpcode:
   Maps a lowercase RISC-V instruction mnemonic string to its corresponding
   RISC-V MC opcode integer.
   - M: Lowercase mnemonic text (e.g. "lr.w", "sc.d.aqrl", "bne", "fence").
   Covers LR/SC variants, integer arithmetic, loads and stores, conditional
   and unconditional control transfers, branch aliases, and FENCE.
   Returns 0 for any unrecognised mnemonic, which causes the caller
   (buildInlineAsmLineRecords) to reject the complete inline-asm parse. */
unsigned RISCVExpandINLINEASM::mnemonicToOpcode(StringRef M) {
  LLVM_DEBUG(dbgs() << "[mnemonicToOpcode] M='" << M << "'\n");
/* Map load-reserved mnemonics and their acquire/release variants. */
return StringSwitch<unsigned>(M)
    .Case("lr.w", RISCV::LR_W)
    .Case("lr.w.aq", RISCV::LR_W_AQ)
    .Case("lr.w.rl", RISCV::LR_W_RL)
    .Case("lr.w.aqrl", RISCV::LR_W_AQRL)
    .Case("lr.d", RISCV::LR_D)
    .Case("lr.d.aq", RISCV::LR_D_AQ)
    .Case("lr.d.rl", RISCV::LR_D_RL)
    .Case("lr.d.aqrl", RISCV::LR_D_AQRL)

    /* Map store-conditional mnemonics and their ordering variants. */
    .Case("sc.w", RISCV::SC_W)
    .Case("sc.w.aq", RISCV::SC_W_AQ)
    .Case("sc.w.rl", RISCV::SC_W_RL)
    .Case("sc.w.aqrl", RISCV::SC_W_AQRL)
    .Case("sc.d", RISCV::SC_D)
    .Case("sc.d.aq", RISCV::SC_D_AQ)
    .Case("sc.d.rl", RISCV::SC_D_RL)
    .Case("sc.d.aqrl", RISCV::SC_D_AQRL)

    /* Map register-register integer arithmetic and comparison operations. */
    // Register-register integer instructions.
    .Case("add", RISCV::ADD)
    .Case("sub", RISCV::SUB)
    .Case("xor", RISCV::XOR)
    .Case("or", RISCV::OR)
    .Case("and", RISCV::AND)
    .Case("sll", RISCV::SLL)
    .Case("srl", RISCV::SRL)
    .Case("sra", RISCV::SRA)
    .Case("slt", RISCV::SLT)
    .Case("sltu", RISCV::SLTU)

    /* Map immediate integer arithmetic, logical, shift, and comparison forms. */
    // Immediate integer instructions.
    .Case("addi", RISCV::ADDI)
    .Case("xori", RISCV::XORI)
    .Case("ori", RISCV::ORI)
    .Case("andi", RISCV::ANDI)
    .Case("slli", RISCV::SLLI)
    .Case("srli", RISCV::SRLI)
    .Case("srai", RISCV::SRAI)
    .Case("slti", RISCV::SLTI)
    .Case("sltiu", RISCV::SLTIU)

    /* Map integer loads and stores of the supported widths and signedness. */
    // Loads and stores.
    .Case("lb", RISCV::LB)
    .Case("lh", RISCV::LH)
    .Case("lw", RISCV::LW)
    .Case("ld", RISCV::LD)
    .Case("lbu", RISCV::LBU)
    .Case("lhu", RISCV::LHU)
    .Case("lwu", RISCV::LWU)

    .Case("sb", RISCV::SB)
    .Case("sh", RISCV::SH)
    .Case("sw", RISCV::SW)
    .Case("sd", RISCV::SD)

    /* Map conditional branches to their target instruction opcodes. */
    // Conditional branches.
    .Case("bne", RISCV::BNE)
    .Case("beq", RISCV::BEQ)
    .Case("blt", RISCV::BLT)
    .Case("bge", RISCV::BGE)
    .Case("bltu", RISCV::BLTU)
    .Case("bgeu", RISCV::BGEU)
    /* Map unconditional control transfers. */
    // Unconditional branches.
    .Case("j", RISCV::PseudoBR)
    .Case("jal", RISCV::JAL)
    .Case("jalr", RISCV::JALR)
    /* Lower zero-register branch aliases to their two-register base opcodes. */
    // Branch aliases.
    .Case("bnez", RISCV::BNE)
    .Case("beqz", RISCV::BEQ)
    .Case("bltz", RISCV::BLT)

    /* Map memory-ordering barriers and reject every unknown mnemonic. */
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
   causes parseOperand() to return an invalid operand and emit an unsupported
   debug message. */
Register RISCVExpandINLINEASM::parseRegName(StringRef N) {
  LLVM_DEBUG(dbgs() << "[parseRegName] N='" << N << "'\n");

  /* Map zero, return-address, stack, global, and thread-pointer names. */
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

      /* Map the first group of temporary-register names. */
      .Case("X5", RISCV::X5)
      .Case("x5", RISCV::X5)
      .Case("t0", RISCV::X5)
      .Case("X6", RISCV::X6)
      .Case("x6", RISCV::X6)
      .Case("t1", RISCV::X6)
      .Case("X7", RISCV::X7)
      .Case("x7", RISCV::X7)
      .Case("t2", RISCV::X7)

      /* Map frame-pointer and initial saved-register names. */
      .Case("X8", RISCV::X8)
      .Case("x8", RISCV::X8)
      .Case("s0", RISCV::X8)
      .Case("fp", RISCV::X8)
      .Case("X9", RISCV::X9)
      .Case("x9", RISCV::X9)
      .Case("s1", RISCV::X9)

      /* Map the eight argument and return-value register names. */
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

      /* Map the remaining callee-saved register names. */
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

      /* Map the final four temporary-register names. */
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

      /* Return an invalid Register when no spelling matches. */
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
  /* Start with no predecessor or successor classes selected. */
  int64_t V = 0;
  /* Accumulate the ISA-defined bit associated with each ordering character. */
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
  /* Return the completed four-bit ordering mask. */
  return V;
}

/*--------------------------------------------------------------------------*/
/* dumpStats:
   Emits a human-readable summary of pass statistics to the given output
   stream.
   - OS: The raw_ostream to write to (typically dbgs() from the destructor).
   Prints the module name recorded during the most recent runOnMachineFunction
   call and the number of InlineAsmLineInstrRecords that were parsed and
   stored in InlineAsmInstructionsRecords.
   Called unconditionally from the destructor when the
   -dump-expand-inline-asm-stat command-line flag is enabled. */
void RISCVExpandINLINEASM::dumpStats(raw_ostream &OS) {
  /* Print the module identity followed by the retained record count. */
  OS << "RISCVExpandINLINEASM stats for module " << ModuleName << "\n";
  OS << "  expanded records: " << InlineAsmInstructionsRecords.size() << "\n";
}