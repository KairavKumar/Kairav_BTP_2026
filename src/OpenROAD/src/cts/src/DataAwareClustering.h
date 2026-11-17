// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace cts {
struct AlignmentSink
{
  double x, y, cap, target, local_delay;
  double lower = -std::numeric_limits<double>::infinity();
  double upper = std::numeric_limits<double>::infinity();
  double reference = 0;
};
struct AlignmentWindow
{
  int sink;            // -1 denotes an immutable primary-input source.
  double early, late;  // Clock-free offset, including predicted local delay.
};
struct AlignmentGate
{
  double weight;
  std::vector<AlignmentWindow> windows;
  double ceiling = std::numeric_limits<double>::infinity();
};
struct AlignmentTimingCheck
{
  int launch, capture;  // -1 is an immutable external source/capture clock.
  double sign, limit;   // sign*(launch_delta-capture_delta) <= limit.
};

// Timing-first agglomeration. Geometry/capacitance are hard feasibility bounds;
// heap priority is the full affected-gate spread penalty of sharing one root.
inline std::vector<std::vector<unsigned>> clusterAlignedSinks(
    const std::vector<AlignmentSink>& sinks,
    const std::vector<AlignmentGate>& gates,
    unsigned max_size,
    double max_diameter,
    double max_cap,
    double max_target_span,
    double max_merge_penalty = 0,
    const std::vector<AlignmentTimingCheck>& checks = {})
{
  if (!max_size || !(max_diameter > 0) || !(max_cap > 0)
      || !std::isfinite(max_target_span) || max_target_span < 0
      || !std::isfinite(max_merge_penalty) || max_merge_penalty < 0) {
    throw std::invalid_argument("Invalid alignment clustering limits");
  }
  const size_t n = sinks.size();
  std::vector<std::vector<unsigned>> groups(n);
  std::vector<size_t> owner(n), version(n, 0);
  std::vector<std::set<size_t>> affected(n), neighbors(n);
  std::vector<std::set<size_t>> timing_affected(n);
  for (size_t s = 0; s < n; ++s) {
    const auto& v = sinks[s];
    if (!std::isfinite(v.target) || !std::isfinite(v.local_delay)
        || !std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.cap)
        || v.cap < 0 || v.cap > max_cap || !std::isfinite(v.reference)) {
      throw std::invalid_argument("Invalid alignment sink");
    }
    groups[s] = {static_cast<unsigned>(s)};
    owner[s] = s;
  }
  for (size_t c = 0; c < checks.size(); ++c) {
    const auto& check = checks[c];
    if (!std::isfinite(check.limit) || (check.sign != 1 && check.sign != -1)) {
      throw std::invalid_argument("Invalid alignment timing check");
    }
    for (int sink : {check.launch, check.capture}) {
      if (sink < -1 || sink >= static_cast<int>(n)) {
        throw std::invalid_argument("Unknown alignment timing endpoint");
      }
      if (sink >= 0) {
        timing_affected[sink].insert(c);
      }
    }
  }
  for (size_t g = 0; g < gates.size(); ++g) {
    if (!(gates[g].weight > 0) || gates[g].windows.empty()) {
      throw std::invalid_argument("Invalid alignment gate");
    }
    for (const auto& w : gates[g].windows) {
      if (w.sink < -1 || w.sink >= static_cast<int>(n)
          || !std::isfinite(w.early) || !std::isfinite(w.late)
          || w.early > w.late) {
        throw std::invalid_argument("Invalid alignment source window");
      }
      if (w.sink >= 0) {
        affected[w.sink].insert(g);
      }
    }
  }
  auto root = [&](const std::vector<unsigned>& members) {
    double total = 0;
    for (unsigned s : members) {
      total += sinks[s].target - sinks[s].local_delay;
    }
    return total / members.size();
  };
  auto valid = [&](const std::vector<unsigned>& members) {
    if (members.size() > max_size) {
      return false;
    }
    double cap = 0, lo = std::numeric_limits<double>::infinity(), hi = -lo;
    const double common_root = root(members);
    for (unsigned a : members) {
      if (common_root + sinks[a].local_delay < sinks[a].lower - 1e-10
          || common_root + sinks[a].local_delay > sinks[a].upper + 1e-10) {
        return false;
      }
      cap += sinks[a].cap;
      lo = std::min(lo, sinks[a].target - sinks[a].local_delay);
      hi = std::max(hi, sinks[a].target - sinks[a].local_delay);
      for (unsigned b : members) {
        if (std::abs(sinks[a].x - sinks[b].x)
                + std::abs(sinks[a].y - sinks[b].y)
            > max_diameter) {
          return false;
        }
      }
    }
    return cap <= max_cap && hi - lo <= max_target_span;
  };
  auto score = [&](const std::set<size_t>& touched,
                   size_t a,
                   size_t b,
                   double merged_root,
                   bool merge) {
    double total = 0;
    for (size_t g : touched) {
      double lo = std::numeric_limits<double>::infinity(), hi = -lo;
      for (const auto& w : gates[g].windows) {
        double arrival = 0;
        if (w.sink >= 0) {
          const size_t group = owner[w.sink];
          arrival = merge && (group == a || group == b) ? merged_root
                                                        : root(groups[group]);
        }
        lo = std::min(lo, w.early + arrival);
        hi = std::max(hi, w.late + arrival);
      }
      if (merge && hi - lo > gates[g].ceiling + 1e-10) {
        return std::numeric_limits<double>::infinity();
      }
      total += gates[g].weight * (hi - lo);
    }
    return total;
  };
  using Entry = std::tuple<double, size_t, size_t, size_t, size_t>;
  std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
  auto offer = [&](size_t a, size_t b) {
    if (a > b) {
      std::swap(a, b);
    }
    if (a == b || groups[a].empty() || groups[b].empty()) {
      return;
    }
    auto merged = groups[a];
    merged.insert(merged.end(), groups[b].begin(), groups[b].end());
    if (!valid(merged)) {
      return;
    }
    auto touched = affected[a];
    touched.insert(affected[b].begin(), affected[b].end());
    const double target = root(merged);
    auto touched_checks = timing_affected[a];
    touched_checks.insert(timing_affected[b].begin(), timing_affected[b].end());
    auto delta = [&](int sink) {
      if (sink < 0) {
        return 0.0;
      }
      const size_t group = owner[sink];
      return ((group == a || group == b) ? target : root(groups[group]))
             + sinks[sink].local_delay - sinks[sink].reference;
    };
    for (size_t c : touched_checks) {
      const auto& check = checks[c];
      if (check.sign * (delta(check.launch) - delta(check.capture))
          > check.limit + 1e-9) {
        return;
      }
    }
    const double penalty = score(touched, a, b, target, true)
                           - score(touched, a, b, target, false);
    queue.emplace(penalty, a, b, version[a], version[b]);
  };
  // Spatial candidate generation does not assign groups. Every nearby pair can
  // compete on data timing; no pre-existing geometric cluster is privileged.
  for (size_t a = 0; a < n; ++a) {
    for (size_t b = a + 1; b < n; ++b) {
      if (std::abs(sinks[a].x - sinks[b].x) + std::abs(sinks[a].y - sinks[b].y)
          <= max_diameter) {
        neighbors[a].insert(b);
        neighbors[b].insert(a);
        offer(a, b);
      }
    }
  }
  // Spend only an explicitly bounded fraction of the planner's predicted gain
  // on sharing. Every merge separately respects all affected model gate caps.
  double spent = 0;
  while (!queue.empty()) {
    const auto [penalty, a, b, va, vb] = queue.top();
    queue.pop();
    if (groups[a].empty() || groups[b].empty() || va != version[a]
        || vb != version[b]) {
      continue;
    }
    if (!std::isfinite(penalty)
        || spent + std::max(0.0, penalty) > max_merge_penalty + 1e-5) {
      break;
    }
    spent += std::max(0.0, penalty);
    groups[a].insert(groups[a].end(), groups[b].begin(), groups[b].end());
    for (unsigned s : groups[b]) {
      owner[s] = a;
    }
    groups[b].clear();
    affected[a].insert(affected[b].begin(), affected[b].end());
    timing_affected[a].insert(timing_affected[b].begin(),
                              timing_affected[b].end());
    neighbors[a].insert(neighbors[b].begin(), neighbors[b].end());
    ++version[a];
    ++version[b];
    // A changed source can affect the cost of neighboring merges even when
    // neither endpoint is a/b. Re-offer all affected dependency neighbors.
    std::set<size_t> refresh{a};
    for (size_t g : affected[a]) {
      for (const auto& w : gates[g].windows) {
        if (w.sink >= 0) {
          refresh.insert(owner[w.sink]);
        }
      }
    }
    for (size_t c : timing_affected[a]) {
      for (int sink : {checks[c].launch, checks[c].capture}) {
        if (sink >= 0) {
          refresh.insert(owner[sink]);
        }
      }
    }
    for (size_t r : refresh) {
      if (r != a) {
        ++version[r];
      }
      for (size_t neighbor : neighbors[r]) {
        offer(r, owner[neighbor]);
      }
    }
  }
  std::vector<std::vector<unsigned>> result;
  for (auto& group : groups) {
    if (!group.empty()) {
      result.push_back(std::move(group));
    }
  }
  return result;
}
}  // namespace cts
