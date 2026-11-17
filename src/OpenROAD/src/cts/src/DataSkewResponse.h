// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cts {

// One source-output/transition remains distinct until gate extrema are taken.
struct DataSkewSourceRow
{
  size_t source;
  size_t input;
  double early_offset;
  double late_offset;
};

inline std::pair<double, double> sourceArrival(
    const DataSkewSourceRow& row,
    const std::vector<std::pair<double, double>>& launches)
{
  if (row.source >= launches.size()) {
    throw std::runtime_error("Source arrival dimension mismatch");
  }
  return {row.early_offset + launches[row.source].first,
          row.late_offset + launches[row.source].second};
}

struct DataSkewScore
{
  double worst = 0;
  double weighted = 0;
  double movement = 0;
};

// Match the staged selector's 0.001 ps refinement allowances. These compare
// objective quality only; they never loosen a physical acceptance limit.
inline bool betterDataSkewScore(const DataSkewScore& a, const DataSkewScore& b)
{
  constexpr double epsilon_ns = 1e-6;
  if (a.worst > b.worst + epsilon_ns) {
    return false;
  }
  if (a.worst < b.worst - epsilon_ns) {
    return true;
  }
  if (a.weighted > b.weighted + epsilon_ns) {
    return false;
  }
  if (a.weighted < b.weighted - epsilon_ns) {
    return true;
  }
  return a.movement < b.movement - epsilon_ns;
}

}  // namespace cts
