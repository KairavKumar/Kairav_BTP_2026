// SPDX-License-Identifier: BSD-3-Clause
// Internal search transaction, included inside sizeDataSkewLeaves.
bool accepted_backup = false;
bool portfolio = true;
if (auto* p
    = odb::dbIntProperty::find(block, "cts_data_skew_native_portfolio")) {
  if (p->getValue() != 0 && p->getValue() != 1) {
    throw std::runtime_error("Portfolio flag must be 0/1");
  }
  portfolio = p->getValue() == 1;
}
int pair_budget = 32;
if (auto* p
    = odb::dbIntProperty::find(block, "cts_data_skew_native_pair_budget")) {
  pair_budget = p->getValue();
}
if (pair_budget < 0 || pair_budget > 256) {
  throw std::runtime_error("Pair budget must be 0..256");
}
const size_t primitive_count = choices.size();
const auto score = [&](const Metrics& m) {
  DataSkewScore value;
  value.weighted = m.cost;
  for (size_t g = 0; g < gates.size(); ++g) {
    value.worst = std::max(value.worst, m.widths[g] - floors[g]);
  }
  for (size_t r = 0; r < refs.size(); ++r) {
    value.movement
        += std::abs(m.clocks[r].first - reference.clocks[r].first)
           + std::abs(m.clocks[r].second - reference.clocks[r].second);
  }
  return value;
};
Metrics best = baseline;
auto best_score = score(best);
std::vector<size_t> best_selection;
std::string best_family = "entry";
const auto expand = [&](const std::vector<size_t>& selection) {
  std::set<size_t> result;
  std::set<std::string> actors;
  for (size_t c : selection) {
    if (c >= choices.size()) {
      throw std::runtime_error("Invalid selected control");
    }
    const auto members = choices[c].members.empty() ? std::vector<size_t>{c}
                                                    : choices[c].members;
    for (size_t member : members) {
      if (member >= primitive_count || !result.insert(member).second
          || !actors.insert(choices[member].inst->getName()).second) {
        throw std::runtime_error("Conflicting bundle primitives");
      }
    }
  }
  return std::vector<size_t>(result.begin(), result.end());
};
const auto apply = [&](const std::vector<size_t>& selection) {
  if (!edits.empty()) {
    throw std::runtime_error("Unfinished data-skew transaction");
  }
  const auto primitives = expand(selection);
  try {
    for (size_t c : primitives) {
      const auto& choice = choices[c];
      const DelaySite before_site{choice.inst->getLocation(),
                                  choice.inst->getOrient()};
      if (choice.sites.empty()) {
        if (!resizer->replaceCell(
                network->dbToSta(choice.inst), choice.replacement, false)) {
          throw std::runtime_error("Clock sizing application failed");
        }
        ClockEdit edit;
        edit.leaf = choice.inst;
        edit.original = choice.original;
        edit.original_site = before_site;
        edits.push_back(edit);
        choice.inst->setOrient(choice.leaf_site.orient);
        choice.inst->setLocation(choice.leaf_site.point.x(),
                                 choice.leaf_site.point.y());
      } else {
        auto* input = choice.delay_input ? choice.delay_input
                                         : bufferInput(choice.inst);
        edits.push_back(
            ClockEdit{choice.inst,
                      choice.delay_input ? nullptr : choice.original,
                      input,
                      input->getNet(),
                      before_site});
        auto* master = choice.delay_input
                           ? block->getDb()->findMaster(
                                 choice.replacement->name().c_str())
                           : delay_master;
        if (!master) {
          throw std::runtime_error("Missing clock insertion master");
        }
        for (const auto& site : choice.sites) {
          addDelayBuffer(edits.back(), master, site, resizer);
        }
      }
      applied.push_back(c);
    }
    full_estimate();
    current = measure(block, network, sta, refs, gates, source_model.get());
  } catch (...) {
    rollback();
    throw;
  }
};
const auto predictionError = [&](const std::vector<size_t>& selection) {
  double error = 0;
  for (size_t g = 0; g < gates.size(); ++g) {
    std::vector<std::pair<double, double>> predicted(
        baseline.envelopes[g].size(),
        {std::numeric_limits<double>::infinity(),
         -std::numeric_limits<double>::infinity()});
    for (size_t p = 0; p < responseSize(baseline, g); ++p) {
      auto [early, late] = responseAt(baseline, g, p);
      for (size_t c : selection) {
        const auto base = responseAt(baseline, g, p),
                   trial = responseAt(choices[c].measured, g, p);
        early += trial.first - base.first;
        late += trial.second - base.second;
      }
      const size_t input = source_model ? source_model->rows[g][p].input : p;
      predicted[input].first = std::min(predicted[input].first, early);
      predicted[input].second = std::max(predicted[input].second, late);
    }
    for (size_t p = 0; p < predicted.size(); ++p) {
      error = std::max(
          {error,
           std::abs(predicted[p].first - current.envelopes[g][p].first) * 1000,
           std::abs(predicted[p].second - current.envelopes[g][p].second)
               * 1000});
    }
  }
  return error;
};
int actual_evaluations = 0;
const auto evaluate = [&](const std::vector<size_t>& selection,
                          const std::string& family) {
  apply(selection);
  ++actual_evaluations;
  ++rounds;
  bool placement = true, improved = false;
  proposal_worsened = 0;
  max_proposal_regression = 0;
  for (size_t c : applied) {
    placement &= placementLegal(choices[c].inst);
  }
  for (size_t g = 0; g < gates.size(); ++g) {
    improved |= current.widths[g] < baseline.widths[g] - spread_tolerance_ns;
    proposal_worsened
        += current.widths[g] > reference.widths[g] + spread_tolerance_ns;
    max_proposal_regression = std::max(max_proposal_regression,
                                       current.widths[g] - reference.widths[g]);
  }
  const bool feasible = legal(current, reference)
                        && protectsGates(current, reference) && placement
                        && improved;
  const bool better
      = feasible && betterDataSkewScore(score(current), best_score);
  if (better) {
    best = current;
    best_score = score(current);
    best_selection = selection;
    best_family = family;
  }
  std::string reason;
  if (!current.finite) {
    reason += "nonfinite;";
  }
  if (!current.electrical) {
    reason += "electrical;";
  }
  if (current.setup < .01) {
    reason += "setup;";
  }
  if (current.hold < .01) {
    reason += "hold;";
  }
  if (current.shift > .04) {
    reason += "clock_movement;";
  }
  if (current.area > 1.10 * reference.area) {
    reason += "area;";
  }
  if (current.clock_area > reference.clock_area_ratio * reference.clock_area) {
    reason += "clock_area;";
  }
  if (proposal_worsened) {
    reason += "gate_cap;";
  }
  if (!placement) {
    reason += "placement;";
  }
  if (!improved) {
    reason += "no_improvement;";
  }
  // Capture source/end identities now; rollback invalidates this timing state.
  for (const auto* mode : {sta::MinMax::min(), sta::MinMax::max()}) {
    const double slack
        = mode == sta::MinMax::min() ? current.hold : current.setup;
    if (slack >= .01) {
      continue;
    }
    sta::Slack actual_slack;
    sta::Vertex* endpoint = nullptr;
    sta->worstSlack(mode, actual_slack, endpoint);
    auto* path = endpoint ? sta->vertexWorstSlackPath(endpoint, mode) : nullptr;
    if (path) {
      sta::PathExpanded expanded(path, sta);
      report << "timing_blocker,"
             << (mode == sta::MinMax::min() ? "hold" : "setup") << ",source,"
             << network->pathName(expanded.startPath()->pin(sta))
             << ",endpoint," << network->pathName(path->pin(sta))
             << ",slack_ps," << sta::delayAsFloat(actual_slack) / ns * 1000
             << '\n';
    }
  }
  report << "portfolio," << family << ",patterns," << selection.size()
         << ",primitives," << applied.size() << '\n';
  report << rounds << ',' << solverStatus(diag.primary) << ','
         << solverStatus(diag.secondary) << ',' << solverStatus(diag.tertiary)
         << ',' << solverStatus(diag.relaxation) << ',' << selection.size()
         << ',' << proposal_worsened << ',' << predictionError(selection) << ','
         << (better     ? "ARCHIVED"
             : feasible ? "FEASIBLE_NOT_BETTER"
                        : reason)
         << '\n';
  logger->info(utl::CTS,
               329,
               "Data-skew {} evaluation {}: {} controls, {}",
               family,
               actual_evaluations,
               applied.size(),
               better     ? "archived"
               : feasible ? "feasible"
                          : reason);
  if (!feasible) {
    ++joint_rejected;
  }
  return feasible;
};
const auto family
    = [&](const std::vector<bool>* allowed, const std::string& name) {
        std::vector<std::vector<size_t>> denied;
        for (int attempt = 0; attempt < feedback_budget; ++attempt) {
          diag = JointDiagnostics{};
          auto proposal = chooseJoint(choices,
                                      baseline,
                                      ceilings,
                                      &reference,
                                      floors,
                                      weights,
                                      denied,
                                      &diag,
                                      solve_budget_ms,
                                      false,
                                      allowed);
          report << "family," << name << ",attempt," << attempt + 1 << '\n'
                 << diag.stage_report;
          if (proposal.empty()) {
            break;
          }
          const bool feasible = evaluate(proposal, name);
          rollback();
          if (feasible) {
            break;
          }
          denied.push_back(std::move(proposal));
        }
      };
std::vector<bool> singles(choices.size(), false);
for (size_t c : incumbents) {
  singles[c] = true;
}
if (portfolio || feasible_subpool) {
  family(&singles, "feasible_singles");
}
int pair_trials = 0, accepted_patterns = 0;
if (portfolio && pair_budget) {
  struct Pair
  {
    size_t a, b;
    DataSkewScore score;
    bool both;
  };
  std::vector<std::vector<size_t>> improvers(gates.size());
  for (size_t c = 0; c < primitive_count; ++c) {
    for (size_t g = 0; g < gates.size(); ++g) {
      if (choices[c].measured.widths[g] < baseline.widths[g] - 1e-7) {
        improvers[g].push_back(c);
      }
    }
  }
  std::vector<Pair> proposals;
  std::set<std::pair<size_t, size_t>> seen;
  const auto compatible = [&](size_t a, size_t b) {
    if (choices[a].inst == choices[b].inst) {
      return false;
    }
    for (const auto& x : choices[a].boxes) {
      for (const auto& y : choices[b].boxes) {
        if (x.overlaps(y)) {
          return false;
        }
      }
    }
    return true;
  };
  for (size_t a = 0; a < primitive_count; ++a) {
    if (choices[a].measured.setup < .01 || choices[a].measured.hold < .01) {
      continue;
    }
    std::vector<size_t> bad;
    for (size_t g = 0; g < gates.size(); ++g) {
      if (choices[a].measured.widths[g]
          > reference.widths[g] + spread_tolerance_ns) {
        bad.push_back(g);
      }
    }
    if (bad.empty()) {
      continue;
    }
    auto candidates = improvers[bad.front()];
    for (size_t i = 1; i < bad.size(); ++i) {
      std::vector<size_t> next;
      std::set_intersection(candidates.begin(),
                            candidates.end(),
                            improvers[bad[i]].begin(),
                            improvers[bad[i]].end(),
                            std::back_inserter(next));
      candidates.swap(next);
    }
    std::stable_sort(
        candidates.begin(), candidates.end(), [&](size_t x, size_t y) {
          const auto merit = [&](size_t b) {
            double excess = 0;
            for (size_t g : bad) {
              excess += std::max(0.0,
                                 choices[a].measured.widths[g]
                                     + choices[b].measured.widths[g]
                                     - baseline.widths[g] - reference.widths[g]
                                     - spread_tolerance_ns);
            }
            return std::pair{excess, choices[b].measured.cost};
          };
          return merit(x) < merit(y);
        });
    if (candidates.size() > 16) {
      candidates.resize(16);
    }
    for (size_t b : candidates) {
      const auto key = std::minmax(a, b);
      if (!seen.emplace(key.first, key.second).second || !compatible(a, b)
          || choices[b].measured.setup < .01
          || choices[b].measured.hold < .01) {
        continue;
      }
      Metrics predicted = baseline;
      predicted.cost = 0;
      for (size_t g = 0; g < gates.size(); ++g) {
        double low = std::numeric_limits<double>::infinity(), high = -low;
        for (size_t p = 0; p < responseSize(baseline, g); ++p) {
          const auto base = responseAt(baseline, g, p),
                     x = responseAt(choices[a].measured, g, p),
                     y = responseAt(choices[b].measured, g, p);
          low = std::min(low, x.first + y.first - base.first);
          high = std::max(high, x.second + y.second - base.second);
        }
        predicted.widths[g] = high - low;
        predicted.cost += weights[g] * (high - low);
      }
      if (!protectsGates(predicted, reference)
          || predicted.cost >= baseline.cost) {
        continue;
      }
      proposals.push_back({a,
                           b,
                           score(predicted),
                           !protectsGates(choices[b].measured, reference)});
    }
  }
  std::stable_sort(
      proposals.begin(), proposals.end(), [](const auto& a, const auto& b) {
        if (a.both != b.both) {
          return a.both > b.both;
        }
        return std::tie(a.score.worst, a.score.weighted, a.a, a.b)
               < std::tie(b.score.worst, b.score.weighted, b.a, b.b);
      });
  setInt(block, "cts_data_skew_native_pair_proposals", proposals.size());
  for (const auto& pair : proposals) {
    if (pair_trials >= pair_budget) {
      break;
    }
    ++pair_trials;
    diag = JointDiagnostics{};
    const bool feasible = evaluate({pair.a, pair.b}, "measured_pair");
    if (feasible) {
      Choice pattern = choices[pair.a];
      pattern.measured = current;
      pattern.members = {pair.a, pair.b};
      pattern.boxes.insert(pattern.boxes.end(),
                           choices[pair.b].boxes.begin(),
                           choices[pair.b].boxes.end());
      choices.push_back(std::move(pattern));
      ++accepted_patterns;
    }
    rollback();
  }
  // A measured bundle is useful both atomically and as a neighborhood for
  // exchanges around the verified incumbent. Keep these proposal families
  // separate: overlapping bundle columns can otherwise hide a simple legal
  // primitive combination behind a few inaccurate cross-bundle responses.
  std::vector<bool> repair_pool(choices.size(), false);
  for (size_t c : expand(best_selection)) {
    repair_pool[c] = true;
  }
  for (size_t c = primitive_count; c < choices.size(); ++c) {
    bool both_infeasible = true;
    for (size_t member : choices[c].members) {
      both_infeasible &= !protectsGates(choices[member].measured, reference);
    }
    if (both_infeasible) {
      for (size_t member : choices[c].members) {
        repair_pool[member] = true;
      }
    }
  }
  family(&repair_pool, "incumbent_repair_portfolio");
  std::vector<bool> pattern_pool(choices.size(), false);
  for (size_t c : incumbents) {
    pattern_pool[c] = true;
  }
  for (size_t c = primitive_count; c < choices.size(); ++c) {
    pattern_pool[c] = true;
  }
  family(&pattern_pool, "measured_patterns");
}
// Preserve a full-pool proposal family for compensating moves not discovered
// by the bounded pair heuristic. Never discard the best measured archive.
if (!portfolio && !feasible_subpool) {
  family(nullptr, "full_pool");
}
if (best_selection.empty() && !incumbents.empty()) {
  diag = JointDiagnostics{};
  evaluate({incumbents.front()}, "single_backup");
  rollback();
}
selected = best_selection;
if (!selected.empty()) {
  apply(selected);
  bool placement = true;
  for (size_t c : applied) {
    placement &= placementLegal(choices[c].inst);
  }
  if (!legal(current, reference) || !protectsGates(current, reference)
      || !placement || !betterDataSkewScore(score(current), score(baseline))) {
    rollback();
    throw std::runtime_error("Archived data-skew solution failed fresh replay");
  }
  for (size_t g = 0; g < gates.size(); ++g) {
    if (std::abs(current.widths[g] - best.widths[g]) > spread_tolerance_ns) {
      rollback();
      throw std::runtime_error("Archived solution timing did not reproduce");
    }
  }
  accepted = applied.size();
  selected = applied;
  accepted_backup = best_family == "single_backup";
  stop_reason = "ACCEPTED_ARCHIVE";
} else {
  current = baseline;
  stop_reason = "NO_VERIFIED_IMPROVEMENT";
}
report << "archive,family," << best_family << ",primitives," << accepted
       << ",actual_evaluations," << actual_evaluations << '\n';
setInt(block, "cts_data_skew_native_actual_evaluations", actual_evaluations);
setInt(block, "cts_data_skew_native_pair_trials", pair_trials);
setInt(block, "cts_data_skew_native_patterns", accepted_patterns);
