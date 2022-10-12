#pragma once

#include <fmt/chrono.h>
#include <fmt/core.h>

#include <iostream>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <numeric>
#include <vector>

class LatencyProfiler {
 public:
  using Nano = std::chrono::nanoseconds;
  using Micro = std::chrono::microseconds;
  using Milli = std::chrono::milliseconds;

  struct MeasurementGroup {
    Nano start;
    Nano end;
    Nano granularity;
    size_t indices;
    size_t start_idx;

    MeasurementGroup(Nano start, Nano end, Nano granularity)
        : start{start}, end{end}, granularity{granularity} {
      indices = static_cast<size_t>((end - start) / granularity);

      if (start + indices * granularity != end) {
        throw std::runtime_error("Imperfect granularity!");
      }
    }
  };

  LatencyProfiler(size_t skip = 0) : skip{skip} {
    grp.emplace_back(Nano(0), Nano(1000), Nano(1));
    grp.emplace_back(Micro(1), Micro(10), Nano(10));
    grp.emplace_back(Micro(10), Micro(100), Nano(20));
    grp.emplace_back(Micro(100), Milli(1), Nano(50));
    grp.emplace_back(Milli(1), Milli(100), Micro(100));

    // TODO(anon): Check for perfect overlap

    // Compute the start_idx for all groups
    size_t start_idx = 0;
    for (auto &g : grp) {
      g.start_idx = start_idx;
      start_idx += g.indices;
    }

    // Set the vector size to fit the buckets of all groups
    freq.resize(start_idx);
  }

  template <typename Duration>
  void addMeasurement(Duration const &duration) {
    auto d = std::chrono::duration_cast<Nano>(duration);
    auto count = d.count();

    if (measurement_idx++ < skip) {
      return;
    }

    if (duration >= grp.back().end) {
      fmt::print("Measurement of {} exceeded {}.\n", duration, grp.back().end);
      return;
    }

    // Find the right group
    auto it = std::lower_bound(grp.begin(), grp.end(), d,
                               [](MeasurementGroup const &g, Nano duration) {
                                 return g.start <= duration;
                               });

    auto group_index = static_cast<size_t>(std::distance(grp.begin(), it - 1));

    // Find the index inside the group
    auto &group = grp.at(group_index);
    auto freq_index = group.start_idx + static_cast<size_t>((d - group.start) /
                                                            group.granularity);

    freq.at(freq_index)++;
  }

  Nano percentile(double perc) {
    auto acc_freq(freq);
    auto measurents_cnt =
        std::accumulate(acc_freq.begin(), acc_freq.end(), 0UL);

    std::partial_sum(acc_freq.begin(), acc_freq.end(), acc_freq.begin());

    auto it_freq =
        std::lower_bound(acc_freq.begin(), acc_freq.end(),
                         static_cast<double>(measurents_cnt) * perc / 100.0);

    auto freq_idx =
        static_cast<size_t>(std::distance(acc_freq.begin(), it_freq));

    // Find the right group
    auto it =
        std::lower_bound(grp.begin(), grp.end(), freq_idx,
                         [](MeasurementGroup const &g, uint64_t freq_idx) {
                           return g.start_idx <= freq_idx;
                         });

    auto group_index = static_cast<size_t>(std::distance(grp.begin(), it - 1));

    // Find the index inside the group
    auto &group = grp.at(group_index);
    auto time = group.start + (freq_idx - group.start_idx) * group.granularity;

    return time + group.granularity;
  }

  template <typename Duration>
  std::string prettyTime(Duration const &d) {
    if (d < Nano(1000)) {
      Nano dd = std::chrono::duration_cast<Nano>(d);
      return std::to_string(dd.count()) + "ns";
    }

    if (d < Micro(1000)) {
      Micro dd = std::chrono::duration_cast<Micro>(d);
      return std::to_string(dd.count()) + "us";
    }

    /*if (d < Milli(1000))*/ {
      Milli dd = std::chrono::duration_cast<Milli>(d);
      return std::to_string(dd.count()) + "ms";
    }
  }

  void report() {
    std::cout << "Skipping " << skip << " initial measurements"
              << "\n";

    uint64_t total = std::accumulate(freq.begin(), freq.end(), 0UL);
    std::cout << "Total number of measurements: " << total << "\n";

    for (auto &g : grp) {
      auto meas_cnt = std::accumulate(
          freq.begin() +
              static_cast<std::vector<uint64_t>::difference_type>(g.start_idx),
          freq.begin() + static_cast<std::vector<uint64_t>::difference_type>(
                             g.start_idx + g.indices),
          0UL);

      std::cout << "Total number of measurements [" << prettyTime(g.start)
                << ", " << prettyTime(g.end) << "): " << meas_cnt << "\n";
    }

    std::cout << "Mean without extremes (0.5*50th + 0.1*60th + 0.1*70th + 0.1*80th + 0.1*90th 0.1*95th)/0.95 " << ((50*percentile(50.0) + 10*percentile(60.0) + 10*percentile(70.0) + 10*percentile(80.0) + 10*percentile(90.0) + 5 * percentile(95.0)) / 95).count() << std::endl;

    std::cout << "50th-percentile (ns): " << percentile(50.0).count() << "\n"
              << "60th-percentile (ns): " << percentile(60.0).count() << "\n"
              << "70th-percentile (ns): " << percentile(70.0).count() << "\n"
              << "80th-percentile (ns): " << percentile(80.0).count() << "\n"
              << "90th-percentile (ns): " << percentile(90.0).count() << "\n"
              << "95th-percentile (ns): " << percentile(95.0).count() << "\n"
              << "98th-percentile (ns): " << percentile(98.0).count() << "\n"
              << "99th-percentile (ns): " << percentile(99.0).count() << "\n"
              << "99.5th-percentile (ns): " << percentile(99.5).count() << "\n"
              << "99.9th-percentile (ns): " << percentile(99.9).count() << "\n"
              << "99.99th-percentile (ns): " << percentile(99.99).count()
              << "\n"
              << "99.999th-percentile (ns): " << percentile(99.999).count()
              << "\n"
              << "99.9999th-percentile (ns): " << percentile(99.9999).count()
              << "\n"
              << "99.99999th-percentile (ns): " << percentile(99.99999).count()
              << "\n"
              << "99.999999th-percentile (ns): "
              << percentile(99.999999).count() << "\n"
              << "99.9999999th-percentile (ns): "
              << percentile(99.9999999).count() << "\n"
              << "99.99999999th-percentile (ns): "
              << percentile(99.99999999).count() << "\n"
              << "99.999999999th-percentile (ns): "
              << percentile(99.999999999).count() << "\n"
              << std::endl;
  }

  void reportOnce() {
    if (!reported) {
      report();
      reported = true;
    }
  }

  void reportBuckets() {
    for (auto &g : grp) {
      std::cout << "Reporting detailed data for range (in ns) ["
                << prettyTime(g.start) << ", " << prettyTime(g.end) << ")"
                << "\n";

      for (size_t i = 0; i < g.indices; i++) {
        auto f = freq.at(g.start_idx + i);
        if (f == 0) {
          continue;
        }

        std::cout << "[" << (g.start + i * g.granularity).count() << ", "
                  << (g.start + (i + 1) * g.granularity).count() << ") " << f
                  << "\n";
      }
      std::cout << std::endl;
    }
  }

  size_t measured() const {
    return skip >= measurement_idx ? 0 : (measurement_idx - skip);
  }

 private:
  size_t const skip;
  size_t measurement_idx = 0;
  bool reported = false;
  std::vector<MeasurementGroup> grp;
  std::vector<uint64_t> freq;
};
