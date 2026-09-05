#************************************************************************
#
#    DomesdayDuplicator.SDC
#
#    Timing constraints
#    Domesday Duplicator - LaserDisc RF sampler
#    SPDX-FileCopyrightText: 2018-2026 Simon Inns
#    SPDX-License-Identifier: GPL-3.0-or-later
#
#************************************************************************
#
# Until 2026 this file was three lines: the 50 MHz input clock and the two
# derive_* commands. That constrained the logic inside the FPGA and nothing at
# its pins, which was survivable while the design's own clocks were the only
# things it had to get right and the FX3 link ran at 60 MHz. Moving to one
# 80 MHz clock makes the pins the interesting part: the FX3 interface is the
# fastest thing on the board, and the ADC's sampling clock is now something
# this design generates in the fabric rather than something the PLL hands it.
#
# READ THIS BEFORE TRUSTING A TIMING REPORT
#
# The clock definitions below are facts about the design and are exact. The
# I/O delays are not: they are **deliberately pessimistic bounds**, not
# datasheet values, because the ADS825, 74LVTH541 and CYUSB3014 datasheets
# were not to hand when this file was written.
#
# They are chosen so that the error is in the safe direction. Every external
# delay is set larger than any plausible real value, and every setup and hold
# requirement is set tighter than the published one. So:
#
#   - if timing closes with these numbers, it closes with the real ones, and
#     the result can be believed;
#   - if it does not close, the numbers are the first thing to check, not the
#     design. Replace the values in the block below with the datasheet ones
#     and re-run before changing any logic.
#
# Each value says what it stands for and what to look up. Nothing else in the
# file needs editing to correct them.
#
#************************************************************************


# ---------------------------------------------------------------------------
# Values to confirm against the datasheets
# ---------------------------------------------------------------------------

# The altpll output pin that carries the 80 MHz system clock. This is the
# conventional Quartus path for an altpll instance named pll_generator_0; if a
# future Quartus renames it, every reference below comes from here.
set pll_clock_pin {pll_generator_0|altpll_component|auto_generated|pll1|clk[0]}

# FX3 GPIF II, 16-bit synchronous slave. The FPGA drives PCLK, so these are
# what the FX3 requires of data this design presents to it. CYUSB3014
# publishes about 2 ns setup and 0.5 ns hold; the values here are tighter, so
# meeting them means meeting the real ones.
set fx3_setup 2.5
set fx3_hold 1.0

# What the FX3 does with its own outputs, relative to the same PCLK edge -
# readData is the only one this design times. A clock-to-out window of 1 to
# 8 ns is wider than the part is specified for, which makes the constraint
# harder to meet rather than easier.
set fx3_ctl_max 8.0
set fx3_ctl_min 1.0

# The ADC data path, replaced with real numbers for the ADS828 (Texas
# Instruments SBAS126A) now that this design targets that part rather than
# the ADS825 the placeholder above was guessing at.
#
# Only ONE 74LVTH541 buffer is in this path, not two: the hardware guide
# ("ADC" section) explains the board needs two buffer ICs because a 10-bit bus
# does not fit in one octal buffer's 8 channels, and places them in PARALLEL -
# each bit crosses exactly one of the two chips, never both. Treating this as
# two buffers in series would have doubled a real number into a wrong one.
#
# adc_data_max: 12 ns (ADS828 t2, "New Data Delay Time", SBAS126A pg. 2) +
# 3.5 ns (SN74LVTH541 tPLH/tPHL max at VCC = 3.3 V +-0.3 V). Board trace delay
# is not added on top: the hardware guide places the ADC "as close as
# possible" to the FPGA header specifically to keep this short, and nothing
# here has a real number for it - if timing closure ever comes down to a
# fraction of a ns, that assumption is the first thing to revisit.
#
# adc_data_min: the ADS828's own t1 ("Data Hold Time") alone, deliberately
# not adding the buffer's minimum propagation delay on top - the smaller
# number is the more conservative one for a -min bound (it assumes data could
# become invalid sooner than the full chain really allows), consistent with
# this file's rule of erring the safe direction.
set adc_data_max 15.5
set adc_data_min 3.9


# ---------------------------------------------------------------------------
# Clocks
# ---------------------------------------------------------------------------

# The DE0-Nano's 50 MHz oscillator, the only clock that enters the FPGA
create_clock -name CLOCK_50 -period 20.000 [get_ports CLOCK_50]

# The 80 MHz system clock out of the PLL. Everything in the design runs from
# it, so this one command covers every internal path.
derive_pll_clocks

# PCLK to the FX3, at Fx3DataWidth == 32: the same divide-by-two register as
# adc_clock, not the system clock driven straight onto the pin. The wide bus
# gives up the FX3 running faster than the sampling rate - that headroom now
# comes from halving the word rate instead - so the interface clock is 1:1
# with adc_clock, on the same register, for the same reason adc_clock is
# sourced from the PLL rather than from its own pin two entries below: that
# register is duplicated by the fitter, once per pin it drives, and naming
# either duplicate as a source fails.
#
# TODO once hardware bring-up starts: GPIO0[33] (adc_clock) carries a
# FAST_OUTPUT_REGISTER assignment in the .qsf for a clean edge at the pin;
# GPIO1[31] (this clock) does not. Both are driven by the same register now,
# and PCLK reaching the FX3 with more jitter than adc_clock reaching the ADC
# is worth checking for in the fitter report before trusting this on real
# hardware. If this proves it matters, GPIO1[31] needs the same assignment.
create_generated_clock -name fx3_pclk \
    -source [get_pins $pll_clock_pin] \
    -divide_by 2 \
    [get_ports {GPIO1[31]}]

# The ADC's 40 MHz sampling clock: the divide-by-two register's output, driven
# onto a pin.
#
# Sourced from the PLL rather than from the divider's own clock pin, which is
# what this said first and which Quartus rejected outright: "Argument -source
# is a collection with more than one object". The FAST_OUTPUT_REGISTER
# assignment on this pin makes the fitter duplicate the divider, one copy in
# the I/O cell and one in the fabric driving sample_enable, so the register
# name matches two pins and is not a usable source. The .qsf predicted the
# duplication; this file did not account for it.
#
# It failed loudly, but everything downstream of it failed quietly: with no
# adc_clock defined, the two set_input_delay commands and both multicycle
# exceptions below were dropped as well, and the ADC databus went unanalysed.
create_generated_clock -name adc_clock \
    -source [get_pins $pll_clock_pin] \
    -divide_by 2 \
    [get_ports {GPIO0[33]}]

# The remote update block's clock, a divide-by-four of the system clock made in
# the fabric by remoteUpdate. It is divided because the block is specified for a
# 25 ns minimum period and 10.1 ns minimum high and low times on this device, so
# the 80 MHz system clock violates it by a factor of two; Quartus checks this,
# and without the constraint below it would check the divided clock against the
# system clock's pulse widths and report a violation that is not there.
create_generated_clock -name update_clock \
    -source [get_pins $pll_clock_pin] \
    -divide_by 4 \
    [get_registers {*update_clock}]

derive_clock_uncertainty


# ---------------------------------------------------------------------------
# Port groups
# ---------------------------------------------------------------------------

# Everything the FX3 samples on a PCLK edge: the 16-bit databus, and the two
# flags it reads alongside it.
set fx3_synchronous_outputs [get_ports { \
    GPIO1[2]  GPIO1[4]  GPIO1[6]  GPIO1[8]  GPIO1[10] GPIO1[12] GPIO1[14] \
    GPIO1[16] GPIO1[18] GPIO1[20] GPIO1[22] GPIO1[24] GPIO1[26] GPIO1[28] \
    GPIO1[30] GPIO1[32] \
    GPIO1[27] \
    GPIO1[21] \
    GPIO0[2]  GPIO0[3]  GPIO0[4]  GPIO0[5]  GPIO0[6]  GPIO0[7] \
    GPIO0[12] GPIO0[13] GPIO0[14] GPIO0[15] GPIO0[16] GPIO0[17] \
    GPIO0[18] GPIO0[19] GPIO0[20] GPIO0[21] }]

# The GPIO0 half of the databus is only driven at Fx3DataWidth == 32; at 16
# these pins are tri-stated (see DomesdayDuplicator.v) and this group simply
# has nothing to say about them. Quartus does not mind a port group naming a
# pin with no active driver in a given build - the analysis for it is a
# constant, not an error - so one SDC file covers both widths without a
# generate of its own.

# readData, the one FX3 output this design captures on a clock edge
set fx3_synchronous_inputs [get_ports {GPIO1[25]}]

# The 10-bit ADC databus
set adc_data_inputs [get_ports { \
    GPIO0[23] GPIO0[24] GPIO0[25] GPIO0[26] GPIO0[27] \
    GPIO0[28] GPIO0[29] GPIO0[30] GPIO0[31] GPIO0[32] }]


# ---------------------------------------------------------------------------
# The FX3 interface
# ---------------------------------------------------------------------------
#
# Source-synchronous, with this design as the source: it drives PCLK and the
# data together, and the FX3 samples both. The setup requirement becomes the
# maximum output delay and the hold requirement the minimum, negated.
#
# This is the fastest interface on the board and the one the move to 80 MHz
# puts most pressure on, so it is the report to read first.

set_output_delay -clock fx3_pclk -max $fx3_setup $fx3_synchronous_outputs
set_output_delay -clock fx3_pclk -min [expr {0 - $fx3_hold}] $fx3_synchronous_outputs

set_input_delay -clock fx3_pclk -max $fx3_ctl_max $fx3_synchronous_inputs
set_input_delay -clock fx3_pclk -min $fx3_ctl_min $fx3_synchronous_inputs


# ---------------------------------------------------------------------------
# The ADC interface
# ---------------------------------------------------------------------------
#
# The FPGA drives the ADC's clock and reads the result back, so this is
# source-synchronous with the FPGA as the source of the clock and the ADC as
# the source of the data.

set_input_delay -clock adc_clock -max $adc_data_max $adc_data_inputs
set_input_delay -clock adc_clock -min $adc_data_min $adc_data_inputs

# The sample the ADC launches on one of its clock edges is captured one and a
# half ADC periods later, not one: halfBandDecimator's first stage is fed
# sample_enable_delayed, a further one-cycle register delay of sample_enable,
# specifically because one ADC period (13.33 ns at 75 MHz) is shorter than
# the ADC's own real settling time plus its output buffer's (about 15.5 ns,
# ADS828 + SN74LVTH541 datasheets) - the fix moves the capture point later,
# into the window the ADS828's own data-hold spec guarantees is still valid,
# rather than trying to make the ADC or the buffer faster.
#
# Three system-clock periods, not two: this is the whole reason the capture
# was moved a cycle later in the first place, and the SDC has to say so or it
# would go on checking the old, too-tight relationship regardless of what the
# RTL actually does now. The hold multicycle is one less than the setup
# multicycle, which is what keeps the hold check against the same edge it was
# against before the exception.
set_multicycle_path -setup 3 -from [get_clocks adc_clock] -to [get_clocks $pll_clock_pin]
set_multicycle_path -hold 2 -from [get_clocks adc_clock] -to [get_clocks $pll_clock_pin]

# The other direction of the same relationship, for fx3_pclk: the registers
# driving the FX3-facing outputs (buffer.v, fx3StateMachine.v) are clocked by
# system_clock and only change once every second cycle now (transfer_enable in
# DomesdayDuplicator.v), which is exactly what makes fx3_pclk a real 2:1
# divide-by-two of that clock rather than a one-to-one copy of it - see the
# comment above fx3_pclk's create_generated_clock. Without this, TimeQuest has
# no reason to believe a launch on system_clock has two of its own periods to
# reach a pin it only expects a new value at every other one of, and analyses
# every path here at half the time it actually has.
set_multicycle_path -setup 2 -from [get_clocks $pll_clock_pin] -to [get_clocks fx3_pclk]
set_multicycle_path -hold 1 -from [get_clocks $pll_clock_pin] -to [get_clocks fx3_pclk]

# And the input direction of the same 2:1 relationship: readData (GPIO1[25])
# is driven by the FX3 synchronously to the PCLK this design hands it back -
# it only actually changes once every fx3_pclk period - but fx3StateMachine
# samples it into read_data_flag on every system_clock edge, twice that
# often. Without this, TimeQuest has no reason to believe a launch on
# fx3_pclk has two system_clock periods to arrive in and checks it against
# one, which is tighter than the 8 ns pessimistic clock-to-out bound this
# file gives the FX3's own outputs.
set_multicycle_path -setup 2 -from [get_clocks fx3_pclk] -to [get_clocks $pll_clock_pin]
set_multicycle_path -hold 1 -from [get_clocks fx3_pclk] -to [get_clocks $pll_clock_pin]


# ---------------------------------------------------------------------------
# Paths that are not timed, and why
# ---------------------------------------------------------------------------

# The SPI register link. Mode 0, and the master is the FX3 bit-banging GPIO
# through register writes, so the fastest it can go is two orders of magnitude
# below the 2 MHz the slave is specified for. All three inputs go through
# two-flop synchronisers and glitch filters in spiRegisters, which is what
# makes them safe to be asynchronous; timing them against the system clock
# would be constraining a relationship that does not exist.
set_false_path -from [get_ports {GPIO1[17]}]
set_false_path -from [get_ports {GPIO1[15]}]
set_false_path -from [get_ports {GPIO1[11]}]
set_false_path -to [get_ports {GPIO1[13]}]

# nReset from the FX3. It asserts asynchronously by design - a reset that
# needed a clock edge would not work when there is no clock - and the top
# level synchronises its release before any logic sees it, so recovery and
# removal analysis on the pin itself has nothing to say.
set_false_path -from [get_ports {GPIO1[7]}]

# The status LEDs. Driven from the register bank for a human to look at.
set_false_path -to [get_ports {LED[*]}]

# FX3 control lines the board wires up and this design does not use. They are
# tied off or ignored; the pin assignments are fixed by the PCB.
set_false_path -from [get_ports {GPIO1[23]}]
set_false_path -from [get_ports {GPIO1[9]}]
set_false_path -to [get_ports {GPIO1[19]}]
set_false_path -to [get_ports {GPIO1[5]}]
set_false_path -to [get_ports {GPIO1[3]}]
