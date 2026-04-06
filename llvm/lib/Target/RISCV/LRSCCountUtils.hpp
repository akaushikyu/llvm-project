#include "RISCV.h"
#include "RISCVInstrInfo.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <map>
#include <string>
#include <vector>
using namespace llvm;

namespace utils {
/*--------------------------------------------------------------------------*/
/* LRSCCounts: Aggregates LR/SC counting data at multiple granularities:
  - Per MachineFunction (MF)
  - Per MachineBasicBlock (BB)
  - Per opcode "flavor" (string key) within each BB
  Also provides JSON serialization for downstream reporting. */
struct LRSCCounts {

  /*--------------------------------------------------------------------------*/
  /* Key types used for hash maps.
    - MFKey: identifies a function (by pointer identity)
    - BBKey: identifies a basic block (by pointer identity)
    - OpKey: identifies an opcode/flavor label (string) */

  using BBKey = const MachineBasicBlock *;
  using OpKey = std::string;

  /*--------------------------------------------------------------------------*/
  /* BBList: preserves an explicit traversal/iteration order of BB pointers
    for a given MachineFunction. This is used to emit stable bb_index values.
  */
  using BBList = std::vector<BBKey>;

  /* list of BB pointers in MF */
  BBList basicBlockOrder;

  /*--------------------------------------------------------------------------*/
  /* FlavourMap: opcode/flavor string -> count within a BB. */
  using FlavourMap = std::unordered_map<OpKey, int>;

  /*--------------------------------------------------------------------------*/
  /* BBToFlavourMap: BB -> (opcode/flavor -> count). */
  using BBToFlavourMap = std::unordered_map<BBKey, FlavourMap>;

  BBToFlavourMap BBFlavourCounts;

  /*--------------------------------------------------------------------------*/
  /* BBCountMap: BB -> total LR/SC count in that BB (definition-dependent). */
  using BBCountMap = std::unordered_map<BBKey, int>;

  /*--------------------------------------------------------------------------*/
  /* Func -> BB -> (opcode -> count)
    Stores per-basic-block opcode/flavor breakdowns per function. */
  BBCountMap BBCount;

  // Total LRSCCount per function
  unsigned int functionLRSCCount = 0;

  /* Map from function-name -> per-function object (totals + BB list). */
  llvm::json::Object FuncMap; // function-name -> { totals, basic_blocks }

  /*--------------------------------------------------------------------------*/
  /* Clears all stored data across every map. */
  void clearAll() {
    basicBlockOrder.clear();
    BBFlavourCounts.clear();
    BBCount.clear();
  }

  /*--------------------------------------------------------------------------*/
  /* Updates the per-function total count for the provided Func by adding
   * mfCount. */
  void updateFuncCnt(unsigned mfCount) { functionLRSCCount += mfCount; }

  /*--------------------------------------------------------------------------*/
  /* Updates the per-basic-block total count for (MF, MBB) by adding bbCount. */
  void updateBBCnt(const MachineBasicBlock &MBB, unsigned bbCount) {
    BBCount[&MBB] += bbCount;
  }

  /*--------------------------------------------------------------------------*/
  /* Increments the opcode/flavor counter for (MF, MBB, OpKey) by 1. */
  void updateBBFlavCnt(const MachineBasicBlock &MBB, std::string OpKey) {
    BBFlavourCounts[&MBB][OpKey]++;
  }

  /*--------------------------------------------------------------------------*/
  /* Serializes the entire structure to JSON in the following high-level shape:
      {
        "functions": {
          "<function_name>": {
            "total_lrsc_occurrences": <int>,
            "basic_blocks": [
              {
                "bb_index": <int>,
                "mbb_number": <int>,
                "bb_total_lrsc_occurrences": <int>,
                "flavors": { "<op>": <count>, ... }
              },
              ...
            ]
          },
          ...
        }
      } */

  llvm::json::Value getJSONObj() {
    llvm::json::Object Root;
    /* Attach the function map to the root object and return as a JSON value. */
    Root["function"] = std::move(FuncMap);
    return llvm::json::Value(std::move(Root));
  }

  // This is called per function
  void toJSON(MachineFunction &MF) {
    /* Root object for the JSON output. */
    llvm::json::Object Root;

    /* Array of per-basic-block JSON objects for this function. */
    llvm::json::Array Blocks;

    for (size_t i = 0; i < basicBlockOrder.size(); ++i) {
      /* BB pointer at this position in iteration order. */
      const MachineBasicBlock *MBB = basicBlockOrder[i];
      /* JSON object holding this BB's metadata and counts. */
      llvm::json::Object BBObj;

      /* Stable index based on traversal order in Order[]. */
      BBObj["bb_traversal_index"] = i;

      /* LLVM internal BB number, or -1 if MBB is null. */
      BBObj["mbb_number"] = MBB ? static_cast<int64_t>(MBB->getNumber()) : -1;

      // lambda function to calculate MBB size in insn count
      auto getMBBSize = [](const MachineBasicBlock *MBB) {
        unsigned cnt = 0;
        for (auto &MI : *MBB) {
          cnt++;
        }
        return cnt;
      };

      BBObj["num_insn"] = getMBBSize(MBB);
      BBObj["lrsc_count"] = MBB ? BBCount[MBB] : 0;

      /* Build a JSON object of opcode/flavor counts for this BB. */
      llvm::json::Object FlavoursObj;
      auto bbit = BBFlavourCounts.find(MBB);
      if (bbit != BBFlavourCounts.end()) {
        for (const auto &OP : bbit->second) {
          FlavoursObj.try_emplace(OP.first, OP.second);
        }
      }

      /* Attach the flavors object to the BB object. */
      BBObj["flavors"] = std::move(FlavoursObj);

      /* Append the BB object to the basic_blocks array. */
      Blocks.push_back(std::move(BBObj));
    }

    /* Build the per-function JSON object with total and BB list. */
    llvm::json::Object FObj;
    FObj["total_lrsc_occurrences"] = functionLRSCCount;
    FObj["basic_blocks"] = std::move(Blocks);

    /* Insert function object into the function map keyed by function name. */
    FuncMap.try_emplace(std::move(MF.getName()), std::move(FObj));
  }
};
//----------------------------------------------------------------------
// 1. PUBLIC DATA TYPES — plain structs, callers need these
//----------------------------------------------------------------------
/*--------------------------------------------------------------------------*/
/* CycleEntry: Represents a single detected cycle in the CFG.
  - cyclePath:        Ordered list of basic block numbers forming the cycle.
  - isLRCycle:        True if the origin LR block participates in this cycle.
  - instructionCount: Estimated total instruction count along the cycle path.
*/
struct CycleEntry {
  std::vector<std::string> cyclePath;
  bool isLRCycle;
  int instructionCount;
};

/*--------------------------------------------------------------------------*/
/* MatchTuple: Represents a single matched LR/SC pair found during DFS.
  - lrInstruction:        String of the base register (rs1) of the LR.
  - matchedSCInstruction: Opcode name of the matched SC instruction.
  - longestDistance:      Longest instruction distance found between
                          the LR and the SC across all DFS paths. */
struct MatchTuple {
  std::string lrInstruction;
  std::string matchedSCInstruction;
  int longestDistance;
};

/*--------------------------------------------------------------------------*/
/* MatchResult: Aggregated result of the LR/SC analysis for a MachineFunction.
  - matches: All valid LR/SC pairs found during the DFS traversal.
  - cycles:  All CFG cycles detected that include the origin LR block. */
struct MatchResult {
  std::vector<MatchTuple> matches;
  std::vector<CycleEntry> cycles;
};
//----------------------------------------------------------------------
// 2. STATELESS HELPERS — free functions in a namespace, not methods
//----------------------------------------------------------------------
namespace lrsc { // namespace not class: these functions share no state between
                 // calls
/*--------------------------------------------------------------------------*/
/* HELPERS */
/*--------------------------------------------------------------------------*/
/*--------------------------------------------------------------------------*/
/* LRSCWidth: Represents the data-width qualifier of an LR or SC instruction.
  - W:       Word (32-bit) variant, corresponding to LR.W / SC.W opcodes.
  - D:       Doubleword (64-bit) variant, corresponding to LR.D / SC.D opcodes.
  - Unknown: Sentinel value returned when the opcode is not a valid LR or SC
              instruction. Guards against undefined behavior from falling off
              the end of getLRSCWidth() and prevents false width matches in
              the DFS traversal.
  Used in LRSCSearchMetaData to match an LR against its corresponding SC —
  a valid LR/SC pair must share the same width. Stored as uint8_t to avoid
  heap allocation and string comparison overhead of the previous std::string
  representation. */
enum class LRSCWidth : uint8_t {
  W,      // word (32-bit)
  D,      // doubleword (64-bit)
  Unknown // not a valid LR/SC opcode
};
/*--------------------------------------------------------------------------*/
/* isLR: Returns true if opc is any LR (Load-Reserved) variant.
  Covers all eight opcodes: LR_W, LR_D, and their AQ/RL/AQRL combinations. */
bool isLR(uint16_t opc) {
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
    return true;
  }
  return false;
}

/*--------------------------------------------------------------------------*/
/* isSC: Returns true if opc is any SC (Store-Conditional) variant.
  Covers all eight opcodes: SC_W, SC_D, and their AQ/RL/AQRL combinations. */
bool isSC(uint16_t opc) {
  switch (opc) {
  // SC flavours
  case RISCV::SC_W:
  case RISCV::SC_D:
  case RISCV::SC_D_AQ:
  case RISCV::SC_W_AQ:
  case RISCV::SC_D_RL:
  case RISCV::SC_W_RL:
  case RISCV::SC_D_AQRL:
  case RISCV::SC_W_AQRL:
    return true;
  }
  return false;
}

/*--------------------------------------------------------------------------*/
/* getLRSCWidth: Returns the width qualifier string for a given LR or SC
  opcode.
  - Returns "W" for word-width variants.
  - Returns "D" for doubleword-width variants. */
LRSCWidth getLRSCWidth(uint16_t opc) {
  switch (opc) {
  case RISCV::LR_W:
  case RISCV::LR_W_AQ:
  case RISCV::LR_W_RL:
  case RISCV::LR_W_AQRL:
  case RISCV::SC_W:
  case RISCV::SC_W_AQ:
  case RISCV::SC_W_RL:
  case RISCV::SC_W_AQRL:
    return LRSCWidth::W;
    break;

  case RISCV::LR_D:
  case RISCV::LR_D_AQ:
  case RISCV::LR_D_RL:
  case RISCV::LR_D_AQRL:
  case RISCV::SC_D:
  case RISCV::SC_D_AQ:
  case RISCV::SC_D_RL:
  case RISCV::SC_D_AQRL:
    return LRSCWidth::D;
    break;

  default:
    return LRSCWidth::Unknown;
    break;
  }
}

/*--------------------------------------------------------------------------*/
/* stringifyOpcode: Returns the string name of a given LR or SC opcode.
  Used when recording matched SC instructions in a MatchTuple.
  Returns the opcode name (e.g. "LR_W_AQRL"), or "" if unknown. */
std::string stringifyOpcode(uint16_t opc) {
  switch (opc) {
  // LR flavours
  case RISCV::LR_W:
    return "LR_W";
  case RISCV::LR_D:
    return "LR_D";
  case RISCV::LR_D_AQ:
    return "LR_D_AQ";
  case RISCV::LR_W_AQ:
    return "LR_W_AQ";
  case RISCV::LR_D_RL:
    return "LR_D_RL";
  case RISCV::LR_W_RL:
    return "LR_W_RL";
  case RISCV::LR_D_AQRL:
    return "LR_D_AQRL";
  case RISCV::LR_W_AQRL:
    return "LR_W_AQRL";

  // SC flavours
  case RISCV::SC_W:
    return "SC_W";
  case RISCV::SC_D:
    return "SC_D";
  case RISCV::SC_D_AQ:
    return "SC_D_AQ";
  case RISCV::SC_W_AQ:
    return "SC_W_AQ";
  case RISCV::SC_D_RL:
    return "SC_D_RL";
  case RISCV::SC_W_RL:
    return "SC_W_RL";
  case RISCV::SC_D_AQRL:
    return "SC_D_AQRL";
  case RISCV::SC_W_AQRL:
    return "SC_W_AQRL";

  default:
    return "";
  }
}

/*--------------------------------------------------------------------------*/
/* stringifyWidth: Returns a human-readable string representation of an
  LRSCWidth value for use in debug logging.
  - W:       Returns "W"
  - D:       Returns "D"
  - Unknown: Returns "Unknown"
  Used exclusively with LLVM_DEBUG to print LRSCWidth values via dbgs(),
  since enum class disables implicit conversion to string. */
llvm::StringRef stringifyWidth(LRSCWidth w) {
  switch (w) {
  case LRSCWidth::W:
    return "W";
  case LRSCWidth::D:
    return "D";
  case LRSCWidth::Unknown:
    return "Unknown";
  }
  return "Unknown"; // ← add this
}

/*--------------------------------------------------------------------------*/
/* getRegString: Returns the string representation of the register at a given
  operand index in MI, using the target register info from MF.
  Defaults to operand index 1 (the base register for LR/SC).

  Variables:
  - rs1:    Physical register extracted from the operand at operandIdx;
            passed to printReg for string conversion.
  - TRI:    Target register info from MF's subtarget; required by printReg
            to resolve register names.
  - regStr: Destination string that accumulates the printed register name.
  - rso:    String stream backed by regStr; used to write the register name
            via the printReg streaming operator. */
std::string getRegString(MachineInstr &MI, MachineFunction &MF,
                         unsigned operandIdx = 1) {
  Register rs1 = MI.getOperand(operandIdx).getReg();
  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
  std::string regStr;
  llvm::raw_string_ostream rso(regStr);
  rso << printReg(rs1, TRI);
  rso.flush();
  return regStr;
}

/*--------------------------------------------------------------------------*/
/* defsReg: Returns true if MI defines (writes) the specified register.
  Iterates over all def operands of MI and compares their printed name
  against reg.

  Variables:
  - MF:     MachineFunction owning MI; retrieved via the parent basic block
            and used to obtain TargetRegisterInfo.
  - TRI:    Target register info from MF's subtarget; required by printReg
            to resolve register names for comparison.
  - defReg: Destination string accumulating the printed name of the current
            def register operand.
  - rso:    String stream backed by defReg; used to write the register name
            via the printReg streaming operator. */
bool defsReg(MachineInstr &MI, const std::string &reg) {
  MachineFunction *MF = MI.getParent()->getParent();
  const TargetRegisterInfo *TRI = MF->getSubtarget().getRegisterInfo();
  for (auto &MO : MI.operands()) {
    if (MO.isReg() && MO.isDef()) {
      std::string defReg;
      llvm::raw_string_ostream rso(defReg);
      rso << printReg(MO.getReg(), TRI);
      rso.flush();
      if (defReg == reg)
        return true;
    }
  }
  return false;
}

/*--------------------------------------------------------------------------*/
/* getMBBInstrCount: Returns the total number of instructions in MBB.

  Variables:
  - count: Running tally of instructions seen so far in MBB. */
int getMBBInstrCount(const MachineBasicBlock *MBB) {
  int count = 0;
  for (auto &MI : *MBB) {
    count++;
  }
  return count;
}
/*--------------------------------------------------------------------------*/
/* dump: Prints all matches and cycles in a MatchResult to stderr.
For each match: prints the LR base register, matched SC opcode, and
longest distance. For each cycle: prints the block path, whether it is
an LR cycle, the total instruction count, and the basic block count. */
void dump(const MatchResult &result) {
  LLVM_DEBUG({
    dbgs() << "=== Matches ===\n";
    for (auto &m : result.matches)
      dbgs() << "LR: " << m.lrInstruction
             << " SC: " << m.matchedSCInstruction
             << " Distance: " << m.longestDistance << "\n";

    dbgs() << "=== Cycles ===\n";
    for (auto &c : result.cycles) {
      dbgs() << "Cycle: ";
      for (auto &block : c.cyclePath)
        dbgs() << block << " -> ";
      dbgs() << (c.isLRCycle ? "[LR cycle]" : "[no LR]")
             << " InstrCount: " << c.instructionCount << ", "
             << " BasicBlockCount: " << c.cyclePath.size() << "\n";
    }
  });
}

} // namespace lrsc

//----------------------------------------------------------------------
// 3. STATEFUL ALGORITHM — class, because it owns mutable state
//    and the DFS impl structs are private details nobody else needs
//----------------------------------------------------------------------
class LRSCAnalyzer { /* class not struct: DFS impl structs must be hidden
  because DFSTraversalState/DFSOutput hold references that dangle if constructed
  outside the DFS call stack so private enforces that only the class builds them
  correctly*/
public:
  /*--------------------------------------------------------------------------*/
  /* computeLRSCDistancesAndCycles: Entry point for LR/SC distance and cycle
    analysis. Iterates over all basic blocks in MF. For each LR instruction
    found, initializes DFS state and launches a DFS traversal from the
    instruction immediately following the LR. If a valid matching SC is found,
    the result is pushed into result.matches.

    Variables:
    - visitedDistance:    Maps (BasicBlock*, rs1Clobbered) → best instruction
                          distance seen so far; used by DFS for revisit
    pruning.
    - exploredBlockCount: Counts distinct basic blocks visited in the current
                          DFS; bounded by MaxBlocks to limit exploration.
    - longestDistance:    Longest LR-to-SC distance found across all DFS paths
                          for the current LR; -1 means no matching SC yet.
    - visitedBlocksBuffer: Ordered list of block numbers on the current DFS
    path; used for cycle detection and path reconstruction.
    - searchMetaData:                Static search context constructed per LR;
    holds lrscWidth, baseReg, and originLRBlock.
    - state:              Initial DFS traversal state constructed per LR; holds
                          rs1Clobbered, currentDistance, currentDepth, and
                          references to visitedBlocksBuffer and visitedDistance.
    - out:                Output accumulators shared across the DFS; holds
                          references to exploredBlockCount, longestDistance,
                          result.cycles, and the current MatchTuple entry.
    - startIt:            Iterator to the instruction immediately after the LR;
                          passed to DFS as the starting scan position. */
  MatchResult computeLRSCDistancesAndCycles(MachineFunction &MF) {
    /* Pull in lrsc:: helpers (isLR, isSC, getLRSCWidth, etc.) without
     * qualification. */
    using namespace lrsc;
    /* Maps (BasicBlock*, rs1Clobbered) → best instruction distance seen so far;
     * reset per LR. */
    std::map<std::pair<MachineBasicBlock *, bool>, int> visitedDistance;
    /* Running count of distinct blocks visited in the current DFS; reset per
     * LR. */
    uint8_t exploredBlockCount;
    /* Longest LR-to-SC distance found across all DFS paths; -1 means no SC
     * matched yet. */
    int longestDistance;
    /* Ordered list of block numbers on the current DFS path; used for cycle
     * detection. */
    std::vector<MachineBasicBlock *> visitedBlocksBuffer;
    /* Accumulator for all matches and cycles found across the entire
     * MachineFunction. */
    MatchResult result;
    /* Initialize matches to empty so stale data from a previous run cannot
     * persist. */
    result.matches = {};
    /* Initialize cycles to empty so stale data from a previous run cannot
     * persist. */
    result.cycles = {};
    /* Iterate over every basic block in MF in traversal order. */
    for (auto &MBB : MF) {
      /* Log the block number of the basic block currently being scanned. */
      LLVM_DEBUG(dbgs() << "  *** Exploring BasicBlock: " << MBB.getNumber()
                        << " *** \n");
      /* Iterator pointing to the first instruction in this basic block. */
      MachineBasicBlock::iterator MBBI = MBB.begin();
      /* Sentinel iterator marking one past the last instruction in this basic
       * block. */
      MachineBasicBlock::iterator E = MBB.end();
      /* Scan every instruction in the basic block looking for LR instructions.
       */
      while (MBBI != E) {
        /* Extract the opcode of the current instruction for classification. */
        uint16_t opc = MBBI->getOpcode();
        /* Log the raw opcode value of the current instruction. */
        LLVM_DEBUG(dbgs() << "  instr opcode: " << opc << "\n");
        /* Only proceed with DFS setup if this instruction is an LR variant. */
        if (isLR(opc)) {
          /* Build the fixed search context for this LR: width qualifier, base
           * register, and origin block. */
          LRSCSearchMetaData searchMetaData{getLRSCWidth(opc),
                                            getRegString(*MBBI, MF), &MBB};
          /* Reset the revisit-pruning map so the new DFS starts with no prior
           * block visits. */
          visitedDistance = {};
          /* Reset the path buffer so the new DFS starts with an empty traversal
           * path. */
          visitedBlocksBuffer = {};
          /* Reset the block counter so the new DFS starts from zero explored
           * blocks. */
          exploredBlockCount = 0;
          /* Reset to -1 to indicate no matching SC has been found yet for this
           * LR. */
          longestDistance = -1;
          /* Placeholder for the LR/SC match result that DFS will populate if a
           * match is found. */
          MatchTuple currentEntry;
          /* Initialize mutable DFS state: rs1 not yet clobbered, distance
             starts at 1 (LR itself), depth starts at 0, referencing the shared
             visitedBlocksBuffer and visitedDistance. */
          DFSTraversalState state{false, 1, 0, visitedBlocksBuffer,
                                  visitedDistance};
          /* Bundle output accumulators into DFSOutput so all recursive DFS
           * calls share them. */
          DFSOutput out{exploredBlockCount, longestDistance, result.cycles,
                        currentEntry};
          /* Advance past the LR instruction so DFS begins scanning from the
           * next instruction. */
          MachineBasicBlock::iterator startIt = ++MBBI;
          /* Launch the recursive DFS traversal from the instruction immediately
           * after the LR. */
          DFS(&MBB, startIt, searchMetaData, state, out);
          /* Only record a match if DFS found at least one valid matching SC
           * instruction. */
          if (longestDistance != -1)
            /* Append the completed LR/SC match entry to the result's match
             * list. */
            result.matches.push_back(currentEntry);
          /* Skip the ++MBBI at the bottom since MBBI was already advanced past
           * the LR above. */
          continue;
        }
        /* Advance to the next instruction if the current one was not an LR. */
        ++MBBI;
      }
    }
    /* Return the fully populated MatchResult containing all matches and cycles
     * found in MF. */
    return result;
  }

private:
  /*  LRSCSearchMetaData: Fixed parameters describing what the DFS is looking
  for. Set once per LR instruction and never modified during traversal.
  - lrscWidth:     Width qualifier ("W"/"D") of the LR to match against SC.
  - baseReg:       String of the LR's base register (rs1).
  - originLRBlock: Block containing the original LR instruction. */
  struct LRSCSearchMetaData {
    lrsc::LRSCWidth lrscWidth;
    std::string baseReg;
    MachineBasicBlock *originLRBlock;
  };

  /*--------------------------------------------------------------------------*/
  /* DFSTraversalState: Mutable state that changes as the DFS recurses.
    - rs1Clobbered:       True if rs1 was redefined since the LR was seen.
    - currentDistance:    Instruction count accumulated from the LR to now.
    - currentDepth:       Recursion depth; enforced against MaxDepth.
    - visitedBlocksBuffer: Ordered blocks on the current DFS path.
    - visitedDistance:    Map from (block, rs1Clobbered) → best distance seen;
                          used for revisit pruning. */
  struct DFSTraversalState {
    bool rs1Clobbered;
    int currentDistance;
    int currentDepth;
    std::vector<MachineBasicBlock *> &visitedBlocksBuffer;
    std::map<std::pair<MachineBasicBlock *, bool>, int> &visitedDistance;
  };

  /*--------------------------------------------------------------------------*/
  /* DFSOutput: Accumulators written to throughout the DFS traversal.
    - exploredBlockCount: Running count of distinct blocks visited.
    - longestDistance:    Maximum LR-to-SC distance found across all paths.
    - cycles:             All detected CycleEntry objects.
    - entry:              Updated when a valid LR/SC match is found. */
  struct DFSOutput {
    uint8_t &exploredBlockCount;
    int &longestDistance;
    std::vector<CycleEntry> &cycles;
    MatchTuple &entry;
  };

  /*--------------------------------------------------------------------------*/
  /* DFS: Recursive DFS over the CFG searching for a matching SC instruction.
    Scans instructions from startIterator within currentBlock, incrementing
    currentDistance for each instruction passed. The current path terminates
    on:
      - depth or block-count bound exceeded
      - revisit pruning (same block+clobbered state seen at equal/greater
    distance)
      - encountering another LR (possible self-cycle)
      - rs1 being clobbered by an intervening instruction
      - finding a matching or non-matching SC
    After exhausting the current block, recurses into CFG successors.
    Detects and records cycles (up to two hops ahead) that include
    originLRBlock.

    Parameters:
    - currentBlock:  Basic block currently being scanned.
    - startIterator: Instruction at which to begin scanning in currentBlock.
    - searchMetaData:           Static search context (lrscWidth, baseReg,
    originLRBlock); fixed for the entire DFS traversal.
    - state:         Mutable traversal state (rs1Clobbered, currentDistance,
                      currentDepth, visitedBlocksBuffer, visitedDistance);
                      passed by value so that scalar fields backtrack naturally
                      as recursion unwinds while references remain shared.
    - out:           Output accumulators (exploredBlockCount, longestDistance,
                      cycles, entry); passed by reference so all recursive calls
                      write to the same result.

    Cycle detection locals (succ1 loop):
    - succ1Name:             String of succ1's block number; used for
                              membership lookup in visitedBlocksBuffer.
    - it1:                   Iterator to succ1 in visitedBlocksBuffer; non-end
                              indicates a back-edge / one-hop cycle.
    - detectedCycle (1-hop): Cycle path from succ1's first occurrence to the
                              current block, with succ1 appended to close the
                              loop.
    - cycleInstructionCount: Estimated cycle cost: succ1 instructions +
                              currentDistance.
    - lrBlockStr:            String of originLRBlock's number; used to test
                              whether the cycle passes through the LR block.
    - isLRCycle:             True if the cycle's front or back block is the LR
                              block.
    - seen:           Guards against duplicate entries in the cycles
                              vector.

    Cycle detection locals (succ2 loop):
    - succ2Name:             String of succ2's block number; for two-hop
    lookup.
    - it2:                   Iterator to succ2 in visitedBlocksBuffer; non-end
                              indicates a two-hop back-edge.
    - detectedCycle (2-hop): Cycle path extended through succ1 and succ2.
    - cycleInstructionCount: Estimated two-hop cycle cost: succ1 + succ2 +
                              currentDistance.

    Revisit pruning locals:
    - key: Pair of (currentBlock, rs1Clobbered) used as visitedDistance map
            key; distinguishes clobbered vs. clean visits to the same block.
    - vit: Iterator into visitedDistance for key; used to check if this
            (block, clobbered) pair was already visited at equal/greater
            distance.

    Instruction scan locals (LR self-cycle):
    - lrSelfCycle: Copy of visitedBlocksBuffer at self-cycle detection time;
                    used as the cycle path for the LR self-cycle entry.
                    Any existing supercycles are erased before it is inserted.

    Successor recursion locals:
    - succName:  String of the successor block's number; checked against
                  visitedBlocksBuffer before recursing.
    - currName:  String of the current block's number; retained for context
                  during successor traversal logging.
    - nextState: Copy of state with currentDepth incremented by one; passed
                  to the recursive DFS call for each successor. */
    void DFS(MachineBasicBlock *currentBlock,
        MachineBasicBlock::iterator startIterator,
        const LRSCSearchMetaData &searchMetaData, DFSTraversalState state,
        DFSOutput &out) {
    /* Pull in lrsc:: helpers (isLR, isSC, getLRSCWidth, etc.) without qualification. */
    using namespace lrsc;
    /* Log a header marking the start of a new DFS call for readability in debug output. */
    LLVM_DEBUG(dbgs() << "\n=== DFS CALL ===\n");
    /* Log the block number of the basic block currently being scanned. */
    LLVM_DEBUG(dbgs() << "  currentBlock    : " << currentBlock->getNumber() << "\n");
    /* Log the block number of the block containing the LR instruction that launched this DFS. */
    LLVM_DEBUG(dbgs() << "  originLRBlock   : "
                      << searchMetaData.originLRBlock->getNumber() << "\n");
    /* Log the width qualifier (W/D/Unknown) of the LR instruction being matched. */
    LLVM_DEBUG(dbgs() << "  lrscWidth       : "
                      << lrsc::stringifyWidth(searchMetaData.lrscWidth) << "\n");
    /* Log the base register (rs1) of the LR instruction that must be matched by the SC. */
    LLVM_DEBUG(dbgs() << "  baseReg         : " << searchMetaData.baseReg << "\n");
    /* Log whether rs1 has been redefined since the LR was seen, which would invalidate a match. */
    LLVM_DEBUG(dbgs() << "  rs1Clobbered    : " << state.rs1Clobbered << "\n");
    /* Log the instruction count accumulated from the LR to the current scan position. */
    LLVM_DEBUG(dbgs() << "  currentDistance : " << state.currentDistance << "\n");
    /* Log the current recursion depth to track how deep into the CFG this DFS call is. */
    LLVM_DEBUG(dbgs() << "  currentDepth    : " << state.currentDepth << "\n");
    /* Log the number of distinct basic blocks visited so far in this DFS traversal. */
    LLVM_DEBUG(dbgs() << "  exploredBlockCount: " << (int)out.exploredBlockCount << "\n");
    /* Log the longest LR-to-SC distance found across all DFS paths so far. */
    LLVM_DEBUG(dbgs() << "  longestDistance : " << out.longestDistance << "\n");
    /* Log the opening bracket then each block number on the current DFS path. */
    LLVM_DEBUG(dbgs() << "  visitedBlocksBuffer: [");
    LLVM_DEBUG({
      for (auto *MBB : state.visitedBlocksBuffer)
        dbgs() << MBB->getNumber() << " ";
    });
    /* Log the closing bracket of the visited blocks buffer. */
    LLVM_DEBUG(dbgs() << "]\n");
    /* Log the opening bracket then each successor block number of the current block. */
    LLVM_DEBUG(dbgs() << "  successors: [");
    LLVM_DEBUG({
      for (MachineBasicBlock *s : currentBlock->successors())
        dbgs() << s->getNumber() << " ";
    });
    /* Log the closing bracket of the successor list. */
    LLVM_DEBUG(dbgs() << "]\n");

    /* Record the current block pointer onto the DFS path buffer before scanning or recursing. */
    state.visitedBlocksBuffer.push_back(currentBlock);
    /* Log the opening bracket then each block number in the path buffer after pushing current block. */
    LLVM_DEBUG(dbgs() << "  pushed self, buffer now: [");
    LLVM_DEBUG({
      for (auto *MBB : state.visitedBlocksBuffer)
        dbgs() << MBB->getNumber() << " ";
    });
    /* Log the closing bracket of the updated path buffer. */
    LLVM_DEBUG(dbgs() << "]\n");

    /* (21–28) Cycle detection */
    /* Check each direct successor of the current block for a back-edge into the current path. */
    for (MachineBasicBlock *succ1 : currentBlock->successors()) {
      /* Retain succ1's block number as a string for logging and cycle path construction only. */
      std::string succ1Name = std::to_string(succ1->getNumber());
      /* Search the current path buffer for succ1 by pointer; a hit indicates a back-edge. */
      auto it1 = std::find(state.visitedBlocksBuffer.begin(),
                          state.visitedBlocksBuffer.end(), succ1);
      /* succ1 is already on the current path — a one-hop cycle back to it exists. */
      if (it1 != state.visitedBlocksBuffer.end()) {
        /* Build the string cycle path by converting each pointer in the range to its block number. */
        std::vector<std::string> detectedCycle;
        for (auto it = it1; it != state.visitedBlocksBuffer.end(); ++it)
          detectedCycle.push_back(std::to_string((*it)->getNumber()));
        /* Close the cycle by appending succ1's number to show where it loops back to. */
        detectedCycle.push_back(succ1Name);
        /* Estimate cycle cost as succ1's instruction count plus distance accumulated so far. */
        int cycleInstructionCount =
            getMBBInstrCount(succ1) + state.currentDistance;
        /* Log that a one-hop cycle was detected then log each block in its path. */
        LLVM_DEBUG(dbgs() << "  CYCLE DETECTED (succ1): ");
        LLVM_DEBUG({
          for (auto &b : detectedCycle)
            dbgs() << b << " -> ";
        });
        /* Log a newline after the cycle path. */
        LLVM_DEBUG(dbgs() << "\n");
        /* Convert the origin LR block number to string for membership testing in the cycle. */
        std::string lrBlockStr =
            std::to_string(searchMetaData.originLRBlock->getNumber());
        /* A cycle is an LR cycle if the origin LR block is at the front or back of the path. */
        bool isLRCycle = (detectedCycle.front() == lrBlockStr ||
                          detectedCycle.back() == lrBlockStr);
        /* Log that this cycle passes through the LR block if applicable. */
        if (isLRCycle)
          LLVM_DEBUG(dbgs() << "LR Cycle");
        /* Check whether this exact cycle path has already been recorded to avoid duplicates. */
        bool seen = alreadySeenCycle(out.cycles, detectedCycle);
        /* Only record the cycle if it is new and involves the LR block. */
        if (!seen && isLRCycle)
          out.cycles.push_back({detectedCycle, isLRCycle, cycleInstructionCount});
        /* Log that this cycle was skipped because it was already recorded or not an LR cycle. */
        else
          LLVM_DEBUG(dbgs() << "  (already seen, skipped)\n");
      }

      /* Check each two-hop successor of the current block for a back-edge into the current path. */
      for (MachineBasicBlock *succ2 : succ1->successors()) {
        /* Retain succ2's block number as a string for logging and cycle path construction only. */
        std::string succ2Name = std::to_string(succ2->getNumber());
        /* Search the current path buffer for succ2 by pointer; a hit indicates a two-hop back-edge. */
        auto it2 = std::find(state.visitedBlocksBuffer.begin(),
                            state.visitedBlocksBuffer.end(), succ2);
        /* succ2 is already on the current path — a two-hop cycle through succ1 exists. */
        if (it2 != state.visitedBlocksBuffer.end()) {
          /* Build the string cycle path by converting each pointer in the range to its block number. */
          std::vector<std::string> detectedCycle;
          for (auto it = it2; it != state.visitedBlocksBuffer.end(); ++it)
            detectedCycle.push_back(std::to_string((*it)->getNumber()));
          /* Extend the cycle path through succ1 as the intermediate hop. */
          detectedCycle.push_back(succ1Name);
          /* Close the cycle by appending succ2's number to show where it loops back to. */
          detectedCycle.push_back(succ2Name);
          /* Estimate two-hop cycle cost as succ1 + succ2 instruction counts plus current distance. */
          int cycleInstructionCount = getMBBInstrCount(succ1) +
                                      getMBBInstrCount(succ2) +
                                      state.currentDistance;
          /* Log that a two-hop cycle was detected then log each block in its path. */
          LLVM_DEBUG(dbgs() << "  CYCLE DETECTED (succ2): ");
          LLVM_DEBUG({
            for (auto &b : detectedCycle)
              dbgs() << b << " -> ";
          });
          /* Log a newline after the two-hop cycle path. */
          LLVM_DEBUG(dbgs() << "\n");
          /* Convert the origin LR block number to string for membership testing in the cycle. */
          std::string lrBlockStr =
              std::to_string(searchMetaData.originLRBlock->getNumber());
          /* A cycle is an LR cycle if the origin LR block is at the front or back of the path. */
          bool isLRCycle = (detectedCycle.front() == lrBlockStr ||
                            detectedCycle.back() == lrBlockStr);
          /* Log that this two-hop cycle passes through the LR block if applicable. */
          if (isLRCycle)
            LLVM_DEBUG(dbgs() << "LR Cycle");
          /* Check whether this exact two-hop cycle path has already been recorded. */
          bool seen = alreadySeenCycle(out.cycles, detectedCycle);
          /* Only record the two-hop cycle if it is new and involves the LR block. */
          if (!seen && isLRCycle)
            out.cycles.push_back({detectedCycle, isLRCycle, cycleInstructionCount});
          /* Log that this two-hop cycle was skipped because it was already recorded. */
          else
            LLVM_DEBUG(dbgs() << "  (already seen, skipped)\n");
        }
      }
    }

    /* (30–32) Revisit pruning */
    /* Build the pruning key from (currentBlock, rs1Clobbered) to distinguish clobbered visits. */
    auto key = std::make_pair(currentBlock, state.rs1Clobbered);
    /* Look up whether this (block, clobbered) pair has been visited before. */
    auto vit = state.visitedDistance.find(key);
    /* If visited at equal or greater distance this path cannot improve longestDistance so prune. */
    if (vit != state.visitedDistance.end() &&
        vit->second >= state.currentDistance) {
      /* Log that this DFS path is being pruned due to a previously seen better or equal visit. */
      LLVM_DEBUG(dbgs() << "  RETURN: revisit pruning\n");
      /* Remove the current block pointer from the path buffer before returning to restore path state. */
      state.visitedBlocksBuffer.pop_back();
      return;
    }

    /* Record the best distance seen so far for this (block, clobbered) pair for future pruning. */
    state.visitedDistance[key] = state.currentDistance;
    /* Increment the explored block counter now that this block is being fully scanned. */
    out.exploredBlockCount++;

    /* (35–47) Scan instructions */
    /* Scan each instruction in currentBlock starting from startIterator. */
    for (MachineBasicBlock::iterator MBBI = startIterator,
                                    E = currentBlock->end();
        MBBI != E; ++MBBI) {
      /* Extract the opcode of the current instruction for classification. */
      uint16_t opc = MBBI->getOpcode();
      /* Log the raw opcode value of the current instruction being scanned. */
      LLVM_DEBUG(dbgs() << "  instr opcode: " << opc << "\n");

      /* Another LR found before an SC — this path hits a nested or self LR. */
      if (isLR(opc)) {
        /* Count the LR instruction itself as one step in the distance. */
        state.currentDistance++;
        /* Log the distance at the point where the nested LR was encountered. */
        LLVM_DEBUG(dbgs() << "  found LR, currentDistance: "
                          << state.currentDistance << "\n");
        /* If the nested LR is in the same block as the original LR this is a self-cycle. */
        if (currentBlock == searchMetaData.originLRBlock) {
          /* Log that a self-cycle was detected in the origin LR block. */
          LLVM_DEBUG(dbgs() << "  LR self cycle detected\n");
          /* Convert each block pointer in the path buffer to its block number string for the cycle entry. */
          std::vector<std::string> lrSelfCycle;
          for (auto *MBB : state.visitedBlocksBuffer)
            lrSelfCycle.push_back(std::to_string(MBB->getNumber()));
          /* Remove any previously recorded supercycles that contain the self-cycle path
            since the self-cycle is the tightest and most accurate representation. */
          out.cycles.erase(std::remove_if(out.cycles.begin(), out.cycles.end(),
                                          [&](const CycleEntry &c) {
                                            return std::search(
                                                      c.cyclePath.begin(),
                                                      c.cyclePath.end(),
                                                      lrSelfCycle.begin(),
                                                      lrSelfCycle.end()) !=
                                                  c.cyclePath.end();
                                          }),
                          out.cycles.end());
          /* Log each block number in the self-cycle path. */
          LLVM_DEBUG({
            for (auto &b : lrSelfCycle)
              dbgs() << b << " -> ";
          });
          /* Record the self-cycle with isLRCycle=true and the current distance as cost. */
          out.cycles.push_back({lrSelfCycle, true, state.currentDistance});
          /* Remove the current block pointer from the path buffer before returning. */
          state.visitedBlocksBuffer.pop_back();
          return;
        }
        /* Remove the current block pointer from the path buffer before returning. */
        state.visitedBlocksBuffer.pop_back();
        return;
      }

      /* Check if the current instruction redefines rs1 — invalidates any future SC match. */
      if (defsReg(*MBBI, searchMetaData.baseReg)) {
        /* Count the clobbering instruction as one step in the distance. */
        state.currentDistance++;
        /* Log the distance at the point where rs1 was clobbered. */
        LLVM_DEBUG(dbgs() << "  rs1 clobbered at distance: "
                          << state.currentDistance << "\n");
        /* Mark rs1 as clobbered so the SC match check will reject any SC on this path. */
        state.rs1Clobbered = true;
        /* Remove the current block pointer from the path buffer before returning. */
        state.visitedBlocksBuffer.pop_back();
        return;
      }

      /* An SC instruction was found — check if it is a valid match for the origin LR. */
      if (isSC(opc)) {
        /* Log that an SC was found and is being evaluated for a match. */
        LLVM_DEBUG(dbgs() << "  found SC, checking match\n");
        /* Log whether the SC's width qualifier matches the LR's width qualifier. */
        LLVM_DEBUG(dbgs() << "  width match: "
                          << (getLRSCWidth(opc) == searchMetaData.lrscWidth) << "\n");
        /* Log whether the SC's base register (operand 2) matches the LR's base register. */
        LLVM_DEBUG(dbgs() << "  reg match: "
                          << (getRegString(*MBBI, *currentBlock->getParent(), 2) ==
                              searchMetaData.baseReg)
                          << "\n");
        /* Log whether rs1 has not been clobbered on this path, required for a valid match. */
        LLVM_DEBUG(dbgs() << "  not clobbered: " << (!state.rs1Clobbered) << "\n");
        /* All three conditions must hold: widths match, base registers match, rs1 not clobbered. */
        if (getLRSCWidth(opc) == searchMetaData.lrscWidth &&
            searchMetaData.lrscWidth != LRSCWidth::Unknown &&
            getRegString(*MBBI, *currentBlock->getParent(), 2) ==
                searchMetaData.baseReg &&
            state.rs1Clobbered == false) {
          /* Update the longest distance if this path's distance exceeds the current best. */
          out.longestDistance =
              std::max(out.longestDistance, state.currentDistance);
          /* Log the updated longest distance after a successful LR/SC match. */
          LLVM_DEBUG(dbgs() << "  MATCH! longestDistance: "
                            << out.longestDistance << "\n");
          /* Record the LR's base register in the match entry. */
          out.entry.lrInstruction = searchMetaData.baseReg;
          /* Record the matched SC's opcode name in the match entry. */
          out.entry.matchedSCInstruction = stringifyOpcode(opc);
          /* Sync the match entry's distance field with the updated longest distance. */
          out.entry.longestDistance = out.longestDistance;
        }
        /* Remove the current block pointer from the path buffer before returning. */
        state.visitedBlocksBuffer.pop_back();
        return;
      }

      /* Count this non-LR non-SC non-clobbering instruction as one step in the distance. */
      state.currentDistance++;

      /* A terminator ends the instruction scan; successors handle control flow. */
      if (MBBI->isTerminator()) {
        /* Log that the terminator was reached and the instruction scan loop is ending. */
        LLVM_DEBUG(dbgs() << "  terminator reached, breaking\n");
        break;
      }
    }

    /* (48–49) Recurse into successors */
    /* After exhausting the current block's instructions recurse into each CFG successor. */
    for (MachineBasicBlock *succ : currentBlock->successors()) {
      /* Retain the successor's block number as a string for logging only. */
      std::string succName = std::to_string(succ->getNumber());
      /* Check if the successor pointer is already on the current DFS path to avoid redundant recursion. */
      if (std::find(state.visitedBlocksBuffer.begin(),
                    state.visitedBlocksBuffer.end(),
                    succ) != state.visitedBlocksBuffer.end()) {
        /* Skip the successor only if it is not the origin LR block — back-edges to the LR
          block are allowed because they represent the retry loop we are trying to detect. */
        if (succ != searchMetaData.originLRBlock) {
          /* Log that this successor is being skipped because it is already on the path. */
          LLVM_DEBUG(dbgs() << "  SKIP succ " << succName << " (already in path)\n");
          continue;
        }
      }
      /* Log that the DFS is about to recurse into this successor block. */
      LLVM_DEBUG(dbgs() << "  recursing into succ " << succName << "\n");
      /* Build the next recursion's state inheriting current clobbered/distance/depth values
        and incrementing depth by one; visitedBlocksBuffer and visitedDistance are shared. */
      DFSTraversalState nextState{
          state.rs1Clobbered, state.currentDistance, state.currentDepth + 1,
          state.visitedBlocksBuffer, state.visitedDistance};
      /* Recurse into the successor starting from its first instruction. */
      DFS(succ, succ->begin(), searchMetaData, nextState, out);
    }

    /* Log that the DFS is backtracking out of the current block. */
    LLVM_DEBUG(dbgs() << "  backtrack from " << currentBlock->getNumber() << "\n");
    /* Remove the current block pointer from the path buffer to restore the path state for the caller. */
    state.visitedBlocksBuffer.pop_back();
  }
    /*--------------------------------------------------------------------------*/
    /* alreadySeenCycle: Returns true if detectedCycle already exists in cycles.
      Used to guard against inserting duplicate cycle entries into the cycles
      accumulator during DFS traversal.
      - cycles:        The current list of detected cycles in DFSOutput.
      - detectedCycle: The candidate cycle path to check for duplicates. */
    bool alreadySeenCycle(const std::vector<CycleEntry> &cycles,
                          const std::vector<std::string> &detectedCycle) {
      for (const auto &c : cycles) {
        if (c.cyclePath == detectedCycle)
          return true;
      }
      return false;
    }
  };

} // namespace utils