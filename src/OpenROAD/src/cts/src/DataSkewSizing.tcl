# Opt-in clock-target realization stage following native CTS construction.
# Explicitly source this file; stock clock_tree_synthesis remains unchanged.
# reference: dict clock-pin -> {baseline_min_ns baseline_max_ns signed_delta_ns}.
# Placement estimates are a screen, not routed gate-skew or leakage acceptance.
namespace eval cts_data_skew {}

proc cts_data_skew::arrival_ns {vertex rf mode} {
  set path [sta::vertex_worst_arrival_path_rf $vertex $rf $mode]
  if {$path eq "NULL"} {error "Missing $mode/$rf arrival"}
  return [expr {[$path arrival]/1e-9}]
}

proc cts_data_skew::measure {reference {gates {}}} {
  # Property float values are formatted to six digits by OpenSTA. Read numeric
  # path arrivals instead: rounding is material at the 0.01 ps comparison cap.
  sta::worst_slack -max
  set vertices [dict create]
  set iterator [sta::vertex_iterator]
  while {[$iterator has_next]} {
    set vertex [$iterator next]
    dict set vertices [get_full_name [$vertex pin]] $vertex
  }
  $iterator finish
  set score 0.0
  set largest_shift 0.0
  set clocks [dict create]
  dict for {name values} $reference {
    lassign $values lo hi delta
    set pin [sta::get_port_pin_error clock_pin $name]
    set amin [cts_data_skew::arrival_ns [dict get $vertices $name] rise min]
    set amax [cts_data_skew::arrival_ns [dict get $vertices $name] rise max]
    foreach actual [list $amin $amax] baseline [list $lo $hi] {
      set shift [expr {$actual - $baseline}]
      set score [expr {$score + abs($shift - $delta)/2.0}]
      set largest_shift [expr {max($largest_shift, abs($shift))}]
    }
    dict set clocks $name [list $amin $amax]
  }
  set area 0.0
  set clock_area 0.0
  set block [ord::get_db_block]
  set units [$block getDbUnitsPerMicron]
  foreach inst [$block getInsts] {
    set master [$inst getMaster]
    set a [expr {double([$master getWidth])*[$master getHeight]/($units*$units)}]
    set area [expr {$area + $a}]
    foreach term [$inst getITerms] {
      if {[$term isOutputSignal] && [$term getNet] ne "NULL" && [[$term getNet] getSigType] eq "CLOCK"} {
        set clock_area [expr {$clock_area + $a}]
        break
      }
    }
  }
  sta::redirect_string_begin
  set report_code [catch {
    report_check_types -max_slew -max_capacitance -max_fanout -violators
  } report_detail report_options]
  set rules [sta::redirect_string_end]
  if {$report_code} {return -options $report_options $report_detail}
  set gate_score 0.0
  set widths [dict create]
  dict for {gate spec} $gates {
    lassign $spec weight pins
    set width 0.0
    # Include opposite transitions on different inputs, too. This union of
    # rise/fall min/max envelopes is conservative, not a sensitization proof.
    set lower Inf
    set upper -Inf
    foreach rf {rise fall} {
      foreach name $pins {
        set pin [sta::get_port_pin_error data_pin $name]
        set lo [cts_data_skew::arrival_ns [dict get $vertices $name] $rf min]
        set hi [cts_data_skew::arrival_ns [dict get $vertices $name] $rf max]
        if {![expr {abs($lo) < Inf && abs($hi) < Inf}]} {error "Unconstrained marked gate: $name"}
        set lower [expr {min($lower, $lo)}]
        set upper [expr {max($upper, $hi)}]
      }
    }
    set width [expr {$upper - $lower}]
    dict set widths $gate $width
    set gate_score [expr {$gate_score + $weight*$width}]
  }
  return [dict create score_ns $score max_shift_ns $largest_shift clocks $clocks \
    setup_ns [sta::worst_slack -max] hold_ns [sta::worst_slack -min] \
    area_um2 $area clock_area_um2 $clock_area electrical_pass [expr {[string trim $rules] eq ""}] electrical_report $rules \
    gate_score_ns $gate_score gate_widths_ns $widths]
}

proc cts_data_skew::legal {metrics baseline margin bound} {
  expr {[dict get $metrics setup_ns] >= $margin && [dict get $metrics hold_ns] >= $margin \
    && [dict get $metrics max_shift_ns] <= $bound \
    && [dict get $metrics area_um2] <= 1.10*[dict get $baseline area_um2] \
    && [dict get $metrics clock_area_um2] <= 1.50*[dict get $baseline clock_area_um2] \
    && [dict get $metrics electrical_pass]}
}

proc cts_data_skew::size_leaves {reference cells {rounds 2} {margin 0.01} {bound 0.04} {gates {}} {pair_budget 64}} {
  if {![string is integer -strict $rounds] || $rounds < 1 || $rounds > 10} {error "Invalid round budget"}
  if {![string is integer -strict $pair_budget] || $pair_budget < 0 || $pair_budget > 256} {error "Invalid joint trial budget"}
  if {![string is double -strict $margin] || $margin < 0 || ![string is double -strict $bound] || $bound <= 0 || $bound > 0.04} {error "Invalid timing budgets"}
  set names [lsort [lmap pin [all_registers -clock_pins] {get_full_name $pin}]]
  if {[llength [all_clocks]] != 1 || [get_property [lindex [all_clocks] 0] is_generated] || [llength [all_registers -cells -level_sensitive]] || [llength [all_registers -async_pins]]} {error "Only single-clock edge-triggered synchronous circuits supported"}
  if {$names ne [lsort [dict keys $reference]]} {error "Complete register clock coverage required"}
  set block [ord::get_db_block]
  set drivers [dict create]
  dict for {name values} $reference {
    if {[llength $values] != 3} {error "Invalid reference row"}
    foreach value $values {
      if {![string is double -strict $value] || [catch {expr {abs($value) < Inf}} finite] || !$finite} {error "Nonfinite reference"}
    }
    lassign $values lo hi delta
    if {$lo > $hi || abs($delta) > $bound} {error "Invalid reference interval/target"}
    set term [$block findITerm $name]
    if {$term eq "NULL" || [$term getNet] eq "NULL"} {error "Disconnected clock pin"}
    set driver [[$term getNet] getFirstOutput]
    if {$driver eq "NULL"} {error "Missing clock driver"}
    dict set drivers [[$driver getInst] getName] 1
  }
  # Use only cells explicitly characterized/allowed by the calling CTS flow.
  if {[llength $cells] == 0} {error "Empty candidate cell list"}
  foreach cell $cells {
    set libcell [get_lib_cells $cell]
    if {[llength $libcell] != 1 || ![$libcell is_buffer]} {error "Candidate must be a noninverting buffer: $cell"}
  }
  dict for {gate spec} $gates {
    lassign $spec weight pins
    if {[llength $spec] != 2 || ![string is double -strict $weight] || ![expr {$weight > 0 && $weight < Inf}] || [llength $pins] < 2 || [llength $pins] != [llength [lsort -unique $pins]]} {error "Invalid marked gate specification"}
    foreach name $pins {
      set pin [sta::get_port_pin_error data_pin $name]
      if {[get_full_name [$pin instance]] ne $gate || [get_property $pin direction] ne "input"} {error "Marked gate pin mismatch"}
    }
  }
  set objective score_ns
  if {[dict size $gates]} {set objective gate_score_ns}
  estimate_parasitics -placement
  set baseline [measure $reference $gates]
  if {![legal $baseline $baseline $margin $bound]} {
    dict unset baseline clocks
    error "Initial tree fails sizing safety checks: $baseline"
  }
  set current $baseline
  set history {}
  set selected {}
  set pairs_tried 0
  # Each trial measures ALL sinks and full-design setup/hold, including changes
  # in sibling load and upstream slew. No independent per-FF delay assumption.
  for {set round 0} {$round < $rounds} {incr round} {
    set accepted 0
    foreach driver [lsort [dict keys $drivers]] {
      set inst [$block findInst $driver]
      set original [[$inst getMaster] getName]
      set winner $original
      set best $current
      foreach cell $cells {
        if {$cell eq $original} {continue}
        set code [catch {
          replace_cell $driver $cell
          estimate_parasitics -placement
          set measured [measure $reference $gates]
        } detail options]
        # Always roll back the trial, including errors; remeasure after rollback.
        replace_cell $driver $original
        estimate_parasitics -placement
        if {$code} {return -options $options $detail}
        set pass [legal $measured $baseline $margin $bound]
        lappend history [dict create round $round driver $driver cell $cell legal $pass metrics $measured]
        if {$pass && [dict get $measured $objective] < [dict get $best $objective] - 1e-6} {
          set winner $cell
          set best $measured
        }
      }
      if {$winner ne $original} {
        replace_cell $driver $winner
        estimate_parasitics -placement
        set current [measure $reference $gates]
        if {![legal $current $baseline $margin $bound] || abs([dict get $current $objective] - [dict get $best $objective]) > 1e-6} {
          replace_cell $driver $original
          estimate_parasitics -placement
          error "Accepted sizing trial did not reproduce"
        }
        lappend selected [list $driver $original $winner]
        incr accepted
      }
    }
    # If no single edit helps, allow a bounded pair move: neither component
    # needs to improve alone. This remains a local discrete search, not an LP
    # guarantee or exhaustive optimization on a large circuit.
    if {$accepted == 0 && [dict size $gates] && $pairs_tried < $pair_budget} {
      set driver_names [lsort [dict keys $drivers]]
      set pair_winner {}
      set pair_best $current
      for {set i 0} {$i < [llength $driver_names] && $pairs_tried < $pair_budget} {incr i} {
        set a [lindex $driver_names $i]
        set old_a [[[$block findInst $a] getMaster] getName]
        for {set j [expr {$i+1}]} {$j < [llength $driver_names] && $pairs_tried < $pair_budget} {incr j} {
          set b [lindex $driver_names $j]
          set old_b [[[$block findInst $b] getMaster] getName]
          foreach ca $cells {
            if {$ca eq $old_a || $pairs_tried >= $pair_budget} {continue}
            foreach cb $cells {
              if {$cb eq $old_b || $pairs_tried >= $pair_budget} {continue}
              incr pairs_tried
              set code [catch {
                replace_cell $a $ca
                replace_cell $b $cb
                estimate_parasitics -placement
                set measured [measure $reference $gates]
              } detail options]
              replace_cell $a $old_a
              replace_cell $b $old_b
              estimate_parasitics -placement
              if {$code} {return -options $options $detail}
              set pass [legal $measured $baseline $margin $bound]
              lappend history [dict create round $round pair [list [list $a $ca] [list $b $cb]] legal $pass metrics $measured]
              if {$pass && [dict get $measured $objective] < [dict get $pair_best $objective] - 1e-6} {
                set pair_best $measured
                set pair_winner [list [list $a $old_a $ca] [list $b $old_b $cb]]
              }
            }
          }
        }
      }
      if {[llength $pair_winner]} {
        foreach edit $pair_winner {lassign $edit driver old cell; replace_cell $driver $cell}
        estimate_parasitics -placement
        set current [measure $reference $gates]
        if {![legal $current $baseline $margin $bound] || abs([dict get $current $objective] - [dict get $pair_best $objective]) > 1e-6} {
          foreach edit $pair_winner {lassign $edit driver old cell; replace_cell $driver $old}
          estimate_parasitics -placement
          error "Joint sizing trial did not reproduce"
        }
        foreach edit $pair_winner {lappend selected $edit}
        set accepted 2
      }
    }
    puts "CTS_DATA_SKEW_SIZING round=$round accepted=$accepted objective=$objective value=[dict get $current $objective]"
    if {$accepted == 0} {break}
  }
  return [dict create status PLACEMENT_TARGET_SCREEN_ONLY baseline $baseline final $current \
    selected $selected trials $history objective $objective pairs_tried $pairs_tried final_optimizer 0]
}
