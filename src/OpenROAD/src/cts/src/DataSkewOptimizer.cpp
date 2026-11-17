// SPDX-License-Identifier: BSD-3-Clause
#include "DataSkewOptimizer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "DataSkewResponse.h"
#include "db_sta/dbNetwork.hh"
#include "db_sta/dbSta.hh"
#include "est/EstimateParasitics.h"
#include "odb/db.h"
#include "odb/geom.h"
#include "ortools/linear_solver/linear_solver.h"
#include "rsz/Resizer.hh"
#include "sta/Clock.hh"
#include "sta/FuncExpr.hh"
#include "sta/Graph.hh"
#include "sta/GraphDelayCalc.hh"
#include "sta/Liberty.hh"
#include "sta/MinMax.hh"
#include "sta/Mode.hh"
#include "sta/Path.hh"
#include "sta/PathExpanded.hh"
#include "sta/RiseFallMinMax.hh"
#include "sta/Scene.hh"
#include "sta/Sdc.hh"
#include "sta/SearchPred.hh"
#include "sta/Sequential.hh"
#include "sta/TimingArc.hh"
#include "sta/TimingModel.hh"
#include "sta/TimingRole.hh"
#include "utl/Logger.h"

namespace cts {
namespace {
constexpr double ns = 1e-9;
// 0.01 ps reporting tolerance; always compare to a frozen reference.
constexpr double spread_tolerance_ns = 1e-5;
struct Reference
{
  odb::dbITerm* pin;
  double lo;
  double hi;
};
struct Gate
{
  double weight;
  std::vector<odb::dbITerm*> pins;
  double ceiling = std::numeric_limits<double>::quiet_NaN();
  double ranking_floor = 0;
  std::string name{};
};
struct SourceModel;
struct Metrics
{
  double cost = 0, shift = 0, setup = 0, hold = 0, area = 0, clock_area = 0;
  bool electrical = false, finite = true;
  const SourceModel* source_model = nullptr;
  std::vector<std::pair<double, double>> launches;
  std::vector<double> widths;
  std::vector<std::pair<double, double>> clocks;
  double clock_area_ratio = 1.5;
  std::vector<std::vector<std::pair<double, double>>> envelopes;
};
double arrivalNs(sta::dbSta* sta,
                 const sta::Pin* pin,
                 const sta::RiseFallBoth* rf,
                 const sta::MinMax* mode)
{
  const auto value = sta->arrival(pin, rf, mode);
  if (sta::delayInf(value, sta) || sta::delayIsInitValue(value, mode)) {
    return std::numeric_limits<double>::infinity();
  }
  return sta::delayAsFloat(value) / ns;
}
double property(odb::dbObject* object, const char* name)
{
  auto* prop = odb::dbDoubleProperty::find(object, name);
  if (!prop || !std::isfinite(prop->getValue())) {
    throw std::runtime_error(
        std::string("Missing/nonfinite data-skew property: ") + name);
  }
  return prop->getValue();
}
bool legal(const Metrics& value, const Metrics& baseline)
{
  return value.finite && value.electrical && value.setup >= 0.01
         && value.hold >= 0.01 && value.shift <= 0.04
         && value.area <= 1.10 * baseline.area
         && value.clock_area <= baseline.clock_area_ratio * baseline.clock_area;
}
bool protectsGates(const Metrics& value, const Metrics& baseline)
{
  if (!value.finite || value.widths.size() != baseline.widths.size()) {
    return false;
  }
  for (size_t i = 0; i < value.widths.size(); ++i) {
    if (value.widths[i] > baseline.widths[i] + spread_tolerance_ns) {
      return false;
    }
  }
  return true;
}

odb::dbIntProperty* setInt(odb::dbObject* object, const char* name, int value)
{
  auto* p = odb::dbIntProperty::find(object, name);
  if (p) {
    p->setValue(value);
  } else {
    p = odb::dbIntProperty::create(object, name, value);
  }
  return p;
}
odb::dbDoubleProperty* setDouble(odb::dbObject* object,
                                 const char* name,
                                 double value)
{
  auto* p = odb::dbDoubleProperty::find(object, name);
  if (p) {
    p->setValue(value);
  } else {
    p = odb::dbDoubleProperty::create(object, name, value);
  }
  return p;
}
odb::dbStringProperty* setString(odb::dbObject* object,
                                 const char* name,
                                 const char* value)
{
  auto* p = odb::dbStringProperty::find(object, name);
  if (p) {
    p->setValue(value);
  } else {
    p = odb::dbStringProperty::create(object, name, value);
  }
  return p;
}
struct DelaySite
{
  odb::Point point;
  odb::dbOrientType orient;
};
struct Choice
{
  odb::dbInst* inst;
  const sta::LibertyCell* original;
  const sta::LibertyCell* replacement;
  Metrics measured;
  std::vector<DelaySite> sites{};
  std::vector<odb::Rect> boxes{};
  DelaySite leaf_site{};
  // Non-null only for a clock-pin delay; the register itself is never resized.
  odb::dbITerm* delay_input = nullptr;
  std::string delay_pin_name{};
  // Runtime measured bundles refer only to earlier primitive choices.
  std::vector<size_t> members{};
};
// These internal headers depend on the declarations above and on each other.
// clang-format off
#include "DataSkewSourceModel.h"
#include "DataSkewCandidateCache.h"
// clang-format on

// Trial measurements are real STA observations against the fixed entry circuit.
// Keep legal improving trials available when a surrogate search returns no
// winner.
std::vector<size_t> verifiedSingleChoices(const std::vector<Choice>& choices,
                                          const Metrics& baseline,
                                          const Metrics& reference,
                                          const std::vector<double>& floors)
{
  std::vector<std::tuple<double, double, double, size_t>> ranked;
  for (size_t c = 0; c < choices.size(); ++c) {
    const auto& value = choices[c].measured;
    if (!legal(value, reference) || !protectsGates(value, reference)) {
      continue;
    }
    bool improved = false;
    double worst = 0, movement = 0;
    for (size_t g = 0; g < baseline.widths.size(); ++g) {
      improved |= value.widths[g] < baseline.widths[g] - spread_tolerance_ns;
      worst = std::max(worst, value.widths[g] - floors[g]);
    }
    if (!improved) {
      continue;
    }
    for (size_t r = 0; r < reference.clocks.size(); ++r) {
      movement
          += std::abs(value.clocks[r].first - reference.clocks[r].first)
             + std::abs(value.clocks[r].second - reference.clocks[r].second);
    }
    ranked.emplace_back(worst, value.cost, movement, c);
  }
  std::sort(ranked.begin(), ranked.end());
  std::vector<size_t> result;
  for (const auto& entry : ranked) {
    result.push_back(std::get<3>(entry));
  }
  return result;
}

// Gate relaxation diagnostics are explanations, not an irreducible conflict
// set.
struct JointDiagnostics
{
  int primary = 6, secondary = 6, tertiary = 6, relaxation = 6;
  double worst_ps = 0, total_ps = 0;
  std::vector<double> fractions;
  std::string stage_report;
  int64_t primary_ms = 0, secondary_ms = 0, tertiary_ms = 0;
  std::vector<std::pair<size_t, double>> blocking_gates;
};
const char* solverStatus(int status)
{
  switch (status) {
    case 0:
      return "OPTIMAL";
    case 1:
      return "FEASIBLE_INCUMBENT";
    case 2:
      return "INFEASIBLE_MODEL";
    case 3:
      return "UNBOUNDED_MODEL";
    case 4:
      return "ABNORMAL";
    case 5:
      return "MODEL_INVALID";
    default:
      return "NOT_SOLVED_OR_NO_INCUMBENT";
  }
}
// Responses are measured individually and linearized jointly. Actual STA
// decides acceptance.
std::vector<size_t> chooseJoint(const std::vector<Choice>& choices,
                                const Metrics& baseline,
                                const std::vector<double>& ceilings = {},
                                const Metrics* physical_reference = nullptr,
                                const std::vector<double>& floors = {},
                                const std::vector<double>& weights = {},
                                const std::vector<std::vector<size_t>>& rejected
                                = {},
                                JointDiagnostics* diagnostics = nullptr,
                                int solve_budget_ms = 10000,
                                bool relaxation = false,
                                const std::vector<bool>* allowed_choices
                                = nullptr)
{
  using operations_research::MPSolver;
  using operations_research::MPVariable;
  const size_t ng = baseline.widths.size();
  if ((!ceilings.empty() && ceilings.size() != ng)
      || (!floors.empty() && floors.size() != ng)
      || (!weights.empty() && weights.size() != ng)) {
    throw std::runtime_error("Joint gate dimension mismatch");
  }
  std::unique_ptr<MPSolver> solver(
      MPSolver::CreateSolver(relaxation ? "GLOP" : "SCIP"));
  if (!solver) {
    throw std::runtime_error("Native constrained sizing requires SCIP");
  }
  if (solve_budget_ms < 100 || solve_budget_ms > 300000) {
    throw std::runtime_error("Solve budget must be 100..300000 ms");
  }
  solver->set_time_limit(solve_budget_ms);
  // Objective-dependent reductions can discard alternatives needed by the
  // next objective. Keep the feasible set intact across all three stages.
  if (!relaxation
      && !solver->SetSolverSpecificParametersAsString(
          "misc/allowstrongdualreds = FALSE\nmisc/allowweakdualreds = FALSE")) {
    throw std::runtime_error(
        "Cannot disable objective-dependent SCIP reductions");
  }
  operations_research::MPSolverParameters solve_parameters;
  solve_parameters.SetDoubleParam(
      operations_research::MPSolverParameters::RELATIVE_MIP_GAP, 0.0);
  if (!relaxation && !solver->SetNumThreads(1).ok()) {
    throw std::runtime_error("Cannot configure native MILP thread limit");
  }
  JointDiagnostics local;
  auto& diag = diagnostics ? *diagnostics : local;
  diag = JointDiagnostics{};
  diag.stage_report
      = "configured_relative_mip_gap,0\nobjective_dependent_dual_reductions,"
        "disabled\n";
  const double inf = solver->infinity();
  if (allowed_choices && allowed_choices->size() != choices.size()) {
    throw std::runtime_error("Allowed choice dimension mismatch");
  }
  std::vector<MPVariable*> vars;
  std::map<std::string, operations_research::MPConstraint*> one_choice;
  for (const auto& choice : choices) {
    auto* x
        = relaxation ? solver->MakeNumVar(0, 1, "") : solver->MakeBoolVar("");
    vars.push_back(x);
    if (allowed_choices && !(*allowed_choices)[vars.size() - 1]) {
      x->SetBounds(0, 0);
    }
    std::vector<size_t> actors = choice.members;
    if (actors.empty()) {
      actors.push_back(vars.size() - 1);
    }
    for (size_t actor : actors) {
      if (actor >= choices.size() || !choices[actor].members.empty()) {
        throw std::runtime_error("Invalid bundle primitive");
      }
      auto*& row = one_choice[choices[actor].inst->getName()];
      if (!row) {
        row = solver->MakeRowConstraint(0, 1);
      }
      row->SetCoefficient(x, 1);
    }
  }
  for (const auto& combination : rejected) {
    // Exclude exactly the failed vector; a superset may compensate its effects.
    std::vector<bool> included(choices.size(), false);
    for (size_t c : combination) {
      if (c >= choices.size()) {
        throw std::runtime_error("Invalid feedback choice");
      }
      included[c] = true;
    }
    auto* row = solver->MakeRowConstraint(-inf, double(combination.size()) - 1);
    for (size_t c = 0; c < choices.size(); ++c) {
      row->SetCoefficient(vars[c], included[c] ? 1 : -1);
    }
  }
  for (size_t a = 0; a < choices.size(); ++a) {
    for (size_t b = a + 1; b < choices.size(); ++b) {
      if (choices[a].inst == choices[b].inst) {
        continue;
      }
      bool conflict = false;
      for (const auto& x : choices[a].boxes) {
        for (const auto& y : choices[b].boxes) {
          conflict |= x.overlaps(y);
        }
      }
      if (conflict) {
        auto* row = solver->MakeRowConstraint(0, 1);
        row->SetCoefficient(vars[a], 1);
        row->SetCoefficient(vars[b], 1);
      }
    }
  }
  auto* worst = solver->MakeNumVar(0, inf, "worst_source_floor_excess_ps");
  std::vector<std::pair<const MPVariable*, double>> baseline_hint;
  for (auto* x : vars) {
    baseline_hint.emplace_back(x, 0);
  }
  double baseline_worst = 0;
  std::vector<std::pair<MPVariable*, MPVariable*>> windows;
  std::vector<operations_research::MPConstraint*> gate_caps;
  for (size_t g = 0; g < ng; ++g) {
    const double floor = floors.empty() ? 0 : floors[g];
    if (!std::isfinite(floor) || floor < 0
        || (!weights.empty()
            && (!std::isfinite(weights[g]) || weights[g] <= 0))) {
      throw std::runtime_error("Invalid gate ranking data");
    }
    auto* upper = solver->MakeNumVar(-inf, inf, "");
    auto* lower = solver->MakeNumVar(-inf, inf, "");
    windows.emplace_back(upper, lower);
    const double anchor = responseAt(baseline, g, 0).first;
    double be = std::numeric_limits<double>::infinity(), bl = -be;
    for (size_t p = 0; p < responseSize(baseline, g); ++p) {
      const auto e = responseAt(baseline, g, p);
      be = std::min(be, e.first);
      bl = std::max(bl, e.second);
    }
    baseline_hint.emplace_back(upper, (bl - anchor) * 1000);
    baseline_hint.emplace_back(lower, (be - anchor) * 1000);
    baseline_worst = std::max(baseline_worst, (bl - be - floor) * 1000);
    auto* cap = solver->MakeRowConstraint(
        0,
        ((ceilings.empty() ? baseline.widths[g] : ceilings[g])
         + spread_tolerance_ns)
            * 1000);
    cap->SetCoefficient(upper, 1);
    cap->SetCoefficient(lower, -1);
    gate_caps.push_back(cap);
    for (size_t p = 0; p < responseSize(baseline, g); ++p) {
      const auto [early, late] = responseAt(baseline, g, p);
      auto* lo = solver->MakeRowConstraint(-inf, (early - anchor) * 1000);
      auto* hi = solver->MakeRowConstraint((late - anchor) * 1000, inf);
      lo->SetCoefficient(lower, 1);
      hi->SetCoefficient(upper, 1);
      for (size_t c = 0; c < choices.size(); ++c) {
        const auto [ce, cl] = responseAt(choices[c].measured, g, p);
        const double common
            = responseAt(choices[c].measured, g, 0).first - anchor;
        // Subtract the same affine clock/data movement from every input.
        // U-L is invariant; this removes irrelevant common-shift coefficients.
        lo->SetCoefficient(vars[c], -(ce - early - common) * 1000);
        hi->SetCoefficient(vars[c], -(cl - late - common) * 1000);
      }
    }
    // Floors are ranking offsets from a frozen model, never physical
    // constraints.
    auto* row = solver->MakeRowConstraint(-inf, floor * 1000);
    row->SetCoefficient(upper, 1);
    row->SetCoefficient(lower, -1);
    row->SetCoefficient(worst, -1);
  }
  const auto& physical = physical_reference ? *physical_reference : baseline;
  auto* area
      = solver->MakeRowConstraint(-inf, 1.10 * physical.area - baseline.area);
  auto* clock_area = solver->MakeRowConstraint(
      -inf,
      physical.clock_area_ratio * physical.clock_area - baseline.clock_area);
  for (size_t c = 0; c < choices.size(); ++c) {
    area->SetCoefficient(vars[c], choices[c].measured.area - baseline.area);
    clock_area->SetCoefficient(
        vars[c], choices[c].measured.clock_area - baseline.clock_area);
  }
  std::vector<MPVariable*> movement;
  MPVariable* common_clock = nullptr;
  std::vector<double> common_response(choices.size(), 0);
  if (!baseline.clocks.empty()) {
    common_clock = solver->MakeNumVar(-inf, inf, "common_clock_shift_ps");
    baseline_hint.emplace_back(common_clock, 0);
    auto* relation = solver->MakeRowConstraint(0, 0);
    relation->SetCoefficient(common_clock, 1);
    for (size_t c = 0; c < choices.size(); ++c) {
      common_response[c] = (choices[c].measured.clocks.front().first
                            - baseline.clocks.front().first)
                           * 1000;
      relation->SetCoefficient(vars[c], -common_response[c]);
    }
  }
  if (physical.clocks.size() != baseline.clocks.size()) {
    throw std::runtime_error("Clock reference dimension mismatch");
  }
  for (size_t r = 0; r < baseline.clocks.size(); ++r) {
    for (int edge = 0; edge < 2; ++edge) {
      const double base
          = edge ? baseline.clocks[r].second : baseline.clocks[r].first;
      const double ref
          = edge ? physical.clocks[r].second : physical.clocks[r].first;
      auto* guard = solver->MakeRowConstraint((ref - .04 - base) * 1000,
                                              (ref + .04 - base) * 1000);
      auto* absolute = solver->MakeNumVar(0, inf, "");
      movement.push_back(absolute);
      baseline_hint.emplace_back(absolute, std::abs(base - ref) * 1000);
      auto* pos = solver->MakeRowConstraint(-inf, -(base - ref) * 1000);
      auto* neg = solver->MakeRowConstraint(-inf, (base - ref) * 1000);
      pos->SetCoefficient(absolute, -1);
      neg->SetCoefficient(absolute, -1);
      guard->SetCoefficient(common_clock, 1);
      pos->SetCoefficient(common_clock, 1);
      neg->SetCoefficient(common_clock, -1);
      for (size_t c = 0; c < choices.size(); ++c) {
        const auto& m = choices[c].measured.clocks[r];
        const double delta
            = ((edge ? m.second : m.first) - base) * 1000 - common_response[c];
        guard->SetCoefficient(vars[c], delta);
        pos->SetCoefficient(vars[c], delta);
        neg->SetCoefficient(vars[c], -delta);
      }
    }
  }
  baseline_hint.emplace_back(worst, baseline_worst);
  bool zero_feasible = baseline.area <= 1.10 * physical.area
                       && baseline.clock_area <= physical.clock_area_ratio
                                                     * physical.clock_area;
  for (size_t g = 0; g < ng; ++g) {
    zero_feasible &= baseline.widths[g]
                     <= (ceilings.empty() ? baseline.widths[g] : ceilings[g])
                            + spread_tolerance_ns;
  }
  for (size_t r = 0; r < baseline.clocks.size(); ++r) {
    zero_feasible
        &= std::abs(baseline.clocks[r].first - physical.clocks[r].first) <= .04
           && std::abs(baseline.clocks[r].second - physical.clocks[r].second)
                  <= .04;
  }
  for (const auto& denied : rejected) {
    zero_feasible &= !denied.empty();
  }
  if (zero_feasible && !relaxation) {
    solver->SetHint(baseline_hint);
  }
  if (!relaxation) {
    const auto seed_floors
        = floors.empty() ? std::vector<double>(ng, 0) : floors;
    const auto seeds
        = verifiedSingleChoices(choices, baseline, physical, seed_floors);
    for (size_t c : seeds) {
      // A complete feasible witness prevents a zero-only warm start from
      // dominating a large, numerically degenerate response model.
      std::vector<double> witness(solver->NumVariables(), 0);
      witness[vars[c]->index()] = 1;
      double seed_worst = 0;
      for (size_t g = 0; g < ng; ++g) {
        const double anchor = responseAt(baseline, g, 0).first;
        const double common
            = responseAt(choices[c].measured, g, 0).first - anchor;
        double low = std::numeric_limits<double>::infinity(), high = -low;
        for (size_t p = 0; p < responseSize(baseline, g); ++p) {
          const auto [early, late] = responseAt(choices[c].measured, g, p);
          low = std::min(low, early);
          high = std::max(high, late);
        }
        witness[windows[g].first->index()] = (high - anchor - common) * 1000;
        witness[windows[g].second->index()] = (low - anchor - common) * 1000;
        seed_worst = std::max(seed_worst, (high - low - seed_floors[g]) * 1000);
      }
      witness[worst->index()] = seed_worst;
      if (common_clock) {
        witness[common_clock->index()] = common_response[c];
      }
      for (size_t r = 0; r < baseline.clocks.size(); ++r) {
        witness[movement[r * 2]->index()]
            = std::abs(choices[c].measured.clocks[r].first
                       - physical.clocks[r].first)
              * 1000;
        witness[movement[r * 2 + 1]->index()]
            = std::abs(choices[c].measured.clocks[r].second
                       - physical.clocks[r].second)
              * 1000;
      }
      bool feasible = true;
      for (auto* row : solver->constraints()) {
        double value = 0;
        for (const auto& [variable, coefficient] : row->terms()) {
          value += coefficient * witness[variable->index()];
        }
        feasible &= value >= row->lb() - 1e-6 && value <= row->ub() + 1e-6;
      }
      if (!feasible) {
        continue;
      }
      std::vector<std::pair<const MPVariable*, double>> hint;
      for (auto* variable : solver->variables()) {
        hint.emplace_back(variable, witness[variable->index()]);
      }
      solver->SetHint(hint);
      diag.stage_report
          += "measured_feasible_warm_start,choice," + std::to_string(c) + "\n";
      break;
    }
  }
  const auto warm_incumbent = [&]() {
    std::vector<std::pair<const MPVariable*, double>> hint;
    for (auto* variable : solver->variables()) {
      hint.emplace_back(variable, variable->solution_value());
    }
    return hint;
  };
  auto* objective = solver->MutableObjective();
  objective->SetCoefficient(worst, 1);
  objective->SetMinimization();
  const auto primary_start = solver->wall_time();
  auto status = solver->Solve(solve_parameters);
  diag.primary = status;
  diag.primary_ms = solver->wall_time() - primary_start;
  if (status != MPSolver::OPTIMAL && status != MPSolver::FEASIBLE) {
    if (status == MPSolver::INFEASIBLE) {
      // Relax only gate ceilings for diagnosis; all physical rows stay
      // unchanged.
      objective->Clear();
      std::vector<MPVariable*> slack;
      for (auto* cap : gate_caps) {
        auto* s = solver->MakeNumVar(0, inf, "");
        cap->SetCoefficient(s, -1);
        objective->SetCoefficient(s, 1);
        slack.push_back(s);
      }
      diag.relaxation = solver->Solve(solve_parameters);
      if (diag.relaxation == MPSolver::OPTIMAL
          || diag.relaxation == MPSolver::FEASIBLE) {
        for (size_t g = 0; g < ng; ++g) {
          if (slack[g]->solution_value() > .01) {
            diag.blocking_gates.emplace_back(g, slack[g]->solution_value());
          }
        }
        std::sort(
            diag.blocking_gates.begin(),
            diag.blocking_gates.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
      }
    }
    return {};
  }
  std::vector<size_t> selected;
  const auto capture = [&]() {
    selected.clear();
    diag.fractions.clear();
    for (size_t c = 0; c < vars.size(); ++c) {
      diag.fractions.push_back(vars[c]->solution_value());
      if (!relaxation && vars[c]->solution_value() > .5) {
        selected.push_back(c);
      }
    }
  };
  const auto report_stage = [&](const char* phase, double objective_value) {
    double weighted = 0, excess = 0;
    for (size_t g = 0; g < ng; ++g) {
      double low = std::numeric_limits<double>::infinity(), high = -low;
      for (size_t p = 0; p < responseSize(baseline, g); ++p) {
        auto [early, late] = responseAt(baseline, g, p);
        for (size_t c : selected) {
          early += responseAt(choices[c].measured, g, p).first
                   - responseAt(baseline, g, p).first;
          late += responseAt(choices[c].measured, g, p).second
                  - responseAt(baseline, g, p).second;
        }
        low = std::min(low, early);
        high = std::max(high, late);
      }
      weighted += (weights.empty() ? 1 : weights[g]) * (high - low) * 1000;
      excess = std::max(excess,
                        (high - low - (floors.empty() ? 0 : floors[g])) * 1000);
    }
    std::ostringstream record;
    record << std::setprecision(17) << "phase," << phase << ",selected,"
           << selected.size() << ",objective," << objective_value
           << ",best_bound," << objective->BestBound() << ",binary_worst_ps,"
           << excess << ",binary_weighted_ps," << weighted << ",verified,"
           << solver->VerifySolution(1e-6, false) << '\n';
    diag.stage_report += record.str();
    return std::pair<double, double>{excess, weighted};
  };
  capture();
  report_stage("primary", objective->Value());
  auto primary_hint = warm_incumbent();
  diag.worst_ps = worst->solution_value();
  worst->SetBounds(0, diag.worst_ps + .001);
  if (!relaxation) {
    solver->SetHint(primary_hint);
  }
  objective->Clear();
  for (size_t g = 0; g < ng; ++g) {
    const double w = weights.empty() ? 1 : weights[g];
    objective->SetCoefficient(windows[g].first, w);
    objective->SetCoefficient(windows[g].second, -w);
  }
  double baseline_weighted_ps = 0;
  for (size_t g = 0; g < ng; ++g) {
    baseline_weighted_ps
        += (weights.empty() ? 1 : weights[g]) * baseline.widths[g] * 1000;
  }
  objective->SetOffset(-baseline_weighted_ps);
  const auto secondary_start = solver->wall_time();
  diag.secondary = solver->Solve(solve_parameters);
  diag.secondary_ms = solver->wall_time() - secondary_start;
  if (diag.secondary != MPSolver::OPTIMAL
      && diag.secondary != MPSolver::FEASIBLE) {
    return selected;
  }
  capture();
  const auto secondary_score = report_stage("secondary", objective->Value());
  const auto secondary_selection = selected;
  auto secondary_hint = warm_incumbent();
  diag.total_ps = objective->Value() + baseline_weighted_ps;
  auto* total = solver->MakeRowConstraint(-inf, diag.total_ps + .001);
  for (size_t g = 0; g < ng; ++g) {
    const double w = weights.empty() ? 1 : weights[g];
    total->SetCoefficient(windows[g].first, w);
    total->SetCoefficient(windows[g].second, -w);
  }
  objective->Clear();
  if (movement.empty()) {
    for (auto* x : vars) {
      objective->SetCoefficient(x, 1);
    }
  } else {
    for (auto* x : movement) {
      objective->SetCoefficient(x, 1);
    }
  }
  if (!relaxation) {
    solver->SetHint(secondary_hint);
  }
  const auto tertiary_start = solver->wall_time();
  diag.tertiary = solver->Solve(solve_parameters);
  diag.tertiary_ms = solver->wall_time() - tertiary_start;
  if (diag.tertiary == MPSolver::OPTIMAL
      || diag.tertiary == MPSolver::FEASIBLE) {
    capture();
    const auto tertiary_score = report_stage("tertiary", objective->Value());
    // Numerical auxiliaries must not erase a better, already feasible
    // selection.
    if (tertiary_score.first > diag.worst_ps + .001 + 1e-6
        || tertiary_score.second > secondary_score.second + .001 + 1e-6) {
      selected = secondary_selection;
      diag.fractions.assign(choices.size(), 0);
      for (size_t c : selected) {
        diag.fractions[c] = 1;
      }
      diag.stage_report
          += "retained_secondary,tertiary_binary_score_regressed\n";
    }
  }
  return selected;
}
bool placementLegal(odb::dbInst* inst)
{
  if (!inst->isPlaced()) {
    return false;
  }
  const auto box = inst->getBBox()->getBox();
  if (!inst->getBlock()->getCoreArea().contains(box)) {
    return false;
  }
  for (auto* other : inst->getBlock()->getInsts()) {
    if (other != inst && other->isPlaced()
        && box.overlaps(other->getBBox()->getBox())) {
      return false;
    }
  }
  return true;
}
odb::dbITerm* bufferInput(odb::dbInst* inst)
{
  odb::dbITerm* input = nullptr;
  for (auto* pin : inst->getITerms()) {
    if (pin->isInputSignal()) {
      if (input != nullptr) {
        throw std::runtime_error("Multiple buffer inputs unsupported");
      }
      input = pin;
    }
  }
  if (!input || !input->getNet()) {
    throw std::runtime_error("Disconnected clock-buffer input");
  }
  return input;
}
std::optional<DelaySite> freeDelaySite(odb::dbBlock* block,
                                       odb::dbMaster* master,
                                       const odb::Point& target)
{
  std::vector<odb::dbRow*> rows;
  for (auto* row : block->getRows()) {
    if (row->getDirection() == odb::dbRowDir::HORIZONTAL
        && row->getSite()->getHeight() == master->getHeight()) {
      rows.push_back(row);
    }
  }
  std::stable_sort(rows.begin(), rows.end(), [&](auto* a, auto* b) {
    return std::abs(a->getOrigin().y() - target.y())
           < std::abs(b->getOrigin().y() - target.y());
  });
  for (auto* row : rows) {
    const auto origin = row->getOrigin();
    const int step = row->getSpacing();
    if (step <= 0) {
      continue;
    }
    const int center = (target.x() - origin.x()) / step;
    for (int offset = 0; offset <= 80; ++offset) {
      for (int sign : {-1, 1}) {
        const int site = center + sign * offset;
        if (site < 0 || site >= row->getSiteCount()) {
          continue;
        }
        const int x = origin.x() + site * step;
        const odb::Rect box(x,
                            origin.y(),
                            x + master->getWidth(),
                            origin.y() + master->getHeight());
        if (!row->getBBox().contains(box)
            || !block->getCoreArea().contains(box)) {
          continue;
        }
        bool free = true;
        for (auto* inst : block->getInsts()) {
          if (inst->isPlaced() && box.overlaps(inst->getBBox()->getBox())) {
            free = false;
            break;
          }
        }
        if (free) {
          return DelaySite{odb::Point(x, origin.y()), row->getOrient()};
        }
      }
    }
  }
  return std::nullopt;
}
struct ClockEdit
{
  odb::dbInst* leaf = nullptr;
  const sta::LibertyCell* original = nullptr;
  odb::dbITerm* input = nullptr;
  odb::dbNet* parent = nullptr;
  DelaySite original_site{};
  std::vector<odb::dbInst*> buffers{};
  std::vector<odb::dbNet*> nets{};
};
void undoClockEdit(ClockEdit& edit,
                   sta::dbNetwork* network,
                   rsz::Resizer* resizer)
{
  if (edit.input) {
    edit.input->disconnect();
    edit.input->connect(edit.parent);
    for (auto* buffer : edit.buffers) {
      for (auto* pin : buffer->getITerms()) {
        pin->disconnect();
      }
      odb::dbInst::destroy(buffer);
    }
    for (auto* net : edit.nets) {
      if (!net->getITerms().empty() || !net->getBTerms().empty()) {
        throw std::runtime_error("Delay rollback left attached net");
      }
      odb::dbNet::destroy(net);
    }
    edit.buffers.clear();
    edit.nets.clear();
  }
  if (edit.original
      && !resizer->replaceCell(
          network->dbToSta(edit.leaf), edit.original, false)) {
    throw std::runtime_error("Clock sizing rollback failed");
  }
  if (edit.original) {
    edit.leaf->setOrient(edit.original_site.orient);
    edit.leaf->setLocation(edit.original_site.point.x(),
                           edit.original_site.point.y());
  }
}
void addDelayBuffer(ClockEdit& edit,
                    odb::dbMaster* master,
                    const DelaySite& site,
                    rsz::Resizer* resizer)
{
  // Use the ODB clock-only operation so trial cells do not pollute resizer
  // insertion counters. STA observes database changes through dbNetwork.
  auto* buffer = edit.input->getNet()->insertBufferBeforeLoad(
      edit.input, master, &site.point, "cts_data_delay", "cts_data_delay_net");
  if (!buffer) {
    throw std::runtime_error("Clock delay insertion failed");
  }
  edit.buffers.push_back(buffer);
  buffer->setOrient(site.orient);
  buffer->setLocation(site.point.x(), site.point.y());
  buffer->setPlacementStatus(odb::dbPlacementStatus::PLACED);
  buffer->setSourceType(odb::dbSourceType::TIMING);
  for (auto* pin : buffer->getITerms()) {
    const auto type = pin->getMTerm()->getSigType();
    if (type == odb::dbSigType::POWER || type == odb::dbSigType::GROUND) {
      auto* supply = edit.leaf->findITerm(pin->getMTerm()->getConstName());
      if (supply && supply->getNet()) {
        if (pin->getNet()) {
          pin->disconnect();
        }
        pin->connect(supply->getNet());
      }
    }
  }
  auto* net = edit.input->getNet();
  if (net == edit.parent) {
    throw std::runtime_error("Delay insertion did not isolate clock input");
  }
  net->setSigType(odb::dbSigType::CLOCK);
  edit.nets.push_back(net);
  if (!placementLegal(buffer)) {
    throw std::runtime_error("Inserted clock delay cell has illegal placement");
  }
}
Metrics measure(odb::dbBlock* block,
                sta::dbNetwork* network,
                sta::dbSta* sta,
                const std::vector<Reference>& refs,
                const std::vector<Gate>& gates,
                const SourceModel* source_model = nullptr)
{
  sta->updateTiming(false);
  Metrics m;
  m.source_model = source_model;
  if (source_model) {
    m.launches = source_model->measure(sta);
  }
  m.setup = sta::delayAsFloat(sta->worstSlack(sta::MinMax::max())) / ns;
  m.hold = sta::delayAsFloat(sta->worstSlack(sta::MinMax::min())) / ns;
  m.finite = std::isfinite(m.setup) && std::isfinite(m.hold);
  for (const auto& ref : refs) {
    auto* pin = network->dbToSta(ref.pin);
    double lo
        = arrivalNs(sta, pin, sta::RiseFallBoth::rise(), sta::MinMax::min());
    double hi
        = arrivalNs(sta, pin, sta::RiseFallBoth::rise(), sta::MinMax::max());
    m.finite &= std::isfinite(lo) && std::isfinite(hi);
    m.shift = std::max({m.shift, std::abs(lo - ref.lo), std::abs(hi - ref.hi)});
    m.clocks.emplace_back(lo, hi);
  }
  for (const auto& gate : gates) {
    m.envelopes.emplace_back();
    double lo = std::numeric_limits<double>::infinity(), hi = -lo;
    for (auto* term : gate.pins) {
      auto* pin = network->dbToSta(term);
      for (auto* rf : {sta::RiseFallBoth::rise(), sta::RiseFallBoth::fall()}) {
        double amin = arrivalNs(sta, pin, rf, sta::MinMax::min());
        double amax = arrivalNs(sta, pin, rf, sta::MinMax::max());
        m.finite &= std::isfinite(amin) && std::isfinite(amax);
        m.envelopes.back().emplace_back(amin, amax);
        lo = std::min(lo, amin);
        hi = std::max(hi, amax);
      }
    }
    m.widths.push_back(hi - lo);
    m.cost += gate.weight * (hi - lo);
  }
  m.finite &= std::isfinite(m.cost);
  const double units = block->getDbUnitsPerMicron();
  for (auto* inst : block->getInsts()) {
    auto* master = inst->getMaster();
    double area
        = double(master->getWidth()) * master->getHeight() / (units * units);
    m.area += area;
    for (auto* pin : inst->getITerms()) {
      if (pin->isOutputSignal() && pin->getNet()
          && pin->getNet()->getSigType() == odb::dbSigType::CLOCK) {
        m.clock_area += area;
        break;
      }
    }
  }
  sta->checkFanoutPreamble();
  m.electrical
      = sta->maxSlewViolationCount() == 0
        && sta->maxCapacitanceViolationCount() == 0
        && sta->fanoutViolationCount(sta::MinMax::max(), {sta->cmdMode()}) == 0;
  return m;
}
}  // namespace

#include "DataSkewConstructionImpl.h"

void sizeDataSkewLeaves(odb::dbBlock* block,
                        sta::dbNetwork* network,
                        sta::dbSta* sta,
                        rsz::Resizer* resizer,
                        est::EstimateParasitics* parasitics,
                        utl::Logger* logger,
                        const std::vector<std::string>& names)
{
  auto* enabled
      = odb::dbIntProperty::find(block, "cts_data_skew_native_sizing");
  if (!enabled || enabled->getValue() == 0) {
    return;  // Preserve the default CTS flow.
  }
  if (enabled->getValue() != 1) {
    throw std::runtime_error("Native sizing enable must be 0/1");
  }
  int budget = 64;
  if (auto* value
      = odb::dbIntProperty::find(block, "cts_data_skew_native_trial_budget")) {
    budget = value->getValue();
  }
  if (budget < 0 || budget > 4096) {
    throw std::runtime_error("Invalid native sizing trial budget");
  }
  if (sta->scenes().size() != 1 || sta->modes().size() != 1) {
    throw std::runtime_error(
        "Native sizing currently requires one scene and mode");
  }
  const auto& clocks = sta->cmdMode()->sdc()->clocks();
  if (clocks.size() != 1 || clocks[0]->isGenerated()
      || clocks[0]->isVirtual()) {
    throw std::runtime_error(
        "Native data-skew sizing requires one physical clock");
  }
  std::vector<Reference> refs;
  std::vector<Gate> gates;
  std::map<std::string, odb::dbInst*> drivers;
  for (auto* inst : block->getInsts()) {
    auto* cell = network->libertyCell(network->dbToSta(inst));
    if (!cell) {
      auto* master = inst->getMaster();
      bool no_signal_ports = true;
      for (auto* port : master->getMTerms()) {
        no_signal_ports &= port->getSigType() == odb::dbSigType::POWER
                           || port->getSigType() == odb::dbSigType::GROUND;
      }
      const bool physical_type
          = master->getType() == odb::dbMasterType::CORE_WELLTAP
            || master->isFiller() || master->isEndCap();
      if (physical_type && no_signal_ports
          && !odb::dbDoubleProperty::find(inst, "cts_data_skew_gate_weight")) {
        continue;
      }
      throw std::runtime_error(std::string("Unmapped nonphysical cell: ")
                               + inst->getName());
    }
    if (cell->isClockGate()) {
      throw std::runtime_error("Clock gating unsupported");
    }
    if (cell->isSequential()) {
      if (cell->sequentials().empty()) {
        throw std::runtime_error("Unsupported sequential cell");
      }
      for (const auto& seq : cell->sequentials()) {
        if (seq.isLatch() || seq.clear() || seq.preset() || !seq.clock()
            || seq.clock()->op() != sta::FuncExpr::Op::port) {
          throw std::runtime_error(
              "Only positive-edge synchronous registers supported");
        }
      }
      int clock_count = 0;
      for (auto* term : inst->getITerms()) {
        auto* port = network->libertyPort(network->dbToSta(term));
        if (!port || !port->isClock()) {
          continue;
        }
        ++clock_count;
        auto* net = term->getNet();
        if (!net || net->getSigType() != odb::dbSigType::CLOCK) {
          throw std::runtime_error("Disconnected/non-clock register clock");
        }
        double lo = property(term, "cts_data_skew_baseline_min_ns");
        double hi = property(term, "cts_data_skew_baseline_max_ns");
        if (lo > hi) {
          throw std::runtime_error("Invalid reference clock range");
        }
        refs.push_back({term, lo, hi});
        // Respect do-not-touch protections on sinks, drivers and clock nets.
        if (inst->isDoNotTouch() || net->isDoNotTouch()) {
          continue;
        }
        odb::dbInst* driver = nullptr;
        for (auto* pin : net->getITerms()) {
          if (pin->isOutputSignal()) {
            if (driver) {
              throw std::runtime_error("Multiple clock drivers unsupported");
            }
            driver = pin->getInst();
          }
        }
        if (!driver) {
          throw std::runtime_error("Missing leaf clock driver");
        }
        bool protected_net = driver->isDoNotTouch();
        for (auto* pin : driver->getITerms()) {
          if (pin->getNet()) {
            protected_net |= pin->getNet()->isDoNotTouch();
          }
        }
        for (auto* pin : net->getITerms()) {
          protected_net |= pin->getInst()->isDoNotTouch();
        }
        if (!protected_net) {
          drivers[driver->getName()] = driver;
        }
      }
      if (clock_count != 1) {
        throw std::runtime_error("One clock pin per register required");
      }
    }
    if (auto* mark
        = odb::dbDoubleProperty::find(inst, "cts_data_skew_gate_weight")) {
      if (cell->isSequential() || !std::isfinite(mark->getValue())
          || mark->getValue() <= 0) {
        throw std::runtime_error("Invalid marked meeting gate");
      }
      Gate gate{mark->getValue(), {}};
      gate.name = inst->getName();
      if (odb::dbDoubleProperty::find(inst, "cts_data_skew_gate_floor_ns")) {
        gate.ranking_floor = property(inst, "cts_data_skew_gate_floor_ns");
        if (gate.ranking_floor < 0) {
          throw std::runtime_error("Negative ranking floor");
        }
      }
      if (odb::dbDoubleProperty::find(inst,
                                      "cts_data_skew_gate_reference_ns")) {
        gate.ceiling = property(inst, "cts_data_skew_gate_reference_ns");
        if (gate.ceiling < 0) {
          throw std::runtime_error("Negative gate reference spread");
        }
      }
      for (auto* pin : inst->getITerms()) {
        auto* selected
            = odb::dbIntProperty::find(pin, "cts_data_skew_gate_input");
        if (selected && selected->getValue() != 1) {
          throw std::runtime_error("Invalid marked gate-input property");
        }
        if (selected && pin->isInputSignal() && pin->getNet()) {
          gate.pins.push_back(pin);
        }
      }
      if (gate.pins.size() < 2) {
        throw std::runtime_error("Marked gate needs multiple inputs");
      }
      gates.push_back(std::move(gate));
    }
  }
  if (refs.empty() || gates.empty()) {
    throw std::runtime_error(
        "Native sizing requires complete clock references and marked gates");
  }
  for (auto* inst : block->getInsts()) {
    auto* cell = network->libertyCell(network->dbToSta(inst));
    if (!cell || !cell->isBuffer() || cell->isInverter()
        || inst->isDoNotTouch()) {
      continue;
    }
    bool clock_output = false, protected_net = false;
    for (auto* pin : inst->getITerms()) {
      if (auto* net = pin->getNet()) {
        protected_net |= net->isDoNotTouch();
        clock_output |= pin->isOutputSignal()
                        && net->getSigType() == odb::dbSigType::CLOCK;
        for (auto* load : net->getITerms()) {
          protected_net |= load->getInst()->isDoNotTouch();
        }
      }
    }
    if (clock_output && !protected_net) {
      drivers[inst->getName()] = inst;
    }
  }
  for (const auto& [name, inst] : drivers) {
    auto* cell = network->libertyCell(network->dbToSta(inst));
    if (!cell || !cell->isBuffer() || cell->isInverter()) {
      throw std::runtime_error("Unsupported leaf driver");
    }
  }
  std::vector<const sta::LibertyCell*> cells;
  for (const auto& name : names) {
    auto* cell = network->findLibertyCell(name.c_str());
    if (!cell || !cell->isBuffer() || cell->isInverter()) {
      throw std::runtime_error("Noninverting clock buffers required");
    }
    cells.push_back(cell);
  }
  struct IncrementalRestore
  {
    est::EstimateParasitics* parasitics;
    bool previous;
    ~IncrementalRestore()
    {
      parasitics->setIncrementalParasiticsEnabled(previous);
    }
  } incremental_restore{parasitics,
                        parasitics->isIncrementalParasiticsEnabled()};
  bool initialized = false;
  const auto full_estimate = [&]() {
    parasitics->setIncrementalParasiticsEnabled(false);
    parasitics->estimateParasitics(est::ParasiticsSrc::kPlacement);
    parasitics->setIncrementalParasiticsEnabled(true);
    initialized = true;
  };
  const auto estimate = [&]() {
    if (!initialized) {
      full_estimate();
      return;
    }
    // Only clock buffers/clock nets change; all data geometry stays frozen.
    // Callback invalidations are retained; explicitly refresh every clock net.
    for (auto* net : block->getNets()) {
      if (net->getSigType() == odb::dbSigType::CLOCK) {
        parasitics->parasiticsInvalid(net);
      }
    }
    parasitics->updateParasitics();
  };
  estimate();
  bool source_enabled = false;
  if (auto* p = odb::dbIntProperty::find(block, "cts_data_skew_source_model")) {
    if (p->getValue() != 0 && p->getValue() != 1) {
      throw std::runtime_error("Source model flag must be 0/1");
    }
    source_enabled = p->getValue() == 1;
  }
  std::unique_ptr<SourceModel> source_model;
  if (source_enabled) {
    source_model = std::make_unique<SourceModel>(block, network, sta, gates);
  }
  Metrics baseline
      = measure(block, network, sta, refs, gates, source_model.get());
  setDouble(block,
            "cts_data_skew_source_audit_error_ps",
            source_model ? source_model->audit_error * 1000 : 0);
  if (auto* output = odb::dbStringProperty::find(block, "cts_align_export")) {
    if (!source_model) {
      throw std::runtime_error("Alignment export requires the source model");
    }
    exportAlignmentModel(block,
                         network,
                         sta,
                         refs,
                         gates,
                         *source_model,
                         baseline,
                         output->getValue());
    logger->info(
        utl::CTS, 321, "Exported fresh source-preserving alignment model.");
    return;  // Read-only extraction; never enter the final edit optimizer.
  }
  size_t source_rows = 0;
  if (source_model) {
    for (const auto& r : source_model->rows) {
      source_rows += r.size();
    }
  }
  setInt(block, "cts_data_skew_source_rows", source_rows);
  if (!baseline.finite) {
    throw std::runtime_error("Unconstrained marked gate or clock timing");
  }
  std::vector<double> ceilings, floors, weights;
  int solve_budget_ms = 30000;
  if (auto* prop
      = odb::dbIntProperty::find(block, "cts_data_skew_native_solve_ms")) {
    solve_budget_ms = prop->getValue();
  }
  if (solve_budget_ms < 100 || solve_budget_ms > 300000) {
    throw std::runtime_error("Invalid native solver budget");
  }
  int feedback_budget = 4;
  if (auto* prop = odb::dbIntProperty::find(
          block, "cts_data_skew_native_feedback_rounds")) {
    feedback_budget = prop->getValue();
  }
  if (feedback_budget < 1 || feedback_budget > 16) {
    throw std::runtime_error("Native feedback rounds must be 1..16");
  }
  Metrics reference = baseline;
  for (size_t i = 0; i < refs.size(); ++i) {
    reference.clocks[i] = {refs[i].lo, refs[i].hi};
  }
  size_t explicit_refs = 0;
  for (size_t g = 0; g < gates.size(); ++g) {
    if (std::isfinite(gates[g].ceiling)) {
      ++explicit_refs;
      reference.widths[g] = gates[g].ceiling;
    }
    if (!std::isfinite(gates[g].ceiling)) {
      setDouble(block->findInst(gates[g].name.c_str()),
                "cts_data_skew_gate_reference_ns",
                reference.widths[g]);
    }
    ceilings.push_back(reference.widths[g]);
    reference.widths[g] = ceilings.back();
    floors.push_back(gates[g].ranking_floor);
    weights.push_back(gates[g].weight);
  }
  if (explicit_refs != 0 && explicit_refs != gates.size()) {
    throw std::runtime_error("Complete stock gate reference coverage required");
  }
  if (auto* ratio
      = odb::dbDoubleProperty::find(block, "cts_data_skew_clock_area_ratio")) {
    if (!std::isfinite(ratio->getValue()) || ratio->getValue() < 1
        || ratio->getValue() > 1.5) {
      throw std::runtime_error("Clock area ratio must be between 1 and 1.5");
    }
    baseline.clock_area_ratio = ratio->getValue();
    reference.clock_area_ratio = ratio->getValue();
  }
  if (odb::dbDoubleProperty::find(block, "cts_data_skew_stock_area_um2")) {
    reference.area = property(block, "cts_data_skew_stock_area_um2");
    reference.clock_area
        = property(block, "cts_data_skew_stock_clock_area_um2");
    if (reference.area <= 0 || reference.clock_area <= 0) {
      throw std::runtime_error("Invalid stock area reference");
    }
  }
  if (!odb::dbDoubleProperty::find(block, "cts_data_skew_stock_area_um2")) {
    setDouble(block, "cts_data_skew_stock_area_um2", reference.area);
    setDouble(
        block, "cts_data_skew_stock_clock_area_um2", reference.clock_area);
  }
  auto* delay_flag
      = odb::dbIntProperty::find(block, "cts_data_skew_native_delay_insertion");
  const bool delay_enabled = delay_flag && delay_flag->getValue() == 1;
  if (delay_flag && delay_flag->getValue() != 0
      && delay_flag->getValue() != 1) {
    throw std::runtime_error("Delay insertion enable must be 0/1");
  }
  int max_depth = 4;
  if (auto* depth
      = odb::dbIntProperty::find(block, "cts_data_skew_delay_max_depth")) {
    max_depth = depth->getValue();
  }
  if (max_depth < 1 || max_depth > 8) {
    throw std::runtime_error("Clock delay depth must be 1..8");
  }
  auto* delay_master = delay_enabled
                           ? block->getDb()->findMaster(names.front().c_str())
                           : nullptr;
  if (delay_enabled && !delay_master) {
    throw std::runtime_error("Missing delay buffer LEF master");
  }
  auto* sink_flag
      = odb::dbIntProperty::find(block, "cts_data_skew_native_sink_delays");
  const bool sink_delays = sink_flag && sink_flag->getValue() == 1;
  if (sink_flag && sink_flag->getValue() != 0 && sink_flag->getValue() != 1) {
    throw std::runtime_error("Sink clock delay enable must be 0/1");
  }
  if (sink_delays && !delay_enabled) {
    throw std::runtime_error(
        "Sink delays require clock delay insertion enabled");
  }
  // Opt-in measured pool of branch sizing plus CK-pin insertion controls.
  auto* combined_flag
      = odb::dbIntProperty::find(block, "cts_data_skew_combined_controls");
  const bool combined_controls
      = combined_flag && combined_flag->getValue() == 1;
  if (combined_flag && combined_flag->getValue() != 0
      && combined_flag->getValue() != 1) {
    throw std::runtime_error("Combined clock controls must be 0/1");
  }
  auto* menu_flag
      = odb::dbIntProperty::find(block, "cts_data_skew_sink_master_menu");
  const bool master_menu = menu_flag && menu_flag->getValue() == 1;
  if (menu_flag && menu_flag->getValue() != 0 && menu_flag->getValue() != 1) {
    throw std::runtime_error("Sink master menu must be 0/1");
  }
  if ((master_menu || combined_controls) && !sink_delays) {
    throw std::runtime_error("Extended sink controls require sink mode");
  }
  if (combined_controls && budget < 2) {
    throw std::runtime_error("Combined controls need at least two trials");
  }
  const int branch_budget = combined_controls ? budget / 2 : budget;
  int cache_mode = 0;
  std::string cache_path, cache_signature, fingerprint, legacy_fingerprint;
  if (auto* mode
      = odb::dbIntProperty::find(block, "cts_data_skew_candidate_cache_mode")) {
    cache_mode = mode->getValue();
  }
  if (cache_mode < 0 || cache_mode > 2) {
    throw std::runtime_error("Candidate cache mode must be 0/1/2");
  }
  if (cache_mode) {
    auto* path = odb::dbStringProperty::find(
        block, "cts_data_skew_candidate_cache_path");
    auto* sig = odb::dbStringProperty::find(
        block, "cts_data_skew_candidate_cache_signature");
    if (!path || !sig || std::string(sig->getValue()).empty()) {
      throw std::runtime_error("Cache needs path and library/SDC/RC signature");
    }
    cache_path = path->getValue();
    cache_signature = sig->getValue();
    fingerprint = cacheFingerprint(
        block, refs, gates, names, budget, max_depth, delay_enabled, false);
    if (sink_delays) {
      fingerprint = "sink_pin_v3:" + fingerprint;
    }
    if (source_model) {
      fingerprint = "source_q_v1:" + sourceFingerprint(*source_model) + ":"
                    + fingerprint;
    }
    if (master_menu) {
      fingerprint = "menu_v1:" + fingerprint;
    }
    if (combined_controls) {
      fingerprint = "mixed_v1:" + fingerprint;
    }
    auto legacy_gates = gates;
    size_t legacy_floors = 0;
    for (auto& gate : legacy_gates) {
      auto* inst = block->findInst(gate.name.c_str());
      if (odb::dbDoubleProperty::find(inst,
                                      "cts_data_skew_cache_legacy_floor_ns")) {
        gate.ranking_floor
            = property(inst, "cts_data_skew_cache_legacy_floor_ns");
        ++legacy_floors;
      }
    }
    if (legacy_floors != 0 && legacy_floors != gates.size()) {
      throw std::runtime_error("Incomplete legacy cache ranking metadata");
    }
    legacy_fingerprint = cacheFingerprint(block,
                                          refs,
                                          legacy_gates,
                                          names,
                                          budget,
                                          max_depth,
                                          delay_enabled,
                                          true);
    if (sink_delays) {
      legacy_fingerprint = "sink_pin_v3:" + legacy_fingerprint;
    }
    if (source_model) {
      legacy_fingerprint = "source_q_v1:" + sourceFingerprint(*source_model)
                           + ":" + legacy_fingerprint;
    }
    if (master_menu) {
      legacy_fingerprint = "menu_v1:" + legacy_fingerprint;
    }
    if (combined_controls) {
      legacy_fingerprint = "mixed_v1:" + legacy_fingerprint;
    }
    if (cache_mode == 1
        && (std::filesystem::exists(cache_path)
            || std::filesystem::exists(cache_path + ".partial"))) {
      throw std::runtime_error("Cache already exists; input preserved");
    }
  }
  Metrics current = baseline;
  int trials = 0, accepted = 0;
  std::vector<Choice> choices;
  if (cache_mode == 2) {
    choices = loadCandidateCache(
        cache_path,
        cache_signature,
        fingerprint,
        baseline,
        [&](const std::string& inst,
            const std::string& original,
            const std::string& replacement) {
          auto* actor = block->findInst(inst.c_str());
          auto* old_cell = network->findLibertyCell(original.c_str());
          auto* new_cell = network->findLibertyCell(replacement.c_str());
          if (!actor || actor->isDoNotTouch() || !old_cell || !new_cell
              || network->libertyCell(network->dbToSta(actor)) != old_cell
              || std::find(cells.begin(), cells.end(), new_cell) == cells.end()
              || (old_cell->isSequential() ? !sink_delays
                                           : !drivers.count(inst))) {
            throw std::runtime_error("Cached clock actor/cell mismatch");
          }
          return Choice{actor, old_cell, new_cell, baseline};
        },
        legacy_fingerprint);
    for (auto& choice : choices) {
      if (!choice.delay_pin_name.empty()) {
        if (!sink_delays) {
          throw std::runtime_error("Sink edits require sink mode");
        }
        choice.delay_input
            = choice.inst->findITerm(choice.delay_pin_name.c_str());
        if (!choice.delay_input || choice.sites.empty()
            || !choice.delay_input->getNet()
            || choice.delay_input->getNet()->isDoNotTouch()
            || choice.delay_input->getNet()->getSigType()
                   != odb::dbSigType::CLOCK
            || std::none_of(refs.begin(), refs.end(), [&](const auto& ref) {
                 return ref.pin == choice.delay_input;
               })) {
          throw std::runtime_error("Cached sink-clock pin mismatch");
        }
      } else if (choice.original->isSequential()) {
        throw std::runtime_error("Sequential actor lacks a sink clock pin");
      }
    }
    logger->info(
        utl::CTS, 330, "Reused {} measured clock candidates", choices.size());
  } else {
    if (!sink_delays || combined_controls) {
      // Probe candidates against one fixed entry design; do not greedily
      // commit.
      for (const auto& [name, inst] : drivers) {
        if (trials && trials % 128 < int(cells.size())) {
          logger->info(
              utl::CTS, 331, "Measured {} physical clock trials", trials);
        }
        auto* original = network->libertyCell(network->dbToSta(inst));
        const DelaySite original_site{inst->getLocation(), inst->getOrient()};
        for (auto* cell : cells) {
          if (cell == original || trials >= branch_budget) {
            continue;
          }
          if (!resizer->replaceCell(network->dbToSta(inst), cell, false)) {
            continue;
          }
          ++trials;
          if (!placementLegal(inst)) {
            auto site
                = freeDelaySite(block, inst->getMaster(), original_site.point);
            if (site) {
              inst->setOrient(site->orient);
              inst->setLocation(site->point.x(), site->point.y());
            }
            if (!site || !placementLegal(inst)) {
              if (!resizer->replaceCell(
                      network->dbToSta(inst), original, false)) {
                throw std::runtime_error("Placement-screen rollback failed");
              }
              inst->setOrient(original_site.orient);
              inst->setLocation(original_site.point.x(),
                                original_site.point.y());
              continue;
            }
          }
          const DelaySite proposed_site{inst->getLocation(), inst->getOrient()};
          const auto proposed_box = inst->getBBox()->getBox();
          Metrics candidate;
          try {
            estimate();
            candidate
                = measure(block, network, sta, refs, gates, source_model.get());
          } catch (...) {
            if (!resizer->replaceCell(
                    network->dbToSta(inst), original, false)) {
              throw std::runtime_error("Sizing rollback failed");
            }
            inst->setOrient(original_site.orient);
            inst->setLocation(original_site.point.x(), original_site.point.y());
            estimate();
            throw;
          }
          if (!resizer->replaceCell(network->dbToSta(inst), original, false)) {
            throw std::runtime_error("Sizing rollback failed");
          }
          inst->setOrient(original_site.orient);
          inst->setLocation(original_site.point.x(), original_site.point.y());
          estimate();
          if (candidate.finite) {
            Choice choice{inst, original, cell, std::move(candidate)};
            choice.leaf_site = proposed_site;
            choice.boxes.push_back(proposed_box);
            choices.push_back(std::move(choice));
          }
        }
        if (delay_enabled && trials < branch_budget) {
          auto* input = bufferInput(inst);
          double target_error = 0;
          size_t count = 0;
          for (size_t i = 0; i < refs.size(); ++i) {
            if (refs[i].pin->getNet()->getFirstOutput()->getInst() == inst) {
              const double target
                  = property(refs[i].pin, "cts_data_skew_target_ns");
              if (std::abs(target) > .040000001) {
                throw std::runtime_error(
                    "Clock target exceeds 40 ps movement bound");
              }
              target_error
                  += (refs[i].lo + refs[i].hi) / 2 + target
                     - (baseline.clocks[i].first + baseline.clocks[i].second)
                           / 2;
              ++count;
            }
          }
          if (count && target_error / count > .001) {
            ClockEdit edit{
                inst, original, input, input->getNet(), original_site};
            std::vector<DelaySite> sites;
            std::vector<odb::Rect> boxes;
            int wanted_depth = 1;
            for (int depth = 1; depth <= max_depth && trials < branch_budget;
                 ++depth) {
              auto loc
                  = freeDelaySite(block, delay_master, inst->getLocation());
              if (!loc) {
                break;
              }
              try {
                addDelayBuffer(edit, delay_master, *loc, resizer);
                sites.push_back(*loc);
                boxes.push_back(edit.buffers.back()->getBBox()->getBox());
                ++trials;
                estimate();
                auto candidate = measure(
                    block, network, sta, refs, gates, source_model.get());
                if (depth == 1) {
                  double increment = 0;
                  for (size_t i = 0; i < refs.size(); ++i) {
                    if (refs[i].pin->getNet()->getFirstOutput()->getInst()
                        == inst) {
                      increment += (candidate.clocks[i].first
                                    + candidate.clocks[i].second
                                    - baseline.clocks[i].first
                                    - baseline.clocks[i].second)
                                   / 2;
                    }
                  }
                  if (increment > 1e-6) {
                    wanted_depth
                        = std::clamp(int(std::round(target_error / increment)),
                                     1,
                                     max_depth);
                  }
                }
                if (candidate.finite
                    && (depth == 1 || depth == wanted_depth
                        || depth == wanted_depth + 1)) {
                  Choice choice{
                      inst, original, cells.front(), std::move(candidate)};
                  choice.leaf_site = original_site;
                  choice.sites = sites;
                  choice.boxes = boxes;
                  choices.push_back(std::move(choice));
                }
                if (depth >= std::min(max_depth, wanted_depth + 1)) {
                  break;
                }
              } catch (...) {
                undoClockEdit(edit, network, resizer);
                estimate();
                throw;
              }
            }
            undoClockEdit(edit, network, resizer);
            estimate();
          }
        }
        if (trials >= branch_budget) {
          break;
        }
      }
    }  // Shared-branch actuator family.
    if (sink_delays) {
      // Prioritize positive independent launch targets, with deterministic tie
      // order.
      std::vector<std::pair<double, size_t>> targets;
      for (size_t r = 0; r < refs.size(); ++r) {
        auto* pin = refs[r].pin;
        auto* net = pin->getNet();
        auto* driver = net->getFirstOutput();
        if (pin->getInst()->isDoNotTouch() || net->isDoNotTouch() || !driver
            || driver->getInst()->isDoNotTouch()) {
          continue;
        }
        const double target = property(pin, "cts_data_skew_target_ns");
        if (std::abs(target) > .040000001) {
          throw std::runtime_error("Sink clock target exceeds 40 ps bound");
        }
        const double error
            = (refs[r].lo + refs[r].hi) / 2 + target
              - (baseline.clocks[r].first + baseline.clocks[r].second) / 2;
        if (error > .001) {
          targets.emplace_back(error, r);
        }
      }
      std::sort(
          targets.begin(), targets.end(), [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first : a.second < b.second;
          });
      for (const auto& [error, r] : targets) {
        if (trials >= budget) {
          break;
        }
        auto* pin = refs[r].pin;
        auto* inst = pin->getInst();
        auto* original = network->libertyCell(network->dbToSta(inst));
        const DelaySite original_site{inst->getLocation(), inst->getOrient()};
        // Null original disables register resizing/placement during rollback.
        ClockEdit edit{inst, nullptr, pin, pin->getNet(), original_site};
        std::vector<DelaySite> sites;
        std::vector<odb::Rect> boxes;
        // Keep legacy one-master enumeration unless explicitly enabled.
        std::vector<const sta::LibertyCell*> insertion_cells
            = master_menu ? cells
                          : std::vector<const sta::LibertyCell*>{cells.front()};
        for (auto* insertion_cell : insertion_cells) {
          if (trials >= budget) {
            break;
          }
          auto* insertion_master
              = block->getDb()->findMaster(insertion_cell->name().c_str());
          if (!insertion_master) {
            throw std::runtime_error("Missing sink insertion master");
          }
          sites.clear();
          boxes.clear();
          try {
            for (int depth = 1; depth <= max_depth && trials < budget;
                 ++depth) {
              auto loc
                  = freeDelaySite(block, insertion_master, original_site.point);
              if (!loc) {
                break;
              }
              addDelayBuffer(edit, insertion_master, *loc, resizer);
              sites.push_back(*loc);
              boxes.push_back(edit.buffers.back()->getBBox()->getBox());
              ++trials;
              estimate();
              auto candidate = measure(
                  block, network, sta, refs, gates, source_model.get());
              const double candidate_shift = candidate.shift;
              if (candidate.finite) {
                Choice choice{
                    inst, original, insertion_cell, std::move(candidate)};
                choice.leaf_site = original_site;
                choice.sites = sites;
                choice.boxes = boxes;
                choice.delay_input = pin;
                choice.delay_pin_name = pin->getMTerm()->getName();
                choices.push_back(std::move(choice));
              }
              // Longer chains cannot help if this sink already exceeds its
              // movement cap.
              if (candidate_shift > .04) {
                break;
              }
            }
          } catch (...) {
            undoClockEdit(edit, network, resizer);
            estimate();
            throw;
          }
          undoClockEdit(edit, network, resizer);
          estimate();
        }  // Each insertion master is measured against the unchanged baseline.
        if (trials % 64 == 0) {
          logger->info(utl::CTS,
                       616,
                       "Measured {} individual sink clock trials",
                       trials);
        }
      }
    }
  }
  if (cache_mode == 1) {
    saveCandidateCache(
        cache_path, cache_signature, fingerprint, baseline, choices);
  }
  if (auto* prefix
      = odb::dbIntProperty::find(block, "cts_data_skew_candidate_prefix")) {
    if (prefix->getValue() < 1 || prefix->getValue() > int(choices.size())) {
      throw std::runtime_error("Invalid diagnostic candidate prefix");
    }
    choices.resize(prefix->getValue());
  }
  setInt(block, "cts_data_skew_native_cache_hit", cache_mode == 2);
  setInt(block, "cts_data_skew_native_candidate_count", choices.size());
  bool diagnostic_only = false;
  if (auto* flag = odb::dbIntProperty::find(
          block, "cts_data_skew_native_relaxation_only")) {
    if (flag->getValue() != 0 && flag->getValue() != 1) {
      throw std::runtime_error("Relaxation-only flag must be 0/1");
    }
    diagnostic_only = flag->getValue() == 1;
  }
  if (diagnostic_only) {
    JointDiagnostics relaxed;
    chooseJoint(choices,
                baseline,
                ceilings,
                &reference,
                floors,
                weights,
                {},
                &relaxed,
                solve_budget_ms,
                true);
    std::ostringstream summary;
    summary << std::setprecision(17)
            << "primary=" << solverStatus(relaxed.primary)
            << ",secondary=" << solverStatus(relaxed.secondary)
            << ",worst_excess_ps=" << relaxed.worst_ps
            << ",weighted_spread_ps=" << relaxed.total_ps;
    std::ostringstream fractions;
    for (size_t c = 0; c < relaxed.fractions.size(); ++c) {
      if (relaxed.fractions[c] > 1e-8) {
        fractions << c << ',' << choices[c].inst->getName() << ','
                  << relaxed.fractions[c] << '\n';
      }
    }
    setString(block,
              "cts_data_skew_native_relaxation_summary",
              summary.str().c_str());
    setString(block,
              "cts_data_skew_native_relaxation_fractions",
              fractions.str().c_str());
    setInt(block, "cts_data_skew_native_edits", 0);
    logger->info(
        utl::CTS,
        332,
        "Clock response relaxation diagnostic only: {}; no edits applied",
        summary.str());
    return;  // Fractional mixes are not physical clock edits and are never
             // replayed.
  }
  std::vector<size_t> selected;
  std::vector<size_t> applied;
  std::vector<ClockEdit> edits;
  const auto rollback = [&]() {
    for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
      undoClockEdit(*it, network, resizer);
    }
    edits.clear();
    applied.clear();
    estimate();
    current = measure(block, network, sta, refs, gates, source_model.get());
    for (size_t g = 0; g < baseline.widths.size(); ++g) {
      for (size_t p = 0; p < baseline.envelopes[g].size(); ++p) {
        const auto [be, bl] = baseline.envelopes[g][p];
        const auto [ce, cl] = current.envelopes[g][p];
        if (std::max(std::abs(ce - be), std::abs(cl - bl))
            > spread_tolerance_ns) {
          throw std::runtime_error(
              "Joint rollback input timing did not reproduce");
        }
      }
    }
    for (size_t r = 0; r < baseline.clocks.size(); ++r) {
      if (std::max(
              std::abs(current.clocks[r].first - baseline.clocks[r].first),
              std::abs(current.clocks[r].second - baseline.clocks[r].second))
          > spread_tolerance_ns) {
        throw std::runtime_error(
            "Joint rollback clock timing did not reproduce");
      }
    }
  };
  int joint_rejected = 0, rounds = 0, proposal_worsened = 0;
  double max_proposal_regression = 0;
  std::ostringstream report;
  report << "round,primary,secondary,tertiary,relaxation,selected,"
            "actual_regressions,max_input_prediction_error_ps,reason\n";
  std::string stop_reason = "FEEDBACK_BUDGET_EXHAUSTED";
  JointDiagnostics diag;
  const auto incumbents
      = verifiedSingleChoices(choices, baseline, reference, floors);
  bool feasible_subpool = false;
  if (auto* p = odb::dbIntProperty::find(
          block, "cts_data_skew_native_feasible_subpool")) {
    if (p->getValue() != 0 && p->getValue() != 1) {
      throw std::runtime_error("Feasible subpool flag must be 0/1");
    }
    feasible_subpool = p->getValue() == 1;
  }
#include "DataSkewPortfolio.h"
  setInt(block, "cts_data_skew_native_measured_incumbents", incumbents.size());
  setInt(block, "cts_data_skew_native_used_measured_backup", accepted_backup);
  setInt(block, "cts_data_skew_native_iterations", rounds);
  setInt(block, "cts_data_skew_native_solver_status", diag.primary);
  setString(block, "cts_data_skew_native_stop_reason", stop_reason.c_str());
  setString(block, "cts_data_skew_native_diagnostics", report.str().c_str());
  setInt(block, "cts_data_skew_native_proposal_worsened", proposal_worsened);
  setDouble(block,
            "cts_data_skew_native_proposal_regression_ns",
            max_proposal_regression);
  setInt(block,
         "cts_data_skew_native_reference_feasible",
         protectsGates(current, reference));
  int improved = 0, worsened = 0;
  for (size_t g = 0; g < baseline.widths.size(); ++g) {
    improved += current.widths[g] < baseline.widths[g] - spread_tolerance_ns;
    worsened += current.widths[g] > baseline.widths[g] + spread_tolerance_ns;
  }
  setInt(block, "cts_data_skew_native_gates_improved", improved);
  setInt(block, "cts_data_skew_native_gates_worsened", worsened);
  setInt(block, "cts_data_skew_native_joint_selected", selected.size());
  setInt(block, "cts_data_skew_native_joint_rejected", joint_rejected);
  int delay_cells = 0, moved_cells = 0;
  for (const auto& edit : edits) {
    delay_cells += edit.buffers.size();
    moved_cells += edit.leaf->getLocation() != edit.original_site.point
                   || edit.leaf->getOrient() != edit.original_site.orient;
  }
  (void) resizer->designArea();
  setInt(block, "cts_data_skew_native_delay_cells", delay_cells);
  setInt(block, "cts_data_skew_native_moved_clock_cells", moved_cells);
  setDouble(block, "cts_data_skew_native_baseline_cost_ns", baseline.cost);
  setDouble(block, "cts_data_skew_native_final_cost_ns", current.cost);
  setInt(block, "cts_data_skew_native_trials", trials);
  setInt(block, "cts_data_skew_native_edits", accepted);
  logger->info(utl::CTS,
               305,
               "Data-skew native sizing: {} gates, {} clocks, {} trials, {} "
               "edits; gate cost {:.9f} -> {:.9f} ns (placement screen)",
               gates.size(),
               refs.size(),
               trials,
               accepted,
               baseline.cost,
               current.cost);
  if (explicit_refs != 0
      && (!protectsGates(current, reference) || !legal(current, reference))) {
    throw std::runtime_error(
        "No physically legal stock-referenced realization; input preserved");
  }
}
}  // namespace cts
