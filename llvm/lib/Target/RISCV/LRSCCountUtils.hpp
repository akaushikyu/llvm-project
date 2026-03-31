#include "RISCV.h"
#include "RISCVInstrInfo.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <map>
#include <string>
#include <vector>
using namespace llvm;
using MatchEntry = std::tuple<std::string, std::string, int>;
namespace utils {
void computeLRSCDistancesAndCycles(MachineFunction &MF);

void DFS(
    MachineBasicBlock *currentBlock, MachineBasicBlock::iterator startIterator,
    std::string lrscWidth, std::string baseReg,
    MachineBasicBlock *originLRBlock, unsigned rs1Clobbered,
    int currentDistance, int currentDepth,
    std::map<std::pair<MachineBasicBlock *, unsigned>, int> &visitedDistance,
    uint8_t &exploredBlockCount, int &longestDistance,
    std::vector<std::string> &visitedBlocksBuffer,
    std::vector<std::string> &cycles, int MaxDepth, int MaxBlocks,
    MatchEntry &entry);
int getMBBInstrCount(const MachineBasicBlock *MBB);

bool isLR(uint16_t opc);
bool isSC(uint16_t opc);
std::string getLRSCWidth(uint16_t opc);
std::string getRegString(MachineInstr &MI, MachineFunction &MF);

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
}; // LRSCCounts
/*--------------------------------------------------------------------------*/
/* LRSCDistanceAndCycle: Analyzes RISC-V LR/SC instruction pairs in a
   MachineFunction by performing a bounded DFS traversal of the CFG.
   Computes the maximum instruction distance between matched LR/SC pairs
   and detects CFG cycles that contain LR instructions.
   Results are stored in a MatchResult and can be printed via dump(). */
struct LRSCDistanceAndCycle {
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
  /* MatchEntry: Represents a single matched LR/SC pair found during DFS.
     - lrInstruction:        String of the base register (rs1) of the LR.
     - matchedSCInstruction: Opcode name of the matched SC instruction.
     - longestDistance:      Longest instruction distance found between
                             the LR and the SC across all DFS paths. */
  struct MatchEntry {
    std::string lrInstruction;
    std::string matchedSCInstruction;
    int longestDistance;
  };

  /*--------------------------------------------------------------------------*/
  /* MatchResult: Aggregated result of the LR/SC analysis for a MachineFunction.
     - matches: All valid LR/SC pairs found during the DFS traversal.
     - cycles:  All CFG cycles detected that include the origin LR block. */
  struct MatchResult {
    std::vector<MatchEntry> matches;
    std::vector<CycleEntry> cycles;
  };
  MatchResult result;

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
     - visitedBlockBuffer: Ordered list of block numbers on the current DFS
     path; used for cycle detection and path reconstruction.
     - width:              Width qualifier of the current LR ("W" or "D");
                           must match the SC's width for a valid pair.
     - baseReg:            Base register (rs1) string of the current LR;
                           must match the SC's rs1 and must not be clobbered.
     - originLRBlock:      Pointer to the block containing the current LR;
                           used to detect self-cycles and anchor cycle checks.
     - startIt:            Iterator to the instruction immediately after the LR;
                           passed to DFS as the starting scan position. */
  void computeLRSCDistancesAndCycles(MachineFunction &MF) {
    std::map<std::pair<MachineBasicBlock *, bool>, int> visitedDistance;
    uint8_t exploredBlockCount;
    int longestDistance;
    std::vector<std::string> visitedBlockBuffer;
    std::string width = "";
    std::string baseReg = "";
    MachineBasicBlock *originLRBlock;
    MachineBasicBlock::iterator startIt;
    result.matches = {};
    result.cycles = {};

    for (auto &MBB : MF) {

      llvm::errs() << "  *** Exploring BasicBlock: " << MBB.getNumber()
                   << " *** \n";
      MachineBasicBlock::iterator MBBI = MBB.begin();
      MachineBasicBlock::iterator E = MBB.end();
      while (MBBI != E) {
        
        uint16_t opc = MBBI->getOpcode();
        llvm::errs() << "  instr opcode: " << opc << "\n";
        if (isLR(opc)) {
          width = getLRSCWidth(opc);
          baseReg = getRegString(*MBBI, MF);
          originLRBlock = &MBB;

          startIt = ++MBBI;
          exploredBlockCount = 0;
          longestDistance = -1;
          visitedDistance = {};
          visitedBlockBuffer = {};
          MatchEntry currentEntry;
          DFS(&MBB, startIt, width, baseReg, originLRBlock, false, 1, 0,
              visitedDistance, exploredBlockCount, longestDistance,
              visitedBlockBuffer, result.cycles, 100, 200, currentEntry);

          /* (15–17) Record match if valid SC was found */
          if (longestDistance != -1) {
            result.matches.push_back(currentEntry);
          }
          continue;
        }
        ++MBBI;
      }
    }
  }

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
     - currentBlock:        Basic block currently being scanned.
     - startIterator:       Instruction at which to begin scanning in
     currentBlock.
     - lrscWidth:           Width qualifier ("W"/"D") of the LR to match against
     SC.
     - baseReg:             String of the LR's base register (rs1).
     - originLRBlock:       Block containing the original LR instruction.
     - rs1Clobbered:        True if rs1 was redefined since the LR was seen.
     - currentDistance:     Instruction count accumulated from the LR to now.
     - currentDepth:        Recursion depth; enforced against MaxDepth.
     - visitedDistance:     Map from (block, rs1Clobbered) → best distance seen;
                            used for revisit pruning.
     - exploredBlockCount:  Running count of distinct blocks visited; bounded by
     MaxBlocks.
     - longestDistance:     Out-param updated with the maximum LR-to-SC distance
     found.
     - visitedBlocksBuffer: Ordered block numbers on the current DFS path;
                            used for cycle detection and path reconstruction.
     - cycles:              Accumulator for all detected CycleEntry objects.
     - MaxDepth:            Maximum allowed recursion depth.
     - MaxBlocks:           Maximum allowed number of blocks to explore.
     - entry:               Out-param updated when a valid LR/SC match is found.

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
     - alreadySeen:           Guards against duplicate entries in the cycles
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
     - it:          Iterator to any existing cycle entry with the same path;
                    used to replace a stale entry with the updated one.

     Successor recursion locals:
     - succName: String of the successor block's number; checked against
                 visitedBlocksBuffer before recursing.
     - currName: String of the current block's number; retained for
                 context during successor traversal logging. */
  void DFS(MachineBasicBlock *currentBlock,
           MachineBasicBlock::iterator startIterator, std::string lrscWidth,
           std::string baseReg, MachineBasicBlock *originLRBlock,
           bool rs1Clobbered, int currentDistance, int currentDepth,
           std::map<std::pair<MachineBasicBlock *, bool>, int> &visitedDistance,
           uint8_t &exploredBlockCount, int &longestDistance,
           std::vector<std::string> &visitedBlocksBuffer,
           std::vector<CycleEntry> &cycles, int MaxDepth, int MaxBlocks,
           MatchEntry &entry) {

    /* Debug: print all DFS call parameters on entry */
    llvm::errs() << "\n=== DFS CALL ===\n";
    llvm::errs() << "  currentBlock    : " << currentBlock->getNumber() << "\n";
    llvm::errs() << "  originLRBlock   : " << originLRBlock->getNumber()
                 << "\n";
    llvm::errs() << "  lrscWidth       : " << lrscWidth << "\n";
    llvm::errs() << "  baseReg         : " << baseReg << "\n";
    llvm::errs() << "  rs1Clobbered    : " << rs1Clobbered << "\n";
    llvm::errs() << "  currentDistance : " << currentDistance << "\n";
    llvm::errs() << "  currentDepth    : " << currentDepth << "\n";
    llvm::errs() << "  exploredBlockCount: " << (int)exploredBlockCount << "\n";
    llvm::errs() << "  longestDistance : " << longestDistance << "\n";
    /* Debug: print the current DFS path */
    llvm::errs() << "  visitedBlocksBuffer: [";
    for (auto &b : visitedBlocksBuffer)
      llvm::errs() << b << " ";
    llvm::errs() << "]\n";
    /* Debug: print the successors of the current block */
    llvm::errs() << "  successors: [";
    for (MachineBasicBlock *s : currentBlock->successors())
      llvm::errs() << s->getNumber() << " ";
    llvm::errs() << "]\n";

    /* (19) Enforce depth bound */
    if (currentDepth > MaxDepth) {
      llvm::errs() << "  RETURN: depth exceeded\n";
      return;
    }

    /* (20) Enforce block-visit bound */
    if (exploredBlockCount >= MaxBlocks) {
      llvm::errs() << "  RETURN: block count exceeded\n";
      return;
    }

    /* Record currentBlock on the DFS path before scanning its instructions
       or inspecting its successors */
    visitedBlocksBuffer.push_back(std::to_string(currentBlock->getNumber()));
    /* Debug: confirm the block was pushed onto the path */
    llvm::errs() << "  pushed self, buffer now: [";
    for (auto &b : visitedBlocksBuffer)
      llvm::errs() << b << " ";
    llvm::errs() << "]\n";

    /* (21–28) Cycle detection */
    /* Scan one and two hops ahead from currentBlock to detect back-edges
       into the current DFS path before committing to further traversal */
    for (MachineBasicBlock *succ1 : currentBlock->successors()) {
      /* succ1Name: string form of succ1's block number used for path lookup */
      std::string succ1Name = std::to_string(succ1->getNumber());
      /* it1: points to succ1 in the path buffer if a back-edge exists */
      auto it1 = std::find(visitedBlocksBuffer.begin(),
                           visitedBlocksBuffer.end(), succ1Name);
      /* One-hop cycle: succ1 is already on the current DFS path */
      if (it1 != visitedBlocksBuffer.end()) {
        /* detectedCycle: slice of the path from succ1's occurrence to the
           current block, with succ1 appended again to close the loop */
        std::vector<std::string> detectedCycle(it1, visitedBlocksBuffer.end());
        detectedCycle.push_back(succ1Name);
        /* Estimate the cycle's instruction count using succ1's size plus
           the distance already accumulated to reach the current block */
        int cycleInstructionCount = getMBBInstrCount(succ1) + currentDistance;
        /* Debug: print the detected one-hop cycle path */
        llvm::errs() << "  CYCLE DETECTED (succ1): ";
        for (auto &b : detectedCycle)
          llvm::errs() << b << " -> ";
        llvm::errs() << "\n";
        /* lrBlockStr: string of the origin LR block used to test cycle
         * membership */
        std::string lrBlockStr = std::to_string(originLRBlock->getNumber());
        /* isLRCycle: true if the origin LR block is the front or back of the
         * cycle */
        bool isLRCycle = (detectedCycle.front() == lrBlockStr ||
                          detectedCycle.back() == lrBlockStr);

        if (isLRCycle)
          llvm::errs() << "LR Cycle";
        /* alreadySeen: prevents inserting a duplicate cycle into the vector */
        bool alreadySeen = false;
        for (auto &c : cycles) {
          if (c.cyclePath == detectedCycle) {
            alreadySeen = true;
            break;
          }
        }
        /* Only record the cycle if it is new and involves the LR block */
        if (!alreadySeen && isLRCycle)
          cycles.push_back({detectedCycle, isLRCycle, cycleInstructionCount});
        else
          llvm::errs() << "  (already seen, skipped)\n";
      }

      /* Two-hop cycle detection: check if any successor of succ1 is already
         on the current DFS path */
      for (MachineBasicBlock *succ2 : succ1->successors()) {
        /* succ2Name: string form of succ2's block number used for path lookup
         */
        std::string succ2Name = std::to_string(succ2->getNumber());
        /* it2: points to succ2 in the path buffer if a two-hop back-edge exists
         */
        auto it2 = std::find(visitedBlocksBuffer.begin(),
                             visitedBlocksBuffer.end(), succ2Name);
        /* Two-hop cycle: succ2 is already on the current DFS path */
        if (it2 != visitedBlocksBuffer.end()) {
          /* detectedCycle: slice from succ2's occurrence, extended through
             succ1 and back to succ2 to close the two-hop loop */
          std::vector<std::string> detectedCycle(it2,
                                                 visitedBlocksBuffer.end());
          detectedCycle.push_back(succ1Name);
          detectedCycle.push_back(succ2Name);
          /* Estimate the two-hop cycle cost: succ1 + succ2 instructions plus
             the distance accumulated to reach the current block */
          int cycleInstructionCount = getMBBInstrCount(succ1) +
                                      getMBBInstrCount(succ2) + currentDistance;
          /* Debug: print the detected two-hop cycle path */
          llvm::errs() << "  CYCLE DETECTED (succ2): ";
          for (auto &b : detectedCycle)
            llvm::errs() << b << " -> ";
          llvm::errs() << "\n";
          /* lrBlockStr: string of the origin LR block used to test cycle
           * membership */
          std::string lrBlockStr = std::to_string(originLRBlock->getNumber());
          /* isLRCycle: true if the origin LR block is the front or back of the
           * cycle */
          bool isLRCycle = (detectedCycle.front() == lrBlockStr ||
                            detectedCycle.back() == lrBlockStr);
          if (isLRCycle)
            llvm::errs() << "LR Cycle";
          /* alreadySeen: prevents inserting a duplicate two-hop cycle */
          bool alreadySeen = false;
          for (auto &c : cycles) {
            if (c.cyclePath == detectedCycle) {
              alreadySeen = true;
              break;
            }
          }
          /* Only record the two-hop cycle if it is new and involves the LR
           * block */
          if (!alreadySeen && isLRCycle)
            cycles.push_back({detectedCycle, isLRCycle, cycleInstructionCount});
          else
            llvm::errs() << "  (already seen, skipped)\n";
        }
      }
    }

    /* (30–32) Revisit pruning */
    /* key: combines currentBlock and rs1Clobbered to distinguish clobbered
       vs. clean visits to the same block in visitedDistance */
    auto key = std::make_pair(currentBlock, rs1Clobbered);
    /* vit: looks up whether this (block, clobbered) pair was already visited
       with an equal or greater distance, making further exploration redundant
     */
    auto vit = visitedDistance.find(key);
    if (vit != visitedDistance.end() && vit->second >= currentDistance) {
      llvm::errs() << "  RETURN: revisit pruning\n";
      /* Remove currentBlock from the path before returning */
      visitedBlocksBuffer.pop_back();
      return;
    }

    /* (33–34) Record this visit */
    /* Store the best distance seen for this (block, clobbered) pair */
    visitedDistance[key] = currentDistance;
    /* Increment the global block-visit counter for this DFS traversal */
    exploredBlockCount++;

    /* (35–47) Scan instructions */
    /* Walk instructions from startIterator to the end of currentBlock,
       updating currentDistance and checking for LR, SC, or clobbering */
    for (MachineBasicBlock::iterator MBBI = startIterator,
                                     E = currentBlock->end();
         MBBI != E; ++MBBI) {

      uint16_t opc = MBBI->getOpcode();
      /* Debug: print each instruction's opcode as it is scanned */
      llvm::errs() << "  instr opcode: " << opc << "\n";

      /* Another LR found: count it and handle self-cycle or plain termination
       */
      if (isLR(opc)) {
        /* Count this LR as an instruction between the original LR and here */
        currentDistance++;
        llvm::errs() << "  found LR, currentDistance: " << currentDistance
                     << "\n";
        /* Self-cycle: the new LR is in the same block as the original LR */
        if (currentBlock == originLRBlock) {
          llvm::errs() << "  LR self cycle detected\n";

          /* lrSelfCycle: snapshot of the current DFS path used as the cycle
           * path */
          std::vector<std::string> lrSelfCycle = visitedBlocksBuffer;
          /* it: finds any existing entry with the same path so it can be
             replaced with the updated distance */
          auto it = std::find_if(
              cycles.begin(), cycles.end(),
              [&](const CycleEntry &c) { return c.cyclePath == lrSelfCycle; });
          if (it != cycles.end()) {
            /* Remove the stale entry before inserting the updated one */
            cycles.erase(it);
          }
          for (auto &b : lrSelfCycle)
            llvm::errs() << b << " -> ";
          /* Record the self-cycle with the current accumulated distance */
          cycles.push_back({lrSelfCycle, true, currentDistance});
          /* Remove currentBlock from the path before returning */
          visitedBlocksBuffer.pop_back();
          return;
        }
        /* Non-self LR: terminate this path; another LR/SC pair starts here */
        visitedBlocksBuffer.pop_back();
        return;
      }

      /* rs1 clobbered: the base register is redefined, invalidating any future
       * SC match */
      if (defsReg(*MBBI, baseReg)) {
        /* Count the clobbering instruction in the distance */
        currentDistance++;
        llvm::errs() << "  rs1 clobbered at distance: " << currentDistance
                     << "\n";
        rs1Clobbered = true;
        /* Remove currentBlock from the path before returning */
        visitedBlocksBuffer.pop_back();
        return;
      }

      /* SC found: check whether it is a valid match for the current LR */
      if (isSC(opc)) {
        llvm::errs() << "  found SC, checking match\n";
        /* Debug: report each individual match condition */
        llvm::errs() << "  width match: " << (getLRSCWidth(opc) == lrscWidth)
                     << "\n";
        llvm::errs() << "  reg match: "
                     << (getRegString(*MBBI, *currentBlock->getParent(), 2) ==
                         baseReg)
                     << "\n";
        llvm::errs() << "  not clobbered: " << (!rs1Clobbered) << "\n";
        /* Valid match: width, base register, and clobbered state all agree */
        if (getLRSCWidth(opc) == lrscWidth &&
            getRegString(*MBBI, *currentBlock->getParent(), 2) == baseReg &&
            rs1Clobbered == false) {
          /* Update longestDistance if this path gives a longer distance */
          longestDistance = std::max(longestDistance, currentDistance);
          llvm::errs() << "  MATCH! longestDistance: " << longestDistance
                       << "\n";
          /* Populate the output MatchEntry with the LR/SC pair details */
          entry.lrInstruction = baseReg;
          entry.matchedSCInstruction = stringifyOpcode(opc);
          entry.longestDistance = longestDistance;
        }
        /* Remove currentBlock from the path before returning */
        visitedBlocksBuffer.pop_back();
        return;
      }

      /* Regular instruction: count it toward the LR-to-SC distance */
      currentDistance++;

      /* Terminator reached: stop scanning this block and fall through to
         recurse into successors */
      if (MBBI->isTerminator()) {
        llvm::errs() << "  terminator reached, breaking\n";
        break;
      }
    }

    /* (48–49) Recurse into successors */
    /* Continue the DFS into each successor block that has not yet been
       visited on the current path */
    for (MachineBasicBlock *succ : currentBlock->successors()) {
      /* succName: string of the successor's block number for path membership
       * check */
      std::string succName = std::to_string(succ->getNumber());
      /* currName: string of the current block's number retained for logging
       * context */
      std::string currName = std::to_string(currentBlock->getNumber());
      /* Skip successors already on the path, unless the successor is the
         origin LR block (allowing the DFS to close an LR-containing loop) */
      if (std::find(visitedBlocksBuffer.begin(), visitedBlocksBuffer.end(),
                    succName) != visitedBlocksBuffer.end()) {
        if (succName != std::to_string(originLRBlock->getNumber())) {
          llvm::errs() << "  SKIP succ " << succName << " (already in path)\n";
          continue;
        }
      }
      llvm::errs() << "  recursing into succ " << succName << "\n";
      /* Recurse into the successor starting from its first instruction,
         incrementing depth by one */
      DFS(succ, succ->begin(), lrscWidth, baseReg, originLRBlock, rs1Clobbered,
          currentDistance, currentDepth + 1, visitedDistance,
          exploredBlockCount, longestDistance, visitedBlocksBuffer, cycles,
          MaxDepth, MaxBlocks, entry);
    }

    /* Backtrack: remove currentBlock from the DFS path on the way back up */
    llvm::errs() << "  backtrack from " << currentBlock->getNumber() << "\n";
    visitedBlocksBuffer.pop_back();
  }

  /*--------------------------------------------------------------------------*/
  /* HELPERS */
  /*--------------------------------------------------------------------------*/

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
  std::string getLRSCWidth(uint16_t opc) {
    switch (opc) {
    case RISCV::LR_W:
    case RISCV::LR_W_AQ:
    case RISCV::LR_W_RL:
    case RISCV::LR_W_AQRL:
    case RISCV::SC_W:
    case RISCV::SC_W_AQ:
    case RISCV::SC_W_RL:
    case RISCV::SC_W_AQRL:
      return "W";
      break;

    case RISCV::LR_D:
    case RISCV::LR_D_AQ:
    case RISCV::LR_D_RL:
    case RISCV::LR_D_AQRL:
    case RISCV::SC_D:
    case RISCV::SC_D_AQ:
    case RISCV::SC_D_RL:
    case RISCV::SC_D_AQRL:
      return "D";
      break;

    default:
      break;
    }
  }

  /*--------------------------------------------------------------------------*/
  /* stringifyOpcode: Returns the string name of a given LR or SC opcode.
     Used when recording matched SC instructions in a MatchEntry.
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
  /* dump: Prints all matches and cycles in a MatchResult to stderr.
     For each match: prints the LR base register, matched SC opcode, and
     longest distance. For each cycle: prints the block path, whether it is
     an LR cycle, the total instruction count, and the basic block count. */
  void dump(const MatchResult &result) {
    llvm::errs() << "=== Matches ===\n";
    for (auto &m : result.matches) {
      llvm::errs() << "LR: " << m.lrInstruction
                   << " SC: " << m.matchedSCInstruction
                   << " Distance: " << m.longestDistance << "\n";
    }

    llvm::errs() << "=== Cycles ===\n";
    for (auto &c : result.cycles) {
      llvm::errs() << "Cycle: ";
      for (auto &block : c.cyclePath) {
        llvm::errs() << block << " -> ";
      }
      llvm::errs() << (c.isLRCycle ? "[LR cycle]" : "[no LR]")
                   << " InstrCount: " << c.instructionCount << ", "
                   << " BasicBlockCount: " << c.cyclePath.size() << "\n";
    }
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
}; // LRSCDistanceAndCycle

} // namespace utils
