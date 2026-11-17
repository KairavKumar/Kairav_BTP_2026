// SPDX-License-Identifier: BSD-3-Clause
// Experimental target-aware leaf partitioning for data-skew CTS.
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

namespace cts {

// Split existing geometric clusters into subsets with compatible signed clock
// targets. Subsets retain the original fanout, diameter and load upper bounds.
// A finite budget ranks complete splits by target-spread reduction per added
// leaf. Unsplittable/budget-excluded clusters retain their geometric grouping.
// The target-spread score is a topology heuristic, not actual gate timing.
// The span is a topology experiment parameter, not a gate inertial threshold.
inline std::vector<std::vector<unsigned>> partitionDataSkewClusters(
    const std::vector<std::vector<unsigned>>& geometric_clusters,
    const std::vector<double>& targets_ns,
    const double max_span_ns,
    const size_t max_extra_clusters = std::numeric_limits<size_t>::max())
{
  if (!std::isfinite(max_span_ns) || max_span_ns < 0.0) {
    throw std::invalid_argument("Invalid data-skew clustering span");
  }
  std::vector<bool> seen(targets_ns.size(), false);
  struct Candidate
  {
    std::vector<unsigned> original;
    std::vector<std::vector<unsigned>> parts;
    double gain = 0.0;
    bool selected = false;
  };
  std::vector<Candidate> candidates;
  for (const auto& cluster : geometric_clusters) {
    if (cluster.empty()) {
      continue;
    }
    auto ordered = cluster;
    for (const auto index : ordered) {
      if (index >= targets_ns.size() || seen[index]
          || !std::isfinite(targets_ns[index])) {
        throw std::invalid_argument("Invalid data-skew sink coverage");
      }
      seen[index] = true;
    }
    const auto [low, high] = std::minmax_element(
        ordered.begin(), ordered.end(), [&](const auto a, const auto b) {
          return targets_ns[a] < targets_ns[b];
        });
    candidates.push_back({cluster, {}, 0.0, false});
    auto& candidate = candidates.back();
    // Preserve ordering when no split is needed, including the uniform case.
    if (targets_ns[*high] - targets_ns[*low] <= max_span_ns) {
      continue;
    }
    const double original_cost
        = (targets_ns[*high] - targets_ns[*low]) * ordered.size();
    std::stable_sort(ordered.begin(), ordered.end(), [&](auto a, auto b) {
      return targets_ns[a] < targets_ns[b];
    });
    double first_target = targets_ns[ordered.front()];
    candidate.parts.emplace_back();
    for (const auto index : ordered) {
      if (targets_ns[index] - first_target > max_span_ns) {
        candidate.parts.emplace_back();
        first_target = targets_ns[index];
      }
      candidate.parts.back().push_back(index);
    }
    double split_cost = 0.0;
    for (const auto& part : candidate.parts) {
      split_cost
          += (targets_ns[part.back()] - targets_ns[part.front()]) * part.size();
    }
    candidate.gain = original_cost - split_cost;
    if (!std::isfinite(candidate.gain)) {
      throw std::invalid_argument("Nonfinite data-skew split score");
    }
  }
  if (std::find(seen.begin(), seen.end(), false) != seen.end()) {
    throw std::invalid_argument("Incomplete data-skew sink coverage");
  }
  std::vector<size_t> ranked;
  for (size_t index = 0; index < candidates.size(); ++index) {
    if (!candidates[index].parts.empty()) {
      ranked.push_back(index);
    }
  }
  std::stable_sort(ranked.begin(), ranked.end(), [&](auto a, auto b) {
    return candidates[a].gain / (candidates[a].parts.size() - 1)
           > candidates[b].gain / (candidates[b].parts.size() - 1);
  });
  size_t remaining = max_extra_clusters;
  for (const auto index : ranked) {
    auto& candidate = candidates[index];
    const size_t extra = candidate.parts.size() - 1;
    if (extra <= remaining) {
      candidate.selected = true;
      remaining -= extra;
    }
  }
  std::vector<std::vector<unsigned>> result;
  for (const auto& candidate : candidates) {
    if (candidate.selected) {
      result.insert(
          result.end(), candidate.parts.begin(), candidate.parts.end());
    } else {
      result.push_back(candidate.original);
    }
  }
  return result;
}

// Realize a solver-proposed split without silently merging away its controls.
// Each part remains a subset of one original geometric leaf. Fail closed if the
// proposal exceeds the configured branch budget or coverage is incomplete.
inline std::vector<std::vector<unsigned>> partitionPrescribedDataSkewClusters(
    const std::vector<std::vector<unsigned>>& geometric_clusters,
    const std::vector<unsigned>& group_ids,
    const size_t max_extra_clusters)
{
  std::vector<bool> seen(group_ids.size(), false);
  std::vector<std::vector<unsigned>> result;
  size_t added = 0;
  for (const auto& cluster : geometric_clusters) {
    if (cluster.empty()) {
      continue;
    }
    std::map<unsigned, size_t> positions;
    std::vector<std::vector<unsigned>> parts;
    for (const auto index : cluster) {
      if (index >= group_ids.size() || seen[index]) {
        throw std::invalid_argument("Invalid prescribed sink coverage");
      }
      seen[index] = true;
      const auto [it, inserted] = positions.emplace(group_ids[index], parts.size());
      if (inserted) {
        parts.emplace_back();
      }
      parts[it->second].push_back(index);
    }
    const size_t extra = parts.size() - 1;
    if (extra > max_extra_clusters - added) {
      throw std::invalid_argument("Prescribed split exceeds extra-leaf budget");
    }
    added += extra;
    result.insert(result.end(), parts.begin(), parts.end());
  }
  if (std::find(seen.begin(), seen.end(), false) != seen.end()) {
    throw std::invalid_argument("Incomplete prescribed sink coverage");
  }
  return result;
}

// A stock-referenced plan can cross newly computed geometric clusters after
// legalization. Preserve its groups globally, but revalidate physical limits.
inline std::vector<std::vector<unsigned>> globalPrescribedDataSkewClusters(
    const std::vector<unsigned>& group_ids,
    const std::vector<std::pair<float, float>>& points,
    const size_t geometric_count,
    const size_t max_extra_clusters,
    const unsigned max_size,
    const double max_diameter)
{
  if (group_ids.size() != points.size() || max_size == 0
      || !std::isfinite(max_diameter) || max_diameter <= 0) {
    throw std::invalid_argument("Invalid prescribed geometry contract");
  }
  std::map<unsigned, std::vector<unsigned>> parts;
  for (unsigned i = 0; i < group_ids.size(); ++i) {
    if (!std::isfinite(points[i].first) || !std::isfinite(points[i].second)) {
      throw std::invalid_argument("Nonfinite prescribed sink location");
    }
    parts[group_ids[i]].push_back(i);
  }
  if (parts.size() > geometric_count
      && parts.size() - geometric_count > max_extra_clusters) {
    throw std::invalid_argument("Global prescribed split exceeds leaf budget");
  }
  std::vector<std::vector<unsigned>> result;
  for (const auto& [group, members] : parts) {
    if (members.size() > max_size) {
      throw std::invalid_argument("Prescribed group exceeds sink fanout");
    }
    for (unsigned i : members) {
      for (unsigned j : members) {
        const double dx = double(points[i].first) - points[j].first;
        const double dy = double(points[i].second) - points[j].second;
        if (std::hypot(dx, dy) > max_diameter + 1e-6) {
          throw std::invalid_argument("Prescribed group exceeds diameter");
        }
      }
    }
    result.push_back(members);
  }
  return result;
}

}  // namespace cts
