/* Copyright 2026 PixelsDB. Licensed under AGPL-3.0. */
#ifndef PIXELS_CPU_AFFINITY_H
#define PIXELS_CPU_AFFINITY_H

#include <sched.h>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace duckdb {
inline std::vector<int> ParseCPUAffinityMapping(const std::string &mapping) {
  std::vector<int> result;
  size_t start = 0;
  while (start < mapping.size()) {
    size_t end = mapping.find(',', start);
    std::string token = mapping.substr(start, end == std::string::npos ? end : end - start);
    if (token.empty()) throw std::invalid_argument("empty CPU affinity entry");
    size_t consumed = 0;
    int cpu = std::stoi(token, &consumed);
    if (consumed != token.size() || cpu < 0) throw std::invalid_argument("invalid CPU affinity entry");
    result.push_back(cpu);
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return result;
}

inline bool ApplyCPUAffinity(const std::string &strategy, const std::string &mapping,
                             uint64_t worker_id) {
  if (strategy != "round-robin" && strategy != "mapping")
    throw std::invalid_argument("unknown CPU affinity strategy: " + strategy);
  auto cpus = ParseCPUAffinityMapping(mapping);
  if (cpus.empty() && strategy == "round-robin") {
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) == 0) {
      for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &allowed)) cpus.push_back(cpu);
      }
    }
    if (cpus.empty()) {
      auto count = std::thread::hardware_concurrency();
      if (!count) return false;
      cpus.resize(count);
      for (unsigned i = 0; i < count; ++i) cpus[i] = static_cast<int>(i);
    }
  }
  if (cpus.empty()) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpus[worker_id % cpus.size()], &set);
  return sched_setaffinity(0, sizeof(set), &set) == 0;
}
} // namespace duckdb
#endif
