# SPDX-License-Identifier: BSD-3-Clause
# Complete target-driven branch construction after clock-cell legalization.
# This entry dispatches to construction, never to the gate-score edit selector.
proc construct_cts_data_alignment {args} {
  sta::parse_key_args construct_cts_data_alignment args keys {-buf_list} flags {}
  sta::check_argc_eq0 construct_cts_data_alignment $args
  if {![info exists keys(-buf_list)] || ![llength $keys(-buf_list)]} {
    error "construct_cts_data_alignment requires -buf_list"
  }
  set block [ord::get_db_block]
  set cost [odb::dbIntProperty_find $block cts_cost_aware_tree]
  if {$cost ne "NULL" && [$cost getValue] != 0} {
    error "Cost-aware construction already builds shared branches; do not append the old leaf fitter"
  }
  set alignment [odb::dbIntProperty_find $block cts_data_alignment]
  set optimizer [odb::dbIntProperty_find $block cts_data_skew_native_sizing]
  if {$alignment eq "NULL" || [$alignment getValue] != 1
      || ($optimizer ne "NULL" && [$optimizer getValue] != 0)} {
    error "Enable data alignment and disable the final optimizer first"
  }
  set realized [odb::dbIntProperty_find $block cts_align_realized]
  if {$realized ne "NULL" && [$realized getValue] != 0} {
    error "Rebuild the clock tree from the frozen baseline before another construction pass"
  }
  set flag [odb::dbIntProperty_find $block cts_align_realize]
  if {$flag eq "NULL"} {set flag [odb::dbIntProperty_create $block cts_align_realize 0]}
  $flag setValue 1
  try {
    cts::optimize_data_skew [join $keys(-buf_list) " "]
    if {$realized eq "NULL"} {odb::dbIntProperty_create $block cts_align_realized 1} else {$realized setValue 1}
  } finally {
    $flag setValue 0
  }
}
