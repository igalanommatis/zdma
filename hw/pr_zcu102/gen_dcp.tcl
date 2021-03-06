set top [string trim [get_cells *_i] "*_i"]

set modules [list [get_cells -hierarchical  zcore32_?] [get_cells -hierarchical  zcore32_??]]

foreach module $modules { update_design -cell $module -black_box }

write_checkpoint -force dcp/base/$top

