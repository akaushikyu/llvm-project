#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/Support/JSON.h"

#include <string>
#include <unordered_map>
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

    using MFKey = const MachineFunction *;
    using BBKey = const MachineBasicBlock *;
    using OpKey = std::string;

    /*--------------------------------------------------------------------------*/
    /* BBList: preserves an explicit traversal/iteration order of BB pointers
       for a given MachineFunction. This is used to emit stable bb_index values. */
    using BBList = std::vector<BBKey>;

    /*--------------------------------------------------------------------------*/
    /* FuncToBBListMap: maps each function to its BB iteration order list. */
    using FuncToBBListMap = std::unordered_map<MFKey, BBList>;

    /*--------------------------------------------------------------------------*/
    /* MF -> list of BB pointers in Func iteration order */
    FuncToBBListMap basicBlockOrder;

    /*--------------------------------------------------------------------------*/
    /* FlavourMap: opcode/flavor string -> count within a BB. */
    using FlavourMap = std::unordered_map<OpKey, int>;

    /*--------------------------------------------------------------------------*/
    /* BBToFlavourMap: BB -> (opcode/flavor -> count). */
    using BBToFlavourMap = std::unordered_map<BBKey, FlavourMap>;

    /*--------------------------------------------------------------------------*/
    /* FuncToBBFlavourMap: Func -> (BB -> (opcode/flavor -> count)). */
    using FuncToBBFlavourMap = std::unordered_map<MFKey, BBToFlavourMap>;

    /*--------------------------------------------------------------------------*/
    /* BBCountMap: BB -> total LR/SC count in that BB (definition-dependent). */
    using BBCountMap = std::unordered_map<BBKey, int>;

    /*--------------------------------------------------------------------------*/
    /* FuncToBBCountMap: Func -> (BB -> total LR/SC count). */
    using FuncToBBCountMap = std::unordered_map<MFKey, BBCountMap>;

    /*--------------------------------------------------------------------------*/
    /* FuncCountMap: Func -> total LR/SC count for the function. */
    using FuncCountMap = std::unordered_map<MFKey, int>;

    /*--------------------------------------------------------------------------*/
    /* Func -> BB -> (opcode -> count)
       Stores per-basic-block opcode/flavor breakdowns per function. */
    FuncToBBFlavourMap basicBlocksFlavourCounts;

    /*--------------------------------------------------------------------------*/
    /* Func -> BB -> total LR/SC occurrences (or pairs, depending on your
       definition)
       Stores per-basic-block totals per function. */
    FuncToBBCountMap basicBlocksCounts;

    /*--------------------------------------------------------------------------*/
    /* Func -> total LR/SC occurrences (or pairs)
       Stores per-function totals. */
    FuncCountMap functionCounts;

    /*--------------------------------------------------------------------------*/
    /* Clears all stored data across every map. */
    void clearAll() {
      basicBlockOrder.clear();
      basicBlocksFlavourCounts.clear();
      basicBlocksCounts.clear();
      functionCounts.clear();
    }

    /*--------------------------------------------------------------------------*/
    /* Updates the per-function total count for the provided Func by adding mfCount. */
    void updateFuncCnt(const MachineFunction &MF,unsigned  mfCount){
      functionCounts[&MF] += mfCount;
    }

    /*--------------------------------------------------------------------------*/
    /* Updates the per-basic-block total count for (MF, MBB) by adding bbCount. */
    void updateBBCnt(const MachineFunction &MF,const MachineBasicBlock &MBB,unsigned bbCount){
      basicBlocksCounts[&MF][&MBB] += bbCount;
    }

    /*--------------------------------------------------------------------------*/
    /* Increments the opcode/flavor counter for (MF, MBB, OpKey) by 1. */
    void updateBBFlavCnt(const MachineFunction &MF,const MachineBasicBlock &MBB, std::string OpKey){
      basicBlocksFlavourCounts[&MF][&MBB][OpKey]++;
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
    llvm::json::Value toJSON() const {
      /* Root object for the JSON output. */
      llvm::json::Object Root;

      /* Map from function-name -> per-function object (totals + BB list). */
      llvm::json::Object FuncMap; // function-name -> { totals, basic_blocks }

      /* Iterate through recorded BB order for each MachineFunction. This is the
         driver for which functions appear in JSON and in what BB order. */
      for (const auto &FO : basicBlockOrder) {
        /* Extract function pointer (key) and its BB iteration order list. */
        const MachineFunction *MF = FO.first;
        const auto &Order = FO.second;

        /* Determine the function name string for JSON keying. */
        std::string Name = MF ? MF->getName().str() : "<null>";

        /* Look up the function-level total, defaulting to 0 if absent. */
        int FuncTotal = 0;
        auto ItFT = functionCounts.find(MF);
        if (ItFT != functionCounts.end()) {
          FuncTotal = ItFT->second;
        }
        /* Array of per-basic-block JSON objects for this function. */
        llvm::json::Array Blocks;

        /* Pre-locate iterators for per-BB totals and per-BB flavor maps for MF.
           If MF is missing from either map, lookups fall back to defaults. */
        auto ItBBTotals = basicBlocksCounts.find(MF);
        auto ItBBFlavors = basicBlocksFlavourCounts.find(MF);

        /* Emit BB objects in the same order stored in basicBlockOrder[MF]. */
        for (size_t i = 0; i < Order.size(); ++i) {
          /* BB pointer at this position in iteration order. */
          const MachineBasicBlock *MBB = Order[i];

          /* JSON object holding this BB's metadata and counts. */
          llvm::json::Object BBObj;

          /* Stable index based on traversal order in Order[]. */
          BBObj["bb_index"] = static_cast<int64_t>(i);

          /* LLVM internal BB number, or -1 if MBB is null. */
          BBObj["mbb_number"] =
              MBB ? static_cast<int64_t>(MBB->getNumber()) : -1;

          // lambda function to calculate MBB size in insn count
          auto getMBBSize = [](const MachineBasicBlock *MBB) {
            unsigned cnt = 0;
            for (auto &MI : *MBB) {
              cnt++;
            }
            return cnt;
          };

          BBObj["num_insn"] = getMBBSize(MBB); 

          /* Look up BB total for this MF and BB, defaulting to 0 if absent. */
          int BBTotal = 0;
          if (ItBBTotals != basicBlocksCounts.end() && MBB) {
            auto It = ItBBTotals->second.find(MBB);
            if (It != ItBBTotals->second.end())
              BBTotal = It->second;
          }

          /* Store the BB total under a dedicated JSON field. */
          BBObj["bb_total_lrsc_occurrences"] = BBTotal;

          /* Build a JSON object of opcode/flavor counts for this BB. */
          llvm::json::Object FlavorsObj;

          /* If flavor data exists for this MF and this BB, serialize it. */
          if (ItBBFlavors != basicBlocksFlavourCounts.end() && MBB) {
            auto ItF = ItBBFlavors->second.find(MBB);
            if (ItF != ItBBFlavors->second.end()) {
              for (const auto &OP : ItF->second)
                FlavorsObj.try_emplace(OP.first, OP.second);
            }
          }

          /* Attach the flavors object to the BB object. */
          BBObj["flavors"] = std::move(FlavorsObj);

          /* Append the BB object to the basic_blocks array. */
          Blocks.push_back(std::move(BBObj));
        }

        /* Build the per-function JSON object with total and BB list. */
        llvm::json::Object FObj;
        FObj["total_lrsc_occurrences"] = FuncTotal;
        FObj["basic_blocks"] = std::move(Blocks);

        /* Insert function object into the function map keyed by function name. */
        FuncMap.try_emplace(std::move(Name), std::move(FObj));
      }

      /* Attach the function map to the root object and return as a JSON value. */
      Root["functions"] = std::move(FuncMap);
      return llvm::json::Value(std::move(Root));
    }
  };
}
