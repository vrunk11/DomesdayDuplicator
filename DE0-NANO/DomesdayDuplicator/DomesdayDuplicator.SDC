#************************************************************************
#	
#	DomesdayDuplicator.SDC
#	SDC file
#	
#	Domesday Duplicator - LaserDisc RF sampler
#	Copyright (C) 2018 Simon Inns
#	
#	This file is part of Domesday Duplicator.
#	
#	Domesday Duplicator is free software: you can redistribute it and/or
#	modify it under the terms of the GNU General Public License as
#	published by the Free Software Foundation, either version 3 of the
#	License, or (at your option) any later version.
#	
#	This program is distributed in the hope that it will be useful,
#	but WITHOUT ANY WARRANTY; without even the implied warranty of
#	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#	GNU General Public License for more details.
#	
#	You should have received a copy of the GNU General Public License
#	along with this program.  If not, see <http://www.gnu.org/licenses/>.
#	Email: simon.inns@gmail.com
#	
#************************************************************************/


# Create 50MHz system Clock
create_clock -period 20.000ns [get_ports CLOCK_50]


# Create Generated Clocks
derive_pll_clocks
derive_clock_uncertainty

# ----------------------------------------------------------------------
# ADS828 revision (75MHz adc_clock / 100MHz fx3_clock): ADC capture path
# ----------------------------------------------------------------------
# adc_databus (GPIO0[23:32]) was never previously constrained relative to
# adc_clock, so TimeQuest never actually checked this capture path - it
# just happened to work at 40MSPS (25ns period) because the ADS825's
# clock-to-data-valid delay fit comfortably in that period even treated
# as ~0ns by default. At 75MSPS the period shrinks to 13.33ns (tCONV min
# per the ADS828 datasheet is itself 13.3ns, i.e. 75MSPS is right at the
# part's rated max - zero margin on the ADC side), so this path needs a
# real set_input_delay.
#
# Values below are from the ADS828 datasheet (SBAS126A), timing table
# page 4, referenced to the rising edge of CLK, CL = 15pF max:
#   -max 12.0ns : t2, "New Data Delay Time" (tOD) max
#   -min  3.9ns : t1, "Data Hold Time" (CL = 0pF)
# These do not yet include any board trace skew between GPIO0[33]
# (adc_clock out to the ADC) and GPIO0[23:32] (data back in) - add that
# on top if/when trace lengths are known to differ meaningfully.
set_input_delay -clock { IPpllGenerator0|altpll_component|auto_generated|pll1|clk[1] } -max 12.0 [get_ports {GPIO0[23] GPIO0[24] GPIO0[25] GPIO0[26] GPIO0[27] GPIO0[28] GPIO0[29] GPIO0[30] GPIO0[31] GPIO0[32]}]
set_input_delay -clock { IPpllGenerator0|altpll_component|auto_generated|pll1|clk[1] } -min 3.9 [get_ports {GPIO0[23] GPIO0[24] GPIO0[25] GPIO0[26] GPIO0[27] GPIO0[28] GPIO0[29] GPIO0[30] GPIO0[31] GPIO0[32]}]

# NOTE: the FX3-facing bus (fx3_databus/fx3_control, clocked out on
# fx3_clock = GPIO1[31]) is likewise unconstrained here. It was raised
# from 60 to 100MHz in this revision to keep up with the higher ADC rate
# (see DomesdayDuplicator.v) - before relying on that in hardware, add
# set_output_delay constraints derived from the CYUSB3014 GPIF-II
# synchronous slave-FIFO AC timing spec (setup/hold at the FX3 pins) and
# confirm the actual clk[0]/clk[1] names TimeQuest assigns after a real
# compile (the path above is the standard Quartus/ALTPLL naming pattern
# but should be verified against the Timing Analyzer's clock list).
