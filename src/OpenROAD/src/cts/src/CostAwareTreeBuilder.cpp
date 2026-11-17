// SPDX-License-Identifier: BSD-3-Clause
#include "CostAwareTreeBuilder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "DataAwareClustering.h"
#include "db_sta/dbNetwork.hh"
#include "db_sta/dbSta.hh"
#include "sta/Liberty.hh"
#include "sta/MinMax.hh"
#include "sta/Mode.hh"
#include "sta/Sdc.hh"
#include "sta/TimingArc.hh"
#include "sta/TimingModel.hh"

namespace cts {
namespace {
constexpr double ns = 1e-9;
// Rising-edge launch alignment is primary in this experimental mode. Falling
// latency remains modeled and its actual movement is checked after
// construction.
constexpr double fall_importance = .05;
struct Cell
{
  sta::LibertyCell* liberty;
  sta::LibertyPort* input;
  sta::LibertyPort* output;
  double cap;
};
struct Sink
{
  ClockInst* inst;
  double rise, fall, weight;
};
struct State;
using Choice = std::shared_ptr<State>;
struct State
{
  // Desired arrival at this subtree's input, aggregated over its registers.
  double weight = 0, sum = 0, square = 0;
  double fall_sum = 0, fall_square = 0;
  double energy = 0, cap = 0;
  int x = 0, y = 0, registers = 0, buffers = 0, depth = 0;
  std::vector<int> chain;
  std::vector<Choice> children;
  std::vector<unsigned> sinks;
  std::string tag;
};

double mean(const Choice& state)
{
  return (state->sum + fall_importance * state->fall_sum)
         / ((1 + fall_importance) * state->weight);
}
double centeredError(const Choice& state)
{
  return std::max(0.0,
                  (state->square + fall_importance * state->fall_square)
                          / ((1 + fall_importance) * state->weight)
                      - mean(state) * mean(state))
         * 1e6;
}
void shift(State& state, double rise, double fall)
{
  state.square += rise * rise * state.weight - 2 * rise * state.sum;
  state.sum -= rise * state.weight;
  state.fall_square += fall * fall * state.weight - 2 * fall * state.fall_sum;
  state.fall_sum -= fall * state.weight;
}
}  // namespace

void CostAwareTreeBuilder::run()
{
  auto* block = clock_.getNetObj()->getBlock();
  auto real = [&](const char* name, double fallback) {
    auto* p = odb::dbDoubleProperty::find(block, name);
    const double v = p ? p->getValue() : fallback;
    if (!std::isfinite(v)) {
      throw std::runtime_error("Nonfinite cost-aware CTS setting");
    }
    return v;
  };
  const double energy_weight = real("cts_cost_energy_weight", 20.0);
  auto* frontier_property
      = odb::dbIntProperty::find(block, "cts_cost_pareto_frontier");
  const bool pareto_frontier
      = frontier_property && frontier_property->getValue() != 0;
  const double energy_budget
      = real("cts_cost_energy_budget_fj_per_register", 0.0);
  if (energy_budget < 0) {
    throw std::runtime_error("Negative clock energy budget");
  }
  const double target_span = real("cts_cost_target_span_ns", .020);
  const double half_period = real("cts_cost_half_period_ns", 0);
  const double bin = real("cts_cost_latency_bin_ns", .001);
  const double input_slew = real("cts_cost_input_slew_ns", .020) * ns;
  const double diameter = options_->getMaxDiameter() * options_->getDbUnits();
  const unsigned leaf_size = options_->getSinkClusteringSize();
  std::string leaf_master;
  if (auto* p = odb::dbStringProperty::find(block, "cts_cost_leaf_master")) {
    leaf_master = p->getValue();
  }
  const double leaf_max_slew = real("cts_cost_leaf_max_slew_ns", 0) * ns;
  if (leaf_max_slew < 0) {
    throw std::runtime_error("Negative leaf slew limit");
  }
  std::map<std::string, std::array<double, 2>> measured_slews;
  if (auto* p = odb::dbStringProperty::find(block, "cts_cost_slews")) {
    std::istringstream data(p->getValue());
    std::string tag;
    double r, f;
    while (data >> tag) {
      if (!(data >> r >> f) || !std::isfinite(r) || !std::isfinite(f) || r <= 0
          || f <= 0) {
        throw std::runtime_error("Malformed measured subtree slews");
      }
      measured_slews[tag] = {r * ns, f * ns};
    }
  }
  if (energy_weight < 0 || target_span <= 0 || half_period <= 0 || bin <= 0
      || input_slew <= 0 || !leaf_size || diameter <= 0) {
    throw std::runtime_error("Invalid cost-aware CTS settings/clock period");
  }
  unsigned beam = 96;
  unsigned upper_fanout = 32;
  if (auto* p = odb::dbIntProperty::find(block, "cts_cost_upper_fanout")) {
    if (p->getValue() < 2 || p->getValue() > 64) {
      throw std::runtime_error("Cost-aware upper fanout must be 2..64");
    }
    upper_fanout = p->getValue();
  }
  if (auto* p = odb::dbIntProperty::find(block, "cts_cost_frontier")) {
    if (p->getValue() < 4 || p->getValue() > 128) {
      throw std::runtime_error("Cost-aware frontier must be 4..128");
    }
    beam = p->getValue();
  }
  std::vector<Cell> cells;
  for (const auto& name : options_->getBufferList()) {
    auto* cell = network_->findLibertyCell(name.c_str());
    if (!cell || !cell->isBuffer() || cell->isInverter()
        || !db_->findMaster(name.c_str())) {
      continue;
    }
    sta::LibertyPort *a, *z;
    cell->bufferPorts(a, z);
    bool conditional = false;
    for (const auto& power : cell->internalPowers()) {
      conditional |= power.when() != nullptr;
    }
    if (conditional) {
      continue;
    }
    cells.push_back({cell, a, z, a->capacitance(sta::MinMax::max())});
  }
  if (cells.empty()) {
    throw std::runtime_error("No characterized unconditional buffer choices");
  }
  const auto* pvt
      = sta_->cmdMode()->sdc()->operatingConditions(sta::MinMax::max());
  const double voltage
      = pvt ? pvt->voltage()
            : cells.front().liberty->libertyLibrary()->nominalVoltage();
  const double cap_per_dbu = techChar_->getCapPerDBU();
  const double res_per_dbu = techChar_->getResPerDBU();
  auto characterize = [&](int index,
                          double load,
                          std::array<double, 2> slew,
                          std::array<double, 2>& delay,
                          std::array<double, 2>& transition,
                          double& energy) {
    const auto& c = cells[index];
    float limit;
    bool exists;
    c.output->capacitanceLimit(sta::MinMax::max(), limit, exists);
    if (exists && load > limit * .95) {
      return false;
    }
    energy = load * voltage * voltage * 1e15;
    for (int edge = 0; edge < 2; ++edge) {
      auto* rf = edge == 0 ? sta::RiseFall::rise() : sta::RiseFall::fall();
      double d = 0, s = 0;
      int count = 0;
      for (auto* arcs : c.liberty->timingArcSets(c.input, c.output)) {
        for (auto* arc : arcs->arcs()) {
          auto* model = dynamic_cast<sta::GateTimingModel*>(arc->model());
          if (model && arc->fromEdge()->asRiseFall() == rf
              && arc->toEdge()->asRiseFall() == rf) {
            float value, out;
            model->gateDelay(pvt, slew[edge], load, value, out);
            d += value / ns;
            s = std::max(s, double(out));
            ++count;
          }
        }
      }
      if (!count || !std::isfinite(d) || d <= 0 || s > .15 * ns) {
        return false;
      }
      delay[edge] = d / count;
      transition[edge] = s;
      for (const auto& power : c.liberty->internalPowers()) {
        energy += power.power(rf, pvt, slew[edge], load) * 1e15;
      }
    }
    return std::isfinite(energy) && energy >= 0;
  };

  std::vector<Sink> sinks;
  std::vector<AlignmentSink> grouping_sinks;
  std::vector<AlignmentGate> grouping_gates;
  std::map<std::string, int> sink_index;
  clock_.forEachSink([&](ClockInst& inst) {
    auto* pin = inst.getDbInputPin();
    auto* target = odb::dbDoubleProperty::find(pin, "cts_align_target_ns");
    auto* reference
        = odb::dbDoubleProperty::find(pin, "cts_align_reference_ns");
    auto* fall = odb::dbDoubleProperty::find(pin, "cts_align_stock_fall_ns");
    if (!target || !reference || !fall) {
      throw std::runtime_error("Missing absolute cost-aware clock targets");
    }
    double weight = 1;
    const unsigned index = sinks.size();
    sink_index[std::string(pin->getInst()->getName()) + "/"
               + pin->getMTerm()->getName()] = index;
    if (auto* p = odb::dbStringProperty::find(pin, "cts_align_profile")) {
      std::istringstream data(p->getValue());
      unsigned gate;
      double lo, hi, w;
      while (data >> gate) {
        if (!(data >> lo >> hi >> w) || !std::isfinite(lo) || !std::isfinite(hi)
            || !std::isfinite(w) || w <= 0 || hi < lo) {
          throw std::runtime_error("Malformed source-preserving sink window");
        }
        weight += w;
        grouping_gates.resize(
            std::max(grouping_gates.size(), size_t(gate + 1)));
        grouping_gates[gate].weight = w;
        grouping_gates[gate].windows.push_back({int(index), lo, hi});
      }
      if (!data.eof()) {
        throw std::runtime_error("Malformed source-preserving sink profile");
      }
    }
    const double rise = target->getValue();
    const double f
        = fall->getValue() - half_period + rise - reference->getValue();
    if (!std::isfinite(rise) || !std::isfinite(f) || weight <= 0) {
      throw std::runtime_error("Invalid cost-aware sink requirements");
    }
    sinks.push_back({&inst, rise, f, weight});
    grouping_sinks.push_back({double(inst.getX()),
                              double(inst.getY()),
                              inst.getInputCap(),
                              rise,
                              0});
    grouping_sinks.back().reference = reference->getValue();
  });
  if (sinks.empty()) {
    throw std::runtime_error("Cost-aware CTS has no sinks");
  }
  if (auto* p = odb::dbStringProperty::find(block, "cts_align_fixed")) {
    std::istringstream data(p->getValue());
    unsigned gate;
    double lo, hi, weight;
    while (data >> gate) {
      if (!(data >> lo >> hi >> weight)) {
        throw std::runtime_error("Malformed fixed-source windows");
      }
      grouping_gates.resize(std::max(grouping_gates.size(), size_t(gate + 1)));
      grouping_gates[gate].weight = weight;
      grouping_gates[gate].windows.push_back({-1, lo, hi});
    }
  }
  // Empty profiles are permitted for delivery-only toy fixtures.
  if (grouping_gates.empty()) {
    grouping_gates.push_back({1, {{-1, 0, 0}}});
  }
  std::vector<AlignmentTimingCheck> timing_checks;
  if (auto* p = odb::dbStringProperty::find(block, "cts_align_timing_checks")) {
    std::istringstream data(p->getValue());
    std::string launch, capture;
    double sign, limit;
    while (data >> launch) {
      if (!(data >> capture >> sign >> limit)) {
        throw std::runtime_error("Malformed shared-branch timing constraints");
      }
      auto endpoint = [&](const std::string& pin) {
        if (pin == "FIXED") {
          return -1;
        }
        return sink_index.at(pin);
      };
      timing_checks.push_back(
          {endpoint(launch), endpoint(capture), sign, limit});
    }
  }
  double largest_cap = 0;
  for (const auto& s : grouping_sinks) {
    largest_cap = std::max(largest_cap, s.cap);
  }
  std::vector<std::vector<unsigned>> groups;
  std::map<int, std::vector<unsigned>> prescribed;
  size_t covered = 0;
  for (unsigned i = 0; i < sinks.size(); ++i) {
    if (auto* p = odb::dbIntProperty::find(sinks[i].inst->getDbInputPin(),
                                           "cts_cost_prescribed")) {
      if (p->getValue() < 0) {
        throw std::runtime_error("Negative shared-branch group");
      }
      prescribed[p->getValue()].push_back(i);
      ++covered;
    }
  }
  if (!prescribed.empty()) {
    if (covered != sinks.size()) {
      throw std::runtime_error("Incomplete shared-branch grouping");
    }
    for (const auto& [id, members] : prescribed) {
      if (members.size() > leaf_size) {
        throw std::runtime_error("Shared-branch fanout exceeded");
      }
      double cap = 0;
      for (unsigned a : members) {
        cap += grouping_sinks[a].cap;
        for (unsigned b : members) {
          if (std::abs(grouping_sinks[a].x - grouping_sinks[b].x)
                      + std::abs(grouping_sinks[a].y - grouping_sinks[b].y)
                  > diameter
              || std::abs(sinks[a].rise - sinks[b].rise) > target_span + 1e-9) {
            throw std::runtime_error("Incompatible prescribed shared branch");
          }
        }
      }
      if (cap > largest_cap * leaf_size + 1e-30) {
        throw std::runtime_error("Shared-branch capacitance exceeded");
      }
      groups.push_back(members);
    }
    for (const auto& check : timing_checks) {
      auto delta = [&](int s) {
        return s < 0 ? 0.0
                     : grouping_sinks[s].target - grouping_sinks[s].reference;
      };
      if (check.sign * (delta(check.launch) - delta(check.capture))
          > check.limit + 1e-7) {
        throw std::runtime_error(
            "Prescribed clock schedule violates planning timing");
      }
    }
  } else {
    groups = clusterAlignedSinks(grouping_sinks,
                                 grouping_gates,
                                 leaf_size,
                                 diameter,
                                 largest_cap * leaf_size,
                                 target_span,
                                 real("cts_cost_merge_penalty_ns", 30),
                                 timing_checks);
  }
  std::vector<double> group_x, group_y, group_time;
  for (const auto& group : groups) {
    double x = 0, y = 0, t = 0;
    for (unsigned i : group) {
      x += sinks[i].inst->getX();
      y += sinks[i].inst->getY();
      t += sinks[i].rise;
    }
    group_x.push_back(x / group.size());
    group_y.push_back(y / group.size());
    group_time.push_back(t / group.size());
  }
  auto score = [&](const Choice& state) {
    return centeredError(state)
           + energy_weight * state->energy / state->registers;
  };
  auto prune = [&](std::vector<Choice> choices, unsigned maximum) {
    if (pareto_frontier) {
      // Approximate DP frontier on the chosen hierarchy. Quantized latency
      // and capped state count deliberately sacrifice exactness. Capacitance
      // and depth remain dominance coordinates because they affect ancestors.
      std::map<int, std::vector<Choice>> frontier;
      std::map<int, Choice> backbone_buckets;
      auto dominates = [&](const Choice& a, const Choice& b) {
        return centeredError(a) <= centeredError(b) && a->energy <= b->energy
               && a->cap <= b->cap && a->depth <= b->depth;
      };
      for (auto& state : choices) {
        if (mean(state) < -.005) {
          continue;
        }
        const int latency = std::lround(mean(state) / bin);
        auto previous = backbone_buckets.find(latency);
        if (previous == backbone_buckets.end()
            || score(state) < score(previous->second)) {
          backbone_buckets[latency] = state;
        }
        auto& bucket = frontier[std::lround(mean(state) / bin)];
        if (std::any_of(bucket.begin(), bucket.end(), [&](const Choice& other) {
              return dominates(other, state);
            })) {
          continue;
        }
        bucket.erase(std::remove_if(bucket.begin(), bucket.end(),
                                    [&](const Choice& other) {
                                      return dominates(state, other);
                                    }),
                     bucket.end());
        bucket.push_back(std::move(state));
      }
      std::vector<Choice> retained;
      for (auto& [key, bucket] : frontier) {
        retained.insert(retained.end(), bucket.begin(), bucket.end());
      }
      std::vector<Choice> backbone;
      for (const auto& [key, state] : backbone_buckets) {
        backbone.push_back(state);
      }
      if (backbone.size() > maximum) {
        std::set<size_t> indices;
        auto nearest = std::min_element(
            backbone.begin(), backbone.end(), [&](const Choice& a, const Choice& b) {
              return std::abs(mean(a)) < std::abs(mean(b));
            });
        auto cheapest = std::min_element(
            backbone.begin(), backbone.end(), [&](const Choice& a, const Choice& b) {
              return score(a) < score(b);
            });
        indices.insert(nearest - backbone.begin());
        indices.insert(cheapest - backbone.begin());
        for (unsigned i = 0; i < maximum && indices.size() < maximum; ++i) {
          indices.insert(size_t(i) * (backbone.size() - 1) / (maximum - 1));
        }
        std::vector<Choice> sampled;
        for (size_t index : indices) {
          sampled.push_back(backbone[index]);
        }
        backbone = std::move(sampled);
      }
      // Preserve the original latency-coverage beam plus a separately bounded
      // cost frontier. Dense cost buckets must not evict timing witnesses.
      const unsigned extra_limit = std::max(2u, maximum / 2);
      if (retained.size() <= extra_limit) {
        backbone.insert(backbone.end(), retained.begin(), retained.end());
        return backbone;
      }
      std::vector<size_t> energy_order(retained.size());
      std::iota(energy_order.begin(), energy_order.end(), 0);
      std::stable_sort(energy_order.begin(), energy_order.end(),
                       [&](size_t a, size_t b) {
                         return retained[a]->energy < retained[b]->energy;
                       });
      std::set<size_t> selected;
      auto keep_best = [&](auto metric) {
        auto best = std::min_element(retained.begin(), retained.end(),
                                     [&](const Choice& a, const Choice& b) {
                                       return metric(a) < metric(b);
                                     });
        selected.insert(best - retained.begin());
      };
      keep_best([](const Choice& s) { return std::abs(mean(s)); });
      keep_best([](const Choice& s) { return centeredError(s); });
      selected.insert(energy_order.front());
      for (unsigned i = 0; i < extra_limit && selected.size() < extra_limit; ++i) {
        const size_t index = size_t(i) * (retained.size() - 1) / (extra_limit - 1);
        selected.insert(index);
        if (selected.size() < extra_limit) {
          selected.insert(energy_order[index]);
        }
      }
      std::vector<Choice> result = std::move(backbone);
      for (size_t index : selected) {
        result.push_back(retained[index]);
      }
      return result;
    }
    // Keep latency diversity: a faster subtree is not automatically better.
    std::map<int, Choice> buckets;
    for (auto& state : choices) {
      if (mean(state) < -.005) {
        continue;
      }
      const int key = std::lround(mean(state) / bin);
      auto it = buckets.find(key);
      if (it == buckets.end() || score(state) < score(it->second)) {
        buckets[key] = std::move(state);
      }
    }
    std::vector<Choice> result;
    for (auto& [key, value] : buckets) {
      result.push_back(std::move(value));
    }
    if (result.size() > maximum) {
      std::vector<Choice> selected;
      std::set<size_t> indices;
      auto nearest = std::min_element(
          result.begin(), result.end(), [&](const Choice& a, const Choice& b) {
            return std::abs(mean(a)) < std::abs(mean(b));
          });
      auto cheapest = std::min_element(
          result.begin(), result.end(), [&](const Choice& a, const Choice& b) {
            return score(a) < score(b);
          });
      indices.insert(nearest - result.begin());
      indices.insert(cheapest - result.begin());
      for (unsigned i = 0; i < maximum && indices.size() < maximum; ++i) {
        indices.insert((size_t(i) * (result.size() - 1)) / (maximum - 1));
      }
      for (size_t i : indices) {
        selected.push_back(result[i]);
      }
      return selected;
    }
    return result;
  };

  std::function<std::vector<Choice>(
      std::vector<unsigned>, unsigned, std::string)>
      build;
  build = [&](std::vector<unsigned> members, unsigned depth, std::string tag) {
    if (depth >= options_->getClockTreeMaxDepth()) {
      throw std::runtime_error("Cost-aware partition exceeds tree depth");
    }
    double xmin = std::numeric_limits<double>::infinity(), xmax = -xmin;
    double ymin = xmin, ymax = -xmin, tmin = xmin, tmax = -xmin;
    long long total_x = 0, total_y = 0;
    size_t registers = 0;
    for (unsigned i : members) {
      xmin = std::min(xmin, group_x[i]);
      xmax = std::max(xmax, group_x[i]);
      ymin = std::min(ymin, group_y[i]);
      ymax = std::max(ymax, group_y[i]);
      tmin = std::min(tmin, group_time[i]);
      tmax = std::max(tmax, group_time[i]);
      for (unsigned s : groups[i]) {
        total_x += sinks[s].inst->getX();
        total_y += sinks[s].inst->getY();
        ++registers;
      }
    }
    const int x = total_x / registers, y = total_y / registers;
    const unsigned fanout_limit
        = options_->getMaxFanout() ? options_->getMaxFanout() : upper_fanout;
    if (fanout_limit < 2 && members.size() > 1) {
      throw std::runtime_error("Clock branching requires fanout >=2");
    }
    const bool leaf = members.size() == 1;
    std::vector<Choice> combinations;
    if (leaf) {
      auto state = std::make_shared<State>();
      state->x = x;
      state->y = y;
      state->sinks = groups[members[0]];
      double net_xmin = x, net_xmax = x, net_ymin = y, net_ymax = y;
      for (unsigned i : state->sinks) {
        const auto& s = sinks[i];
        const double length
            = std::abs(x - s.inst->getX()) + std::abs(y - s.inst->getY());
        const double wc = length * cap_per_dbu;
        const double wire
            = length * res_per_dbu * (s.inst->getInputCap() + wc / 2) / ns;
        const double r = s.rise - wire, f = s.fall - wire;
        state->weight += s.weight;
        state->sum += s.weight * r;
        state->square += s.weight * r * r;
        state->fall_sum += s.weight * f;
        state->fall_square += s.weight * f * f;
        state->cap += s.inst->getInputCap();
        net_xmin = std::min(net_xmin, double(s.inst->getX()));
        net_xmax = std::max(net_xmax, double(s.inst->getX()));
        net_ymin = std::min(net_ymin, double(s.inst->getY()));
        net_ymax = std::max(net_ymax, double(s.inst->getY()));
      }
      state->cap
          += 1.2 * (net_xmax - net_xmin + net_ymax - net_ymin) * cap_per_dbu;
      state->registers = state->sinks.size();
      combinations.push_back(state);
    } else {
      int axis = xmax - xmin >= ymax - ymin ? 0 : 1;
      if ((tmax - tmin) / target_span
          > std::max(xmax - xmin, ymax - ymin) / diameter) {
        axis = 2;
      }
      std::stable_sort(
          members.begin(), members.end(), [&](unsigned a, unsigned b) {
            const double ka = axis == 2   ? group_time[a]
                              : axis == 0 ? group_x[a]
                                          : group_y[a];
            const double kb = axis == 2   ? group_time[b]
                              : axis == 0 ? group_x[b]
                                          : group_y[b];
            return ka == kb ? a < b : ka < kb;
          });
      const unsigned count = std::min<size_t>(
          std::min(upper_fanout, fanout_limit), members.size());
      for (unsigned group = 0; group < count; ++group) {
        std::vector<unsigned> part(
            members.begin() + group * members.size() / count,
            members.begin() + (group + 1) * members.size() / count);
        auto alternatives = build(
            std::move(part), depth + 1, tag + "_" + std::to_string(group));
        std::vector<Choice> expanded;
        for (const auto& child : alternatives) {
          const double length = std::abs(x - child->x) + std::abs(y - child->y);
          const double wc = length * cap_per_dbu;
          const double wd = length * res_per_dbu * (child->cap + wc / 2) / ns;
          auto shifted = *child;
          shift(shifted, wd, wd);
          auto append = [&](const Choice& previous) {
            auto state = previous ? std::make_shared<State>(*previous)
                                  : std::make_shared<State>();
            state->x = x;
            state->y = y;
            state->weight += shifted.weight;
            state->sum += shifted.sum;
            state->square += shifted.square;
            state->fall_sum += shifted.fall_sum;
            state->fall_square += shifted.fall_square;
            state->registers += child->registers;
            state->buffers += child->buffers;
            state->depth = std::max(state->depth, child->depth);
            state->energy += child->energy;
            state->cap += child->cap + wc;
            state->children.push_back(child);
            expanded.push_back(std::move(state));
          };
          if (combinations.empty()) {
            append(nullptr);
          } else {
            for (const auto& previous : combinations) {
              append(previous);
            }
          }
        }
        combinations = prune(std::move(expanded), beam * 2);
        if (combinations.empty()) {
          throw std::runtime_error(
              "Bounded shared-branch search exhausted its "
              "nonnegative-reference alternatives");
        }
      }
      for (auto& combination : combinations) {
        double lo_x = x, hi_x = x, lo_y = y, hi_y = y, pin_cap = 0;
        for (const auto& child : combination->children) {
          lo_x = std::min(lo_x, double(child->x));
          hi_x = std::max(hi_x, double(child->x));
          lo_y = std::min(lo_y, double(child->y));
          hi_y = std::max(hi_y, double(child->y));
          pin_cap += child->cap;
        }
        combination->cap
            = pin_cap + 1.2 * (hi_x - lo_x + hi_y - lo_y) * cap_per_dbu;
      }
    }
    std::vector<Choice> alternatives;
    const auto slew = measured_slews.count(tag)
                          ? measured_slews.at(tag)
                          : std::array<double, 2>{input_slew, input_slew};
    auto make_chain = [&](const Choice& combination,
                          const std::vector<int>& chain) -> Choice {
      auto state = std::make_shared<State>(*combination);
      auto current_slew = slew;
      for (size_t stage = 0; stage < chain.size(); ++stage) {
        const double load = stage + 1 == chain.size()
                                ? combination->cap
                                : cells[chain[stage + 1]].cap;
        std::array<double, 2> delay, transition;
        double energy;
        if (!characterize(
                chain[stage], load, current_slew, delay, transition, energy)) {
          return nullptr;
        }
        shift(*state, delay[0], delay[1]);
        state->energy += energy;
        current_slew = transition;
      }
      if (!combination->sinks.empty() && leaf_max_slew > 0
          && std::max(current_slew[0], current_slew[1]) > leaf_max_slew) {
        return nullptr;
      }
      state->chain = chain;
      state->cap = cells[chain.front()].cap;
      state->buffers += chain.size();
      state->depth += chain.size();
      state->tag = tag;
      return state->depth <= options_->getClockTreeMaxDepth() ? state : nullptr;
    };
    for (const auto& combination : combinations) {
      for (int index = 0; index < cells.size(); ++index) {
        if (!combination->sinks.empty() && !leaf_master.empty()
            && leaf_master != cells[index].liberty->name()) {
          continue;
        }
        auto state = make_chain(combination, {index});
        if (!state) {
          continue;
        }
        alternatives.push_back(state);
        // Bounded shared prefix stages per subtree. These are physical
        // witnesses, not a continuous interval claiming nonexistent delays.
        for (int count = 0; count < 5; ++count) {
          Choice best;
          for (int prefix = 0; prefix < cells.size(); ++prefix) {
            auto chain = state->chain;
            chain.insert(chain.begin(), prefix);
            auto next = make_chain(combination, chain);
            if (!next) {
              continue;
            }
            if (next->depth <= options_->getClockTreeMaxDepth()) {
              alternatives.push_back(next);
              if (!best || next->energy < best->energy) {
                best = next;
              }
            }
          }
          if (!best) {
            break;
          }
          state = best;
        }
      }
    }
    if (alternatives.empty()) {
      throw std::runtime_error(
          "No electrically feasible cost-aware branch alternative");
    }
    auto retained = prune(std::move(alternatives), beam);
    if (retained.empty()) {
      throw std::runtime_error(
          "Bounded subtree search cannot realize these targets from a "
          "nonnegative clock reference");
    }
    return retained;
  };
  std::vector<unsigned> members(groups.size());
  std::iota(members.begin(), members.end(), 0);
  auto alternatives = build(std::move(members), 0, "root");
  auto root_error = [&](const Choice& s) {
    const auto source = clock_.getSourceLocation();
    const double length
        = std::abs(source.getX() - s->x) + std::abs(source.getY() - s->y);
    const double estimated
        = length * res_per_dbu * (s->cap + .5 * length * cap_per_dbu) / ns;
    const double rise = real("cts_cost_root_rise_ns", estimated);
    const double fall = real("cts_cost_root_fall_ns", estimated);
    return std::max(0.0,
                    (s->square - 2 * rise * s->sum + rise * rise * s->weight
                     + fall_importance
                           * (s->fall_square - 2 * fall * s->fall_sum
                              + fall * fall * s->weight))
                        / ((1 + fall_importance) * s->weight));
  };
  if (pareto_frontier) {
    std::ostringstream report;
    report << "energy_fj_per_register,target_rms_ps,buffers,depth\n";
    for (const auto& state : alternatives) {
      report << state->energy / state->registers << ','
             << std::sqrt(root_error(state)) * 1000 << ',' << state->buffers
             << ',' << state->depth << '\n';
    }
    if (auto* property = odb::dbStringProperty::find(block, "cts_cost_frontier_csv")) {
      property->setValue(report.str().c_str());
    } else {
      odb::dbStringProperty::create(block, "cts_cost_frontier_csv", report.str().c_str());
    }
  }
  if (energy_budget > 0) {
    alternatives.erase(
        std::remove_if(alternatives.begin(), alternatives.end(),
                       [&](const Choice& state) {
                         return state->energy / state->registers > energy_budget;
                       }),
        alternatives.end());
    if (alternatives.empty()) {
      throw std::runtime_error("No retained clock tree meets estimated energy budget");
    }
  }
  const auto best = *std::min_element(
      alternatives.begin(),
      alternatives.end(),
      [&](const Choice& a, const Choice& b) {
        auto root_score = [&](const Choice& s) {
          return root_error(s) * 1e6 + energy_weight * s->energy / s->registers;
        };
        return root_score(a) < root_score(b);
      });
  unsigned serial = 0, group = 0;
  std::ostringstream witness;
  const double unit = techChar_->getLengthUnit();
  std::function<ClockInst*(const Choice&, bool)> emit;
  emit = [&](const Choice& state, bool root) {
    ClockInst *first = nullptr, *last = nullptr;
    ClockSubNet* output = nullptr;
    for (int index : state->chain) {
      const bool is_root = root && !first;
      const std::string id = is_root ? "0" : "cost_" + std::to_string(++serial);
      const auto location
          = legalizeOneBuffer(Point<double>(state->x / unit, state->y / unit),
                              cells[index].liberty->name());
      commitLoc(location);
      auto& driver = clock_.addClockBuffer("clkbuf_" + id,
                                           cells[index].liberty->name(),
                                           location.getX() * unit,
                                           location.getY() * unit);
      driver.setInputCap(cells[index].cap);
      addTreeLevelBuffer(&driver);
      if (output) {
        output->addInst(driver);
      }
      if (!first) {
        first = &driver;
      }
      last = &driver;
      output = &clock_.addSubNet("clknet_" + id);
      output->addInst(driver);
    }
    if (!state->sinks.empty()) {
      addFirstLevelSinkDriver(last);
      output->setLeafLevel(true);
      for (unsigned i : state->sinks) {
        output->addInst(*sinks[i].inst);
        auto* pin = sinks[i].inst->getDbInputPin();
        auto* prop = odb::dbIntProperty::find(pin, "cts_align_group");
        if (prop) {
          prop->setValue(group);
        } else {
          odb::dbIntProperty::create(pin, "cts_align_group", group);
        }
      }
      ++group;
    } else {
      for (const auto& child : state->children) {
        output->addInst(*emit(child, false));
      }
    }
    if (root) {
      topBufferName_ = first->getName();
    }
    witness << state->tag << ' ' << first->getName() << ' ';
    return first;
  };
  emit(best, true);
  auto* record = odb::dbStringProperty::find(block, "cts_cost_witness");
  if (record) {
    record->setValue(witness.str().c_str());
  } else {
    odb::dbStringProperty::create(
        block, "cts_cost_witness", witness.str().c_str());
  }
  treeBufLevels_ = best->depth;
  logger_->info(
      utl::CTS,
      620,
      "Cost-aware asymmetric tree: {} sinks, {} shared leaves, {} buffers, "
      "depth {}, estimated {:.2f} fJ/cycle and {:.2f} ps target RMS.",
      sinks.size(),
      group,
      best->buffers,
      best->depth,
      best->energy,
      std::sqrt(std::max(
          0.0, (best->square + best->fall_square) / (2 * best->weight)))
          * 1000);
}
}  // namespace cts
