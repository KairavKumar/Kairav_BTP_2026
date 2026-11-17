// SPDX-License-Identifier: BSD-3-Clause
// Internal implementation: included after CTS measurement/clock-edit helpers.

namespace {
void exportAlignmentModel(odb::dbBlock* block,
                          sta::dbNetwork* network,
                          sta::dbSta* sta,
                          const std::vector<Reference>& refs,
                          const std::vector<Gate>& gates,
                          const SourceModel& sources,
                          const Metrics& baseline,
                          const std::string& prefix)
{
  for (const char* suffix : {"_sources.csv", "_clocks.csv"}) {
    if (std::filesystem::exists(prefix + suffix)) {
      throw std::runtime_error("Preserve existing alignment model export");
    }
  }
  std::ofstream rows(prefix + "_sources.csv");
  std::ofstream clocks(prefix + "_clocks.csv");
  if (!rows || !clocks) {
    throw std::runtime_error("Cannot open alignment model export");
  }
  rows
      << std::setprecision(17)
      << "gate,weight,source,source_rf,input,early_ns,late_ns,stock_width_ns\n";
  for (size_t g = 0; g < gates.size(); ++g) {
    for (const auto& row : sources.rows[g]) {
      const auto [lo, hi] = sourceArrival(row, sources.baseline_launches);
      const auto& source = sources.launches[row.source];
      rows << gates[g].name << ',' << gates[g].weight << ',' << source.name
           << ',' << source.rf << ',' << row.input << ',' << lo << ',' << hi
           << ',' << baseline.widths[g] << '\n';
    }
  }
  clocks << std::setprecision(17)
         << "pin,x_um,y_um,cap_f,rise_min_ns,rise_max_ns,fall_min_ns,fall_max_"
            "ns,local_ns\n";
  for (const auto& ref : refs) {
    const auto* pin = network->dbToSta(ref.pin);
    int x, y;
    ref.pin->getAvgXY(&x, &y);
    const double lo
        = arrivalNs(sta, pin, sta::RiseFallBoth::rise(), sta::MinMax::min());
    const double hi
        = arrivalNs(sta, pin, sta::RiseFallBoth::rise(), sta::MinMax::max());
    double parent = (lo + hi) / 2;
    for (auto* term : ref.pin->getNet()->getITerms()) {
      if (term->isOutputSignal()) {
        const auto* in = network->dbToSta(bufferInput(term->getInst()));
        parent
            = (arrivalNs(sta, in, sta::RiseFallBoth::rise(), sta::MinMax::min())
               + arrivalNs(
                   sta, in, sta::RiseFallBoth::rise(), sta::MinMax::max()))
              / 2;
      }
    }
    auto* port = network->libertyPort(pin);
    clocks << network->pathName(pin) << ','
           << double(x) / block->getDbUnitsPerMicron() << ','
           << double(y) / block->getDbUnitsPerMicron() << ','
           << port->capacitance(sta::RiseFall::rise(), sta::MinMax::max())
           << ',' << lo << ',' << hi << ','
           << arrivalNs(sta, pin, sta::RiseFallBoth::fall(), sta::MinMax::min())
           << ','
           << arrivalNs(sta, pin, sta::RiseFallBoth::fall(), sta::MinMax::max())
           << ',' << (lo + hi) / 2 - parent << '\n';
  }
  if (!rows || !clocks) {
    throw std::runtime_error("Incomplete alignment model export");
  }
}

// Existing Liberty tables supply cell/transition/load alternatives. This is
// construction against latency targets, not selection by the final gate score.
std::pair<double, double> alignmentGateDelay(const sta::LibertyCell* cell,
                                             sta::dbSta* sta,
                                             double slew,
                                             double cap,
                                             const sta::RiseFall* edge
                                             = sta::RiseFall::rise())
{
  sta::LibertyPort *input, *output;
  cell->bufferPorts(input, output);
  double delay = 0, out_slew = 0;
  size_t count = 0;
  auto* scene = sta->scenes().front();
  const auto* pvt
      = sta->cmdMode()->sdc()->operatingConditions(sta::MinMax::max());
  for (auto* set : const_cast<sta::LibertyCell*>(cell)
                       ->sceneCell(scene, sta::MinMax::max())
                       ->timingArcSets(input, output)) {
    for (auto* arc : set->arcs()) {
      auto* model = dynamic_cast<sta::GateTimingModel*>(arc->model());
      if (model && arc->fromEdge()->asRiseFall() == edge
          && arc->toEdge()->asRiseFall() == edge) {
        float d, s;
        model->gateDelay(pvt, slew, cap, d, s);
        delay += d / ns;
        out_slew = std::max(out_slew, double(s));
        ++count;
      }
    }
  }
  if (!count || !std::isfinite(delay) || delay <= 0) {
    throw std::runtime_error("Missing alignment buffer timing model");
  }
  return {delay / count, out_slew};
}
}  // namespace

void constructDataAlignedBranches(odb::dbBlock* block,
                                  sta::dbNetwork* network,
                                  sta::dbSta* sta,
                                  rsz::Resizer* resizer,
                                  est::EstimateParasitics* parasitics,
                                  utl::Logger* logger,
                                  const std::vector<std::string>& names)
{
  auto* enabled = odb::dbIntProperty::find(block, "cts_data_alignment");
  if (!enabled || enabled->getValue() == 0) {
    return;
  }
  if (enabled->getValue() != 1 || sta->scenes().size() != 1
      || sta->modes().size() != 1) {
    throw std::runtime_error("Alignment requires flag 0/1 and one scene/mode");
  }
  auto* old = odb::dbIntProperty::find(block, "cts_data_skew_native_sizing");
  if (old && old->getValue()) {
    throw std::runtime_error(
        "Disable final skew optimizer in construction mode");
  }
  auto* stable_flag = odb::dbIntProperty::find(block, "cts_align_stable_slew");
  if (stable_flag && stable_flag->getValue() != 0
      && stable_flag->getValue() != 1) {
    throw std::runtime_error("Stable slew flag must be 0/1");
  }
  const bool stable_slew = stable_flag && stable_flag->getValue() == 1;
  int max_stages = 6;
  if (auto* p = odb::dbIntProperty::find(block, "cts_align_max_stages")) {
    max_stages = p->getValue();
  }
  if (max_stages < 1 || max_stages > 16) {
    throw std::runtime_error("Clock branch stage limit must be 1..16");
  }
  parasitics->estimateParasitics(est::ParasiticsSrc::kPlacement);
  sta->updateTiming(false);
  std::map<int, std::vector<odb::dbITerm*>> groups;
  for (auto* inst : block->getInsts()) {
    for (auto* term : inst->getITerms()) {
      if (auto* group = odb::dbIntProperty::find(term, "cts_align_group")) {
        groups[group->getValue()].push_back(term);
      }
    }
  }
  std::vector<const sta::LibertyCell*> cells;
  std::set<std::string> menu(names.begin(), names.end());
  for (const char* name :
       {"BUF_X1", "BUF_X2", "BUF_X4", "BUF_X8", "BUF_X16", "CLKBUF_X1"}) {
    menu.insert(name);
  }
  for (const auto& name : menu) {
    auto* cell = network->findLibertyCell(name.c_str());
    if (cell && cell->isBuffer() && !cell->isInverter()
        && block->getDb()->findMaster(name.c_str())) {
      cells.push_back(cell);
    }
  }
  if (cells.empty() || groups.empty()) {
    throw std::runtime_error(
        "Alignment requires buffers and constructed groups");
  }
  int inserted = 0, unreachable = 0;
  // Every decision uses the same constructed-tree anchor. Applying one branch
  // must not give later groups a different reference without a timing refresh.
  struct Plan
  {
    odb::dbInst* leaf;
    const sta::LibertyCell* final_cell;
    const sta::LibertyCell* stage;
    const sta::LibertyCell* front;
    int count;
    std::vector<const sta::LibertyCell*> chain{};
  };
  std::vector<Plan> plans;
  for (const auto& [id, sinks] : groups) {
    auto* net = sinks.front()->getNet();
    odb::dbInst* leaf = nullptr;
    odb::dbITerm* output = nullptr;
    for (auto* pin : net->getITerms()) {
      if (pin->isOutputSignal()) {
        leaf = pin->getInst();
        output = pin;
      }
    }
    if (!leaf || leaf->isDoNotTouch() || net->isDoNotTouch()) {
      throw std::runtime_error("Protected or missing alignment leaf");
    }
    auto* input = bufferInput(leaf);
    const auto* in_pin = network->dbToSta(input);
    const auto* out_pin = network->dbToSta(output);
    const double input_time
        = (arrivalNs(sta, in_pin, sta::RiseFallBoth::rise(), sta::MinMax::min())
           + arrivalNs(
               sta, in_pin, sta::RiseFallBoth::rise(), sta::MinMax::max()))
          / 2;
    const double output_time
        = (arrivalNs(
               sta, out_pin, sta::RiseFallBoth::rise(), sta::MinMax::min())
           + arrivalNs(
               sta, out_pin, sta::RiseFallBoth::rise(), sta::MinMax::max()))
          / 2;
    const double input_fall
        = (arrivalNs(sta, in_pin, sta::RiseFallBoth::fall(), sta::MinMax::min())
           + arrivalNs(
               sta, in_pin, sta::RiseFallBoth::fall(), sta::MinMax::max()))
          / 2;
    const double output_fall
        = (arrivalNs(
               sta, out_pin, sta::RiseFallBoth::fall(), sta::MinMax::min())
           + arrivalNs(
               sta, out_pin, sta::RiseFallBoth::fall(), sta::MinMax::max()))
          / 2;
    double fall_lower = -std::numeric_limits<double>::infinity();
    double fall_upper = std::numeric_limits<double>::infinity();
    double desired_output = 0;
    for (auto* sink : sinks) {
      if (sink->getNet() != net) {
        throw std::runtime_error("Alignment group lost its leaf topology");
      }
      const auto* p = network->dbToSta(sink);
      const double clock
          = (arrivalNs(sta, p, sta::RiseFallBoth::rise(), sta::MinMax::min())
             + arrivalNs(sta, p, sta::RiseFallBoth::rise(), sta::MinMax::max()))
            / 2;
      auto* build_target
          = odb::dbDoubleProperty::find(sink, "cts_align_build_ns");
      desired_output += (build_target ? build_target->getValue()
                                      : property(sink, "cts_align_target_ns"))
                        - (clock - output_time);
      if (auto* fall_ref
          = odb::dbDoubleProperty::find(sink, "cts_align_stock_fall_ns")) {
        const double fall_clock
            = (arrivalNs(sta, p, sta::RiseFallBoth::fall(), sta::MinMax::min())
               + arrivalNs(
                   sta, p, sta::RiseFallBoth::fall(), sta::MinMax::max()))
              / 2;
        const double wire = fall_clock - output_fall;
        const double movement
            = std::max(0.0, property(block, "cts_align_movement_ns") - .005);
        fall_lower
            = std::max(fall_lower, fall_ref->getValue() - movement - wire);
        fall_upper
            = std::min(fall_upper, fall_ref->getValue() + movement - wire);
      }
    }
    desired_output /= sinks.size();
    const double required = desired_output - input_time;
    const double cap = sta->graphDelayCalc()->loadCap(
        out_pin, sta->scenes().front(), sta::MinMax::max());
    auto* vertex = sta->ensureGraph()->pinLoadVertex(in_pin);
    const double slew = sta::delayAsFloat(sta->slew(
        vertex, sta::RiseFallBoth::rise(), sta->scenes(), sta::MinMax::max()));
    const double fall_slew = sta::delayAsFloat(sta->slew(
        vertex, sta::RiseFallBoth::fall(), sta->scenes(), sta::MinMax::max()));
    double error = std::numeric_limits<double>::infinity();
    double minimum = error;
    const auto* original_cell = network->libertyCell(network->dbToSta(leaf));
    sta::LibertyPort *original_input, *original_output;
    original_cell->bufferPorts(original_input, original_output);
    const double parent_cap_budget
        = original_input->capacitance(sta::RiseFall::rise(), sta::MinMax::max())
          * (1 + 1e-6);
    Plan best{leaf, nullptr, nullptr, original_cell, 0};
    if (stable_slew) {
      // A bounded latency/slew beam explores independently chosen stages. Keep
      // the endpoint driver fixed; regenerate its input waveform rather than
      // using endpoint slew as an uncontrolled clock-delay knob.
      const double wire_cap
          = parasitics->wireClkCapacitance(sta->scenes().front()) * .3e-6;
      const double reference_slew
          = alignmentGateDelay(original_cell, sta, slew, cap).second;
      const double slew_limit = std::max(2e-12, reference_slew * .2);
      bool have_slew_match = false;
      double fallback_slew_error = std::numeric_limits<double>::infinity();
      struct State
      {
        std::vector<const sta::LibertyCell*> chain;
        double prefix, input_slew, forecast = 0, score = 0;
        double fall_prefix = 0, fall_input_slew = 0;
      };
      auto evaluate = [&](State& state) {
        const auto [last_delay, last_slew] = alignmentGateDelay(
            state.chain.empty() ? original_cell : state.chain.back(),
            sta,
            state.input_slew,
            original_input->capacitance(sta::RiseFall::rise(),
                                        sta::MinMax::max())
                + wire_cap);
        const auto [endpoint_delay, computed_endpoint_slew]
            = alignmentGateDelay(original_cell, sta, last_slew, cap);
        const double predicted
            = state.chain.empty() ? output_time - input_time
                                  : state.prefix + last_delay + endpoint_delay;
        const double endpoint_slew
            = state.chain.empty() ? reference_slew : computed_endpoint_slew;
        const auto [last_fall_delay, last_fall_slew] = alignmentGateDelay(
            state.chain.empty() ? original_cell : state.chain.back(),
            sta,
            state.fall_input_slew,
            original_input->capacitance(sta::RiseFall::fall(),
                                        sta::MinMax::max())
                + wire_cap,
            sta::RiseFall::fall());
        const auto [endpoint_fall_delay, endpoint_fall_slew]
            = alignmentGateDelay(
                original_cell, sta, last_fall_slew, cap, sta::RiseFall::fall());
        const double predicted_fall
            = state.chain.empty() ? output_fall
                                  : input_fall + state.fall_prefix
                                        + last_fall_delay + endpoint_fall_delay;
        const double fall_violation = std::max(
            {0.0, fall_lower - predicted_fall, predicted_fall - fall_upper});
        const double e = std::abs(predicted - required);
        const double slew_error = std::abs(endpoint_slew - reference_slew);
        state.forecast = predicted;
        state.score = e + std::max(0.0, slew_error - slew_limit) / ns
                      + 2 * fall_violation;
        const bool match = slew_error <= slew_limit && fall_violation < 1e-7;
        const double total_violation = slew_error + fall_violation * ns;
        if (match && !have_slew_match) {
          have_slew_match = true;
          error = std::numeric_limits<double>::infinity();
          minimum = error;
        }
        if (!match && have_slew_match) {
          return;
        }
        minimum = std::min(minimum, predicted);
        const bool better
            = match ? (e < error - 1e-7
                       || (std::abs(e - error) <= 1e-7
                           && state.chain.size() < best.count))
                    : (total_violation < fallback_slew_error - 1e-15
                       || (std::abs(total_violation - fallback_slew_error)
                               <= 1e-15
                           && e < error));
        if (better) {
          fallback_slew_error = total_violation;
          error = e;
          best = {leaf,
                  original_cell,
                  nullptr,
                  original_cell,
                  static_cast<int>(state.chain.size()),
                  state.chain};
        }
      };
      // An unchanged endpoint already preserves the bank's input load and
      // provides the fastest legal branch. Do not force an isolation stage
      // when no programmed delay is needed.
      State direct{{}, 0, slew};
      direct.fall_input_slew = fall_slew;
      evaluate(direct);
      std::vector<State> beam{{{original_cell}, 0, slew}};
      beam.front().fall_input_slew = fall_slew;
      for (int depth = 1; depth <= max_stages; ++depth) {
        for (auto& state : beam) {
          evaluate(state);
        }
        if (depth == max_stages) {
          break;
        }
        std::vector<State> expanded;
        for (const auto& state : beam) {
          for (const auto* cell : cells) {
            sta::LibertyPort *ci, *co;
            cell->bufferPorts(ci, co);
            if (ci->capacitance(sta::RiseFall::rise(), sta::MinMax::max())
                > parent_cap_budget) {
              continue;
            }
            const auto [d, s] = alignmentGateDelay(
                state.chain.back(),
                sta,
                state.input_slew,
                ci->capacitance(sta::RiseFall::rise(), sta::MinMax::max())
                    + wire_cap);
            State next{state.chain, state.prefix + d, s};
            const auto [fd, fs] = alignmentGateDelay(
                state.chain.back(),
                sta,
                state.fall_input_slew,
                ci->capacitance(sta::RiseFall::fall(), sta::MinMax::max())
                    + wire_cap,
                sta::RiseFall::fall());
            next.fall_prefix = state.fall_prefix + fd;
            next.fall_input_slew = fs;
            next.chain.push_back(cell);
            evaluate(next);
            expanded.push_back(std::move(next));
          }
        }
        std::stable_sort(
            expanded.begin(), expanded.end(), [](const auto& a, const auto& b) {
              return a.score < b.score;
            });
        std::vector<State> next;
        const size_t keep = std::min(size_t{64}, expanded.size());
        for (size_t i = 0; i < keep; ++i) {
          next.push_back(expanded[i]);
        }
        // Keep latency-range coverage, not only near-target prefixes: a faster
        // prefix may become the best complete chain at a greater depth.
        std::stable_sort(
            expanded.begin(), expanded.end(), [](const auto& a, const auto& b) {
              return a.forecast < b.forecast;
            });
        if (!expanded.empty()) {
          for (size_t i = 0; i < 32; ++i) {
            next.push_back(expanded[i * (expanded.size() - 1) / 31]);
          }
        }
        beam = std::move(next);
      }
    } else {
      for (const auto* final_cell : cells) {
        if (stable_slew && final_cell != original_cell) {
          continue;
        }
        sta::LibertyPort *fi, *fo;
        final_cell->bufferPorts(fi, fo);
        if (fi->capacitance(sta::RiseFall::rise(), sta::MinMax::max())
            > parent_cap_budget) {
          continue;  // Do not invalidate the upper tree's characterized load.
        }
        for (const auto* stage : cells) {
          sta::LibertyPort *si, *so;
          stage->bufferPorts(si, so);
          if (si->capacitance(sta::RiseFall::rise(), sta::MinMax::max())
              > parent_cap_budget) {
            continue;
          }
          for (int count = stable_slew ? 1 : 0; count <= 6; ++count) {
            if (count == 0
                && std::abs(fi->capacitance(sta::RiseFall::rise(),
                                            sta::MinMax::max())
                            - original_input->capacitance(sta::RiseFall::rise(),
                                                          sta::MinMax::max()))
                       > parent_cap_budget * 1e-6) {
              continue;
            }
            double predicted = 0, current_slew = slew;
            for (int i = 0; i < count; ++i) {
              const auto* next = i + 1 == count ? final_cell
                                                : (stable_slew && i + 2 == count
                                                       ? original_cell
                                                       : stage);
              sta::LibertyPort *next_input, *next_output;
              next->bufferPorts(next_input, next_output);
              const double load = next_input->capacitance(sta::RiseFall::rise(),
                                                          sta::MinMax::max());
              const auto [d, s] = alignmentGateDelay(
                  (i == 0 || (stable_slew && i + 1 == count)) ? original_cell
                                                              : stage,
                  sta,
                  current_slew,
                  load);
              predicted += d;
              current_slew = s;
            }
            predicted
                += alignmentGateDelay(final_cell, sta, current_slew, cap).first;
            minimum = std::min(minimum, predicted);
            const double e = std::abs(predicted - required);
            if (e < error - 1e-7
                || (std::abs(e - error) <= 1e-7 && count < best.count)) {
              error = e;
              best = {leaf, final_cell, stage, original_cell, count};
            }
          }
        }
      }
    }
    unreachable += required < minimum - 1e-6;
    plans.push_back(best);
  }
  for (const auto& plan : plans) {
    auto* master = block->getDb()->findMaster(plan.final_cell->name().c_str());
    // Keep the constructed position when the selected master fits. Only clock
    // cells may relocate; data cells never move to accommodate a wider master.
    int x, y;
    plan.leaf->getLocation(x, y);
    if (!resizer->replaceCell(
            network->dbToSta(plan.leaf), plan.final_cell, false)) {
      throw std::runtime_error("Alignment leaf master replacement failed");
    }
    if (!placementLegal(plan.leaf)) {
      const auto site = freeDelaySite(block, master, odb::Point(x, y));
      if (!site) {
        throw std::runtime_error("No legal alignment leaf site");
      }
      plan.leaf->setOrient(site->orient);
      plan.leaf->setLocation(site->point.x(), site->point.y());
    }
    plan.leaf->getLocation(x, y);
    ClockEdit edit;
    edit.leaf = plan.leaf;
    edit.input = bufferInput(plan.leaf);
    edit.parent = edit.input->getNet();
    if (!plan.chain.empty()) {
      // Grow upstream from the sink driver. Prefer adjacent sites to its left,
      // so A->Z connections run toward the sink instead of zig-zagging around
      // one fixed origin. Every new stage is row/legal-overlap checked.
      auto* original_input_pin = edit.input;
      for (int i = plan.count - 1; i >= 0; --i) {
        auto* stage = block->getDb()->findMaster(plan.chain[i]->name().c_str());
        const auto stage_site
            = freeDelaySite(block, stage, odb::Point(x - stage->getWidth(), y));
        if (!stage_site) {
          throw std::runtime_error("No legal mixed-stage clock site");
        }
        addDelayBuffer(edit, stage, *stage_site, resizer);
        auto* created = edit.buffers.back();
        created->getLocation(x, y);
        edit.input = bufferInput(created);
        ++inserted;
      }
      edit.input = original_input_pin;
    } else {
      for (int i = 0; i < plan.count; ++i) {
        auto* stage = block->getDb()->findMaster(
            ((i == 0 || (stable_slew && i + 1 == plan.count)) ? plan.front
                                                              : plan.stage)
                ->name()
                .c_str());
        const auto stage_site = freeDelaySite(block, stage, odb::Point(x, y));
        if (!stage_site) {
          throw std::runtime_error("No legal alignment stage site");
        }
        addDelayBuffer(edit, stage, *stage_site, resizer);
        ++inserted;
      }
    }
  }
  parasitics->estimateParasitics(est::ParasiticsSrc::kPlacement);
  sta->updateTiming(false);
  setInt(block, "cts_align_inserted_stages", inserted);
  setInt(block, "cts_align_unreachable_groups", unreachable);
  logger->info(utl::CTS,
               320,
               "Data-aligned construction: {} groups, {} stages, {} targets "
               "below fastest characterized branch.",
               groups.size(),
               inserted,
               unreachable);
}
