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
struct LRSCDistanceAndCycle {
  /*----------computeLRSCDistancesAndCycles-----------*/
  struct CycleEntry {
    std::vector<std::string> cyclePath;
    bool isLRCycle;
    int instructionCount;
  };

  struct MatchEntry {
    std::string lrInstruction;
    std::string matchedSCInstruction;
    int longestDistance;
  };

  struct MatchResult {
    std::vector<MatchEntry> matches;
    std::vector<CycleEntry> cycles;
    
  };
  MatchResult result;
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

      MachineBasicBlock::iterator MBBI = MBB.begin();
      MachineBasicBlock::iterator E = MBB.end();
      while (MBBI != E) {
        uint16_t opc = MBBI->getOpcode();
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
void DFS(MachineBasicBlock *currentBlock,
         MachineBasicBlock::iterator startIterator, std::string lrscWidth,
         std::string baseReg, MachineBasicBlock *originLRBlock,
         bool rs1Clobbered, int currentDistance, int currentDepth,
         std::map<std::pair<MachineBasicBlock *, bool>, int> &visitedDistance,
         uint8_t &exploredBlockCount, int &longestDistance,
         std::vector<std::string> &visitedBlocksBuffer,
         std::vector<CycleEntry> &cycles, int MaxDepth, int MaxBlocks,
         MatchEntry &entry) {
          
    llvm::errs() << "\n=== DFS CALL ===\n";
    llvm::errs() << "  currentBlock    : " << currentBlock->getNumber() << "\n";
    llvm::errs() << "  originLRBlock   : " << originLRBlock->getNumber() << "\n";
    llvm::errs() << "  lrscWidth       : " << lrscWidth << "\n";
    llvm::errs() << "  baseReg         : " << baseReg << "\n";
    llvm::errs() << "  rs1Clobbered    : " << rs1Clobbered << "\n";
    llvm::errs() << "  currentDistance : " << currentDistance << "\n";
    llvm::errs() << "  currentDepth    : " << currentDepth << "\n";
    llvm::errs() << "  exploredBlockCount: " << (int)exploredBlockCount << "\n";
    llvm::errs() << "  longestDistance : " << longestDistance << "\n";
    llvm::errs() << "  visitedBlocksBuffer: [";
    for (auto &b : visitedBlocksBuffer) llvm::errs() << b << " ";
    llvm::errs() << "]\n";
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

    visitedBlocksBuffer.push_back(std::to_string(currentBlock->getNumber()));
    llvm::errs() << "  pushed self, buffer now: [";
    for (auto &b : visitedBlocksBuffer) llvm::errs() << b << " ";
    llvm::errs() << "]\n";

    /* (21–28) Cycle detection */
    for (MachineBasicBlock *succ1 : currentBlock->successors()) {
      std::string succ1Name = std::to_string(succ1->getNumber());
      auto it1 = std::find(visitedBlocksBuffer.begin(),
                           visitedBlocksBuffer.end(), succ1Name);
      if (it1 != visitedBlocksBuffer.end()) {
        std::vector<std::string> detectedCycle(it1, visitedBlocksBuffer.end());
        detectedCycle.push_back(succ1Name);
        int cycleInstructionCount = getMBBInstrCount(succ1) + currentDistance;
        llvm::errs() << "  CYCLE DETECTED (succ1): ";
        for (auto &b : detectedCycle) llvm::errs() << b << " -> ";
        llvm::errs() << "\n";
        std::string lrBlockStr = std::to_string(originLRBlock->getNumber());
        bool isLRCycle = (detectedCycle.front() == lrBlockStr || 
                          detectedCycle.back() == lrBlockStr);
        
        if(isLRCycle) llvm::errs() << "LR Cycle";
        bool alreadySeen = false;
        for (auto &c : cycles) {
          if (c.cyclePath == detectedCycle) { alreadySeen = true; break; }
        }
        if (!alreadySeen && isLRCycle) cycles.push_back({detectedCycle, isLRCycle,cycleInstructionCount});
        else llvm::errs() << "  (already seen, skipped)\n";
      }

      for (MachineBasicBlock *succ2 : succ1->successors()) {
        std::string succ2Name = std::to_string(succ2->getNumber());
        auto it2 = std::find(visitedBlocksBuffer.begin(),
                             visitedBlocksBuffer.end(), succ2Name);
        if (it2 != visitedBlocksBuffer.end()) {
          std::vector<std::string> detectedCycle(it2, visitedBlocksBuffer.end());
          detectedCycle.push_back(succ1Name);
          detectedCycle.push_back(succ2Name);
          int cycleInstructionCount = getMBBInstrCount(succ1) + getMBBInstrCount(succ2) + currentDistance;
          llvm::errs() << "  CYCLE DETECTED (succ2): ";
          for (auto &b : detectedCycle) llvm::errs() << b << " -> ";
          llvm::errs() << "\n";
          std::string lrBlockStr = std::to_string(originLRBlock->getNumber());
          bool isLRCycle = (detectedCycle.front() == lrBlockStr || 
                            detectedCycle.back() == lrBlockStr);
          if(isLRCycle) llvm::errs() << "LR Cycle";
          bool alreadySeen = false;
          for (auto &c : cycles) {
            if (c.cyclePath == detectedCycle) { alreadySeen = true; break; }
          }
          if (!alreadySeen && isLRCycle) cycles.push_back({detectedCycle, isLRCycle, cycleInstructionCount});
          else llvm::errs() << "  (already seen, skipped)\n";
        }
      }
    }

    /* (30–32) Revisit pruning */
    auto key = std::make_pair(currentBlock, rs1Clobbered);
    auto vit = visitedDistance.find(key);
    if (vit != visitedDistance.end() && vit->second >= currentDistance) {
      llvm::errs() << "  RETURN: revisit pruning\n";
      visitedBlocksBuffer.pop_back();
      return;
    }

    /* (33–34) Record this visit */
    visitedDistance[key] = currentDistance;
    exploredBlockCount++;

    /* (35–47) Scan instructions */
    for (MachineBasicBlock::iterator MBBI = startIterator,
                                     E = currentBlock->end();
         MBBI != E; ++MBBI) {

      uint16_t opc = MBBI->getOpcode();
      llvm::errs() << "  instr opcode: " << opc << "\n";

      if (isLR(opc)) {
        currentDistance++;
        llvm::errs() << "  found LR, currentDistance: " << currentDistance << "\n";
        if (currentBlock == originLRBlock) {
          llvm::errs() << "  LR self cycle detected\n";
          
          std::vector<std::string> lrSelfCycle = visitedBlocksBuffer;
          auto it = std::find_if(cycles.begin(), cycles.end(), [&](const CycleEntry &c) {
            return c.cyclePath == lrSelfCycle;
          });
          if (it != cycles.end()) {
              cycles.erase(it);
          }
          for (auto &b : lrSelfCycle) llvm::errs() << b << " -> ";
          cycles.push_back({lrSelfCycle, true, currentDistance});
          visitedBlocksBuffer.pop_back();
          return;
        }
        visitedBlocksBuffer.pop_back();
        return;
      }

      if (defsReg(*MBBI, baseReg)) {
        currentDistance++;
        llvm::errs() << "  rs1 clobbered at distance: " << currentDistance << "\n";
        rs1Clobbered = true;
        visitedBlocksBuffer.pop_back();
        return;
      }

      if (isSC(opc)) {
        llvm::errs() << "  found SC, checking match\n";
        llvm::errs() << "  width match: " << (getLRSCWidth(opc) == lrscWidth) << "\n";
        llvm::errs() << "  reg match: " << (getRegString(*MBBI, *currentBlock->getParent(), 2) == baseReg) << "\n";
        llvm::errs() << "  not clobbered: " << (!rs1Clobbered) << "\n";
        if (getLRSCWidth(opc) == lrscWidth &&
            getRegString(*MBBI, *currentBlock->getParent(), 2) == baseReg &&
            rs1Clobbered == false) {
          longestDistance = std::max(longestDistance, currentDistance);
          llvm::errs() << "  MATCH! longestDistance: " << longestDistance << "\n";
          entry.lrInstruction = baseReg;
          entry.matchedSCInstruction = stringifyOpcode(opc);
          entry.longestDistance = longestDistance;
        }
        visitedBlocksBuffer.pop_back();
        return;
      }

      currentDistance++;

      if (MBBI->isTerminator()) {
        llvm::errs() << "  terminator reached, breaking\n";
        break;
      }
    }

    /* (48–49) Recurse into successors */
    for (MachineBasicBlock *succ : currentBlock->successors()) {
      std::string succName = std::to_string(succ->getNumber());
      std::string currName = std::to_string(currentBlock->getNumber());
      if (std::find(visitedBlocksBuffer.begin(), visitedBlocksBuffer.end(),
                    succName) != visitedBlocksBuffer.end()) {
        if (succName != std::to_string(originLRBlock->getNumber())) {
          llvm::errs() << "  SKIP succ " << succName << " (already in path)\n";
          continue;
        }
      }
      llvm::errs() << "  recursing into succ " << succName << "\n";
      DFS(succ, succ->begin(), lrscWidth, baseReg, originLRBlock, rs1Clobbered,
          currentDistance, currentDepth + 1, visitedDistance,
          exploredBlockCount, longestDistance, visitedBlocksBuffer, cycles,
          MaxDepth, MaxBlocks, entry);
    }

    llvm::errs() << "  backtrack from " << currentBlock->getNumber() << "\n";
    visitedBlocksBuffer.pop_back();
}
  /*----------HELPERS-----------*/
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
      llvm::errs() << (c.isLRCycle ? "[LR cycle]" : "[no LR]") << " InstrCount: " << c.instructionCount << "\n";
    }
  }
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
  int getMBBInstrCount(const MachineBasicBlock *MBB) {
    int count = 0;
    for (auto &MI : *MBB) {
        count++;
    }
    return count;
}
}; // LRSCDistanceAndCycle

} // namespace utils
