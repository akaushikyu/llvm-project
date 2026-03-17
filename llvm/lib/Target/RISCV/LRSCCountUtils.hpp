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

    using BBKey = const MachineBasicBlock *;
    using OpKey = std::string;

    /*--------------------------------------------------------------------------*/
    /* BBList: preserves an explicit traversal/iteration order of BB pointers
       for a given MachineFunction. This is used to emit stable bb_index values. */
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
    /* Updates the per-function total count for the provided Func by adding mfCount. */
    void updateFuncCnt(unsigned  mfCount){
      functionLRSCCount += mfCount;
    }

    /*--------------------------------------------------------------------------*/
    /* Updates the per-basic-block total count for (MF, MBB) by adding bbCount. */
    void updateBBCnt(const MachineBasicBlock &MBB,unsigned bbCount){
      BBCount[&MBB] += bbCount;
    }

    /*--------------------------------------------------------------------------*/
    /* Increments the opcode/flavor counter for (MF, MBB, OpKey) by 1. */
    void updateBBFlavCnt(const MachineBasicBlock &MBB, std::string OpKey){
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
        BBObj["lrsc_count"] = MBB ? BBCount[MBB]: 0;

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
}
