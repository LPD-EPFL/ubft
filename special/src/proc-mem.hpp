#pragma once

#include <cstddef>
#include <sstream>

namespace dory::special {
struct MemoryConsumption {
  size_t vmPeak;
  size_t vmSize;
  size_t vmLck;
  size_t vmPin;
  size_t vmHwm;
  size_t vmRss;
  size_t vmData;
  size_t vmStk;
  size_t vmExe;
  size_t vmLib;
  size_t vmPte;
  size_t vmSwap;

  std::string toString() {
    std::stringstream ss;
    ss << "VmPeak: " << vmPeak << "\n"
       << "VmSize: " << vmSize << "\n"
       << "VmLck: " << vmLck << "\n"
       << "VmPin: " << vmPin << "\n"
       << "VmHwm: " << vmHwm << "\n"
       << "VmRss: " << vmRss << "\n"
       << "VmData: " << vmData << "\n"
       << "VmStk: " << vmStk << "\n"
       << "VmExe: " << vmExe << "\n"
       << "VmLib: " << vmLib << "\n"
       << "VmPte: " << vmPte << "\n"
       << "VmSwap: " << vmSwap << "\n";

    return ss.str();
  }
};

MemoryConsumption process_memory_consumption();
}  // namespace dory::special
