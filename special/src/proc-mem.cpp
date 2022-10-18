#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#include "proc-mem.hpp"

namespace dory::special {
static size_t parse_proc_status_size(std::string const &line) {
  std::string title;
  size_t size;

  std::stringstream ss(line);
  ss >> title;
  ss >> size;

  // Linux uses always kB for this file:
  // https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/fs/proc/task_mmu.c?id=39a8804455fb23f09157341d3ba7db6d7ae6ee76#n22
  return size * 1024;
}

MemoryConsumption process_memory_consumption() {
  std::ifstream status_file("/proc/self/status");

  MemoryConsumption consumption;

  std::string line;
  while (std::getline(status_file, line)) {
    if (line.rfind("VmPeak", 0) == 0) {
      consumption.vmPeak = parse_proc_status_size(line);
      continue;
    }

    if (line.rfind("VmSize", 0) == 0) {
      consumption.vmSize = parse_proc_status_size(line);
      continue;
    }

    if (line.rfind("VmLck", 0) == 0) {
      consumption.vmLck = parse_proc_status_size(line);
      continue;
    }

    if (line.rfind("VmPin", 0) == 0) {
      consumption.vmPin = parse_proc_status_size(line);
      continue;
    }

    if (line.rfind("VmHWM", 0) == 0) {
      consumption.vmHwm = parse_proc_status_size(line);
      continue;
    }

    if (line.rfind("VmRSS", 0) == 0) {
      consumption.vmRss = parse_proc_status_size(line);
      continue;
    }

    if (line.rfind("VmData", 0) == 0) {
      consumption.vmData = parse_proc_status_size(line);
      continue;
    }

    if (line.rfind("VmStk", 0) == 0) {
      consumption.vmStk = parse_proc_status_size(line);
      continue;
    }

    if (line.rfind("VmExe", 0) == 0) {
      consumption.vmExe = parse_proc_status_size(line);
      continue;
    }

    if (line.rfind("VmLib", 0) == 0) {
      consumption.vmLib = parse_proc_status_size(line);
      continue;
    }

    if (line.rfind("VmPTE", 0) == 0) {
      consumption.vmPte = parse_proc_status_size(line);
      continue;
    }

    if (line.rfind("VmSwap", 0) == 0) {
      consumption.vmSwap = parse_proc_status_size(line);
      continue;
    }
  }

  return consumption;
}
}  // namespace dory::special
