# Opt-in bounded per-register clock delay stage. Requires DataSkewSizing.tcl.
# Scores the actual marked gate input envelopes after each physical buffer edit.
# Placement STA screens proposals; routed timing/graph/activity decide acceptance.
namespace eval cts_data_skew {variable delay_serial 0}

proc cts_data_skew::delay_signature {reference} {
  set block [ord::get_db_block]
  set nets [dict create]
  foreach name [dict keys $reference] {
    set term [$block findITerm $name]
    if {$term eq "NULL" || [$term getNet] eq "NULL"} {error "Disconnected clock pin: $name"}
    dict set nets $name [[$term getNet] getName]
  }
  return [list [llength [$block getInsts]] [llength [$block getNets]] $nets]
}

proc cts_data_skew::add_delay {pin cell} {
  variable delay_serial
  set block [ord::get_db_block]
  set term [$block findITerm $pin]
  if {$term eq "NULL"} {error "Unknown delay pin"}
  set p [sta::get_port_pin_error clock_pin $pin]
  if {![get_property $p is_register_clock]} {error "Delay requires a register clock pin"}
  while {1} {
    set name "cts_skew_delay_$delay_serial"
    set net "cts_skew_delay_net_$delay_serial"
    incr delay_serial
    if {[$block findInst $name] eq "NULL" && [$block findNet $net] eq "NULL"} {break}
  }
  lassign [[$term getInst] getLocation] x y
  set units [$block getDbUnitsPerMicron]
  set buffer [insert_buffer -buffer_cell $cell -load_pins [list $pin] \
    -buffer_name $name -net_name $net \
    -location [list [expr {double($x)/$units}] [expr {double($y)/$units}]]]
  # Resizer uniquifies the requested base name even when it is unused. Use its
  # returned instance identity, never infer the actual name from the base.
  set actual [get_full_name $buffer]
  if {[$block findInst $actual] eq "NULL"} {error "Delay instance was not created"}
  if {[[$term getNet] getSigType] ne "CLOCK"} {
    remove_buffers [list $actual]
    error "Inserted branch lost clock net type"
  }
  return $actual
}

proc cts_data_skew::undo_delays {names signature reference} {
  set block [ord::get_db_block]
  # Reject an empty/missing selection: remove_buffers with no cells removes ALL.
  foreach name [lreverse $names] {
    if {[$block findInst $name] eq "NULL"} {error "Rollback instance missing: $name"}
    remove_buffers [list $name]
    if {[$block findInst $name] ne "NULL"} {error "Clock delay rollback failed: $name"}
  }
  estimate_parasitics -placement
  set after [delay_signature $reference]
  if {$after ne $signature} {error "Clock delay rollback changed topology: before=$signature after=$after"}
}

proc cts_data_skew::delay_branches {reference gates eligible cells {max_added 2} {trial_budget 128} {pair_budget 32} {margin 0.01} {bound 0.04}} {
  if {![llength [info commands ::cts_data_skew::measure]]} {error "Source DataSkewSizing.tcl first"}
  foreach value [list $max_added $trial_budget $pair_budget] {
    if {![string is integer -strict $value] || $value < 0} {error "Invalid delay search budget"}
  }
  if {$max_added > 8 || $trial_budget > 512 || $pair_budget > 128} {error "Delay search budget exceeds limits"}
  if {![string is double -strict $margin] || ![expr {$margin >= 0 && $margin < Inf}] || ![string is double -strict $bound] || ![expr {$bound > 0 && $bound <= 0.04}]} {error "Invalid delay timing budgets"}
  if {[llength [all_clocks]] != 1 || [get_property [lindex [all_clocks] 0] is_generated] || [llength [all_registers -cells -level_sensitive]] || [llength [all_registers -async_pins]]} {error "Unsupported sequential/clock topology"}
  set names [lsort [lmap pin [all_registers -clock_pins] {get_full_name $pin}]]
  if {$names ne [lsort [dict keys $reference]]} {error "Complete register clock coverage required"}
  dict for {name values} $reference {
    if {[llength $values] != 3} {error "Invalid delay reference row"}
    foreach value $values {
      if {![string is double -strict $value] || [catch {expr {abs($value) < Inf}} finite] || !$finite} {error "Nonfinite delay reference"}
    }
    lassign $values lo hi delta
    if {$lo > $hi || abs($delta) > $bound} {error "Invalid delay reference interval/target"}
  }
  if {![dict size $gates]} {error "Marked gates required for delay scoring"}
  dict for {gate spec} $gates {
    lassign $spec weight pins
    if {[llength $spec] != 2 || ![string is double -strict $weight] || ![expr {$weight > 0 && $weight < Inf}] || [llength $pins] < 2 || [llength $pins] != [llength [lsort -unique $pins]]} {error "Invalid delay gate specification"}
    foreach name $pins {
      set p [sta::get_port_pin_error data_pin $name]
      if {[get_full_name [$p instance]] ne $gate || [get_property $p direction] ne "input"} {error "Marked delay gate pin mismatch"}
    }
  }
  if {![llength $eligible] || [llength $eligible] != [llength [lsort -unique $eligible]]} {error "Invalid eligible delay pins"}
  set block [ord::get_db_block]
  foreach pin $eligible {
    if {![dict exists $reference $pin]} {error "Unknown eligible delay clock pin"}
    set term [$block findITerm $pin]
    if {$term eq "NULL" || [$term getNet] eq "NULL"} {error "Disconnected eligible delay clock pin"}
    if {[[$term getNet] isDoNotTouch] || [[$term getInst] isDoNotTouch]} {error "Do-not-touch delay endpoint"}
  }
  if {![llength $cells] || [llength $cells] != [llength [lsort -unique $cells]]} {error "Invalid delay cell list"}
  foreach cell $cells {
    set lib [get_lib_cells $cell]
    if {[llength $lib] != 1 || ![$lib is_buffer]} {error "Delay candidate must be a noninverting buffer"}
  }
  estimate_parasitics -placement
  set baseline [measure $reference $gates]
  if {![legal $baseline $baseline $margin $bound]} {error "Initial delay tree fails safety checks"}
  set current $baseline
  set trials {}
  set selected {}
  set inserted {}
  set tried 0
  set pairs 0
  while {[llength $selected] < $max_added && $tried < $trial_budget} {
    set winner {}
    set best $current
    set remaining [lsort $eligible]
    foreach edit $selected {set remaining [lsearch -all -inline -not -exact $remaining [lindex $edit 1]]}
    set signature [delay_signature $reference]
    foreach pin $remaining {
      foreach cell $cells {
        if {$tried >= $trial_budget} {break}
        incr tried
        set created {}
        set code [catch {
          lappend created [add_delay $pin $cell]
          estimate_parasitics -placement
          set m [measure $reference $gates]
        } detail options]
        undo_delays $created $signature $reference
        if {$code} {return -options $options $detail}
        set pass [legal $m $baseline $margin $bound]
        lappend trials [dict create plan [list [list insert $pin $cell]] legal $pass metrics $m]
        if {$pass && [dict get $m gate_score_ns] < [dict get $best gate_score_ns] - 1e-6} {
          set best $m
          set winner [list [list insert $pin $cell]]
        }
      }
    }
    # Search simultaneous insertions only when single moves stall. Total trials
    # and added cells are capped, and no pin gets more than one inserted stage.
    if {![llength $winner] && [llength $selected]+2 <= $max_added} {
      for {set i 0} {$i < [llength $remaining] && $pairs < $pair_budget && $tried < $trial_budget} {incr i} {
        set a [lindex $remaining $i]
        for {set j [expr {$i+1}]} {$j < [llength $remaining] && $pairs < $pair_budget && $tried < $trial_budget} {incr j} {
          set b [lindex $remaining $j]
          foreach ca $cells {
            foreach cb $cells {
              if {$pairs >= $pair_budget || $tried >= $trial_budget} {break}
              incr pairs
              incr tried
              set created {}
              set code [catch {
                lappend created [add_delay $a $ca]
                lappend created [add_delay $b $cb]
                estimate_parasitics -placement
                set m [measure $reference $gates]
              } detail options]
              undo_delays $created $signature $reference
              if {$code} {return -options $options $detail}
              set pass [legal $m $baseline $margin $bound]
              set plan [list [list insert $a $ca] [list insert $b $cb]]
              lappend trials [dict create plan $plan legal $pass metrics $m]
              if {$pass && [dict get $m gate_score_ns] < [dict get $best gate_score_ns] - 1e-6} {set best $m; set winner $plan}
            }
          }
        }
      }
    }
    if {![llength $winner]} {break}
    set created {}
    foreach edit $winner {lassign $edit kind pin cell; lappend created [add_delay $pin $cell]}
    estimate_parasitics -placement
    set current [measure $reference $gates]
    if {![legal $current $baseline $margin $bound] || abs([dict get $current gate_score_ns] - [dict get $best gate_score_ns]) > 1e-6} {
      undo_delays $created $signature $reference
      error "Accepted delay insertion did not reproduce"
    }
    foreach edit $winner {lappend selected $edit}
    foreach name $created {lappend inserted $name}
    puts "CTS_DATA_SKEW_DELAY added=[llength $selected] gate_score_ns=[dict get $current gate_score_ns]"
  }
  return [dict create status PLACEMENT_GATE_SCREEN_ONLY baseline $baseline final $current \
    selected $selected inserted_instances $inserted trials $trials trials_used $tried pairs_tried $pairs \
    budgets [list $max_added $trial_budget $pair_budget $margin $bound] final_optimizer 0]
}
