#!/opt/Xilinx/Vivado_HLS/2017.2/bin/vivado_hls
#set cores [list gauss sobel emboss outline sharpen contrast threshold loopback negative]
set cores [list sobel]
set sizes [list 16]

foreach core $cores {
	foreach size $sizes {
		set fd [open "generated.h" "w"]
		puts $fd "#pragma once\n#define FORCE_DSP48\n#define CORE_NAME zcore${size}\ntypedef uint${size}_t axi_data_t;"
		close $fd
		open_project -reset $core
		set_top zcore$size
		add_files ${core}.cpp
		add_files -tb testbench.cpp
		add_files -tb sample.jpg
		add_files -tb csim/build/out.jpg
		
		open_solution -reset "solution_${size}"
		set_part {xc7z020clg484-1}
		#set_part {xczu9eg-ffvb1156-2-i}
		create_clock -period 7.5 -name default

		csynth_design
		export_design -rtl verilog -format ip_catalog	\
			-description "ZDMA Core $core/${size}"	\
			-vendor "tuc"				\
			-version "3.7"				\
			-display_name "ZDMA Core $core/${size}"

		close_project
	}
}
exit

