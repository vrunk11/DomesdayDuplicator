/************************************************************************

    pllPresetController.v

    Drives pllReconfig's external-ROM interface to retune the ADC
    sampling rate at runtime
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

    pllReconfig (an ALTPLL_RECONFIG variation, generated with "Add ports
    to write to the scan chain from external ROM during run time"
    checked) does not carry a table of presets inside it - it exposes an
    address/data port pair and expects something outside it to answer
    with the right bit for whichever configuration is wanted. This module
    is that something: the eight 144-bit scan-chain contents below are
    not computed here, they are copied bit for bit from the .mif files
    Quartus itself generated for each target frequency (IPpllGenerator's
    "Additional Configuration File" button, one recompute per preset) -
    see TODO.md for how they were produced. Nothing here derives an M/N/C
    counter value from a frequency; that arithmetic already happened
    inside Quartus, once per preset, and what is stored is its answer.

    The handshake with pllReconfig follows the "poll busy" pattern its
    own documentation describes for the ROM interface: pulse
    reset_rom_address, hold write_from_rom while presenting the addressed
    bit until busy has been seen to rise and fall (the load into
    pllReconfig's internal cache is complete), then pulse reconfig and
    wait for a second busy rise and fall (the actual scan into the live
    PLL, and its relock). This sequencing has not been exercised in
    simulation - altpll_reconfig has no free simulation model, the same
    limitation IPpllGenerator itself has - so it is unverified until it
    is run on real hardware. See TODO.md.

    A request for PllPresetNone (0x00) is not acted on. It means "no
    override", not "revert to the default" - a build that never receives
    a preset write never drives this controller at all, and this module
    existing at all must not change that.

    clock is CLOCK_50, not system_clock, and that is load-bearing rather
    than a style choice: pllReconfig's clock input becomes scanclk on the
    PLL it drives, and the Cyclone IV datasheet caps fSCANCLK at 100 MHz.
    Sourcing it from system_clock (up to 150 MHz across the presets this
    module exists to select) reproduced a Minimum Pulse Width violation on
    the PLL's own output that tracked that 10 ns requirement exactly
    across every frequency tried - not a property of the PLL output
    itself, which closes cleanly without reconfiguration enabled at the
    same frequencies. This is the one deliberate second clock domain in
    the design; see the CDC note at preset_request_toggle below for why
    its cost is small.

    A second failsafe, beside spiRegisters.v's refusal to store a
    PLL_PRESET write above MaxAdcRateMHz: that one only covers a write
    arriving after the fact, and does nothing about the rate the PLL is
    already running at the moment this module comes out of reset, which
    is StaticDefaultMHz - a fact about how IPpllGenerator was compiled,
    not about anything this module or spiRegisters knows without being
    told. If a build's StaticDefaultMHz turns out to exceed the
    MaxAdcRateMHz that same build declares - a board fitted with a
    slower ADC than the one the static PLL was generated for, most
    plausibly - this module scans itself down to the highest preset at
    or below MaxAdcRateMHz once, unconditionally, the first time it
    reaches StateIdle after reset, before it ever looks at
    preset_request. No host action required and none possible in time to
    matter: the alternative is a board that runs its ADC out of the only
    spec this module was ever told about, from the moment configuration
    finishes, for as long as it takes a host to notice and write a
    correction.

************************************************************************/

module pllPresetController #(
    // What this specific build's IPpllGenerator was actually generated
    // for - the rate the PLL is already running at the moment this module
    // comes out of reset, before any preset_request has been acted on.
    // Not derived from anything else here because it cannot be: it is a
    // fact about a MegaWizard regeneration, fixed at synthesis time, and
    // the only place it can be recorded is a parameter that whoever
    // changes the static PLL's frequency also has to update.
    parameter [7:0] StaticDefaultMHz = 8'd75,

    // The board's own capability, exactly as given to spiRegisters.v's
    // identically-named parameter - see there for what a mismatch between
    // the two would mean. Passed here separately, rather than read back
    // out of spiRegisters over a wire, because it is a build-time fact
    // about the hardware, not run-time state - the two parameters are
    // meant to be given the same value at every instantiation site, and a
    // build that gave them different ones would be asking the PLL to obey
    // a limit it was told does not apply to it.
    parameter [7:0] MaxAdcRateMHz = 8'd75
) (
    input reset_n,
    input clock,

    // From spiRegisters' PLL_PRESET register, in the system_clock domain:
    // PllPresetNone (0x00) or one of the eight MHz values
    // PllPresetPresent's write-side normalisation guarantees this can be -
    // see spiRegisters.v. Anything else this module cannot occur by
    // construction, so there is no third case to handle here.
    input [7:0] preset_request,

    // Toggles once, in the system_clock domain, for every write that
    // changes preset_request - see the toggle-synchroniser below. This is
    // the one signal this module's clock-domain crossing depends on;
    // preset_request itself is read directly once the toggle has been
    // seen, which is safe because spiRegisters holds it for a whole SPI
    // transaction's worth of system_clock cycles, several orders of
    // magnitude longer than the two clock<->clock synchronisers here take
    // to settle.
    input preset_request_toggle,

    // pllReconfig's external-ROM and reconfigure interface. write_rom_ena
    // is deliberately not a port here: this module answers with the
    // addressed bit unconditionally, so there is nothing to gate on it -
    // pllReconfig is what decides when the address and data it is offered
    // are the ones it actually samples.
    output reg       reset_rom_address,
    output reg       write_from_rom,
    output           rom_data_in,
    input      [7:0] rom_address_out,
    output reg       reconfig,
    input            busy
);

    // The scan-chain content for each preset, 144 bits wide, indexed the
    // same way pllReconfig's own rom_address_out addresses it: bit N of
    // this vector is the value the .mif named at address N. Copied
    // verbatim - see the header comment.
    localparam [143:0] Preset40MHz = {
        36'b000000000000000001000000000000000001,
        36'b000000000000000001000000000000000001,
        36'b010000001110000000001000000001000000,
        36'b000000000000000001100000000110110000
    };
    localparam [143:0] Preset45MHz = {
        36'b000000000000000001000000000000000001,
        36'b000000000000000001000000000000000001,
        36'b010000001110000000001000001101000000,
        36'b000000000000000001100000000110110000
    };
    localparam [143:0] Preset50MHz = {
        36'b000000000000000001000000000000000001,
        36'b000000000000000001000000000000000001,
        36'b110000000110000000011000000011000000,
        36'b000000000000000001100000000110110000
    };
    localparam [143:0] Preset55MHz = {
        36'b000000000000000001000000000000000001,
        36'b000000000000000001000000000000000001,
        36'b010000001110000000101000001011000000,
        36'b000000000000000001100000000110110000
    };
    localparam [143:0] Preset60MHz = {
        36'b000000000000000001000000000000000001,
        36'b000000000000000001000000000000000001,
        36'b010000001110000000011000000011000000,
        36'b000000000000000001100000000110110000
    };
    localparam [143:0] Preset65MHz = {
        36'b000000000000000001000000000000000001,
        36'b000000000000000001000000000000000001,
        36'b010000001110000000011000001111000000,
        36'b000000000000000001100000000110110000
    };
    localparam [143:0] Preset70MHz = {
        36'b000000000000000001000000000000000001,
        36'b000000000000000001000000000000000001,
        36'b010000000010000000001110000001110000,
        36'b010000001110000000100000000110010000
    };
    localparam [143:0] Preset75MHz = {
        36'b000000000000000001000000000000000001,
        36'b000000000000000001000000000000000001,
        36'b010000000010000000011000000011000000,
        36'b000000000000000001100000000110110000
    };

    // The known preset values, gated the same way spiRegisters.v gates
    // them - PllPresetNone deliberately absent, since a request of it is
    // not something this module acts on.
    localparam [7:0] PllPresetNone = 8'h00;
    localparam [7:0] PllPreset40MHz = 8'd40;
    localparam [7:0] PllPreset45MHz = 8'd45;
    localparam [7:0] PllPreset50MHz = 8'd50;
    localparam [7:0] PllPreset55MHz = 8'd55;
    localparam [7:0] PllPreset60MHz = 8'd60;
    localparam [7:0] PllPreset65MHz = 8'd65;
    localparam [7:0] PllPreset70MHz = 8'd70;
    localparam [7:0] PllPreset75MHz = 8'd75;

    function [143:0] preset_bits;
        input [7:0] mhz;
        begin
            case (mhz)
                PllPreset40MHz: preset_bits = Preset40MHz;
                PllPreset45MHz: preset_bits = Preset45MHz;
                PllPreset50MHz: preset_bits = Preset50MHz;
                PllPreset55MHz: preset_bits = Preset55MHz;
                PllPreset60MHz: preset_bits = Preset60MHz;
                PllPreset65MHz: preset_bits = Preset65MHz;
                PllPreset70MHz: preset_bits = Preset70MHz;
                PllPreset75MHz: preset_bits = Preset75MHz;
                // Unreachable by construction - preset_bits is only ever
                // called with active_target, which is only ever loaded from
                // preset_request, which spiRegisters.v's write-side
                // normalisation guarantees is one of the eight arms above or
                // PllPresetNone (never passed here - see StateIdle). Kept
                // rather than omitted so a mistake in that guarantee fails
                // safe to the highest-margin preset instead of to whatever
                // Verilog does with a table that does not cover its input.
                default:        preset_bits = Preset75MHz;
            endcase
        end
    endfunction

    // The highest known preset at or below max_rate, or PllPresetNone if
    // even the lowest known preset exceeds it. That second case is not
    // expected to arise - MaxAdcRateMHz is meant to be a real board
    // capability, and every board this design targets can do at least
    // 40 MHz - but a function that fell back to a plausible-looking wrong
    // answer instead of the one value this module never acts on would be
    // the wrong failure mode for a safety check to have.
    function [7:0] safe_default_preset;
        input [7:0] max_rate;
        begin
            if (PllPreset75MHz <= max_rate) begin
                safe_default_preset = PllPreset75MHz;
            end else if (PllPreset70MHz <= max_rate) begin
                safe_default_preset = PllPreset70MHz;
            end else if (PllPreset65MHz <= max_rate) begin
                safe_default_preset = PllPreset65MHz;
            end else if (PllPreset60MHz <= max_rate) begin
                safe_default_preset = PllPreset60MHz;
            end else if (PllPreset55MHz <= max_rate) begin
                safe_default_preset = PllPreset55MHz;
            end else if (PllPreset50MHz <= max_rate) begin
                safe_default_preset = PllPreset50MHz;
            end else if (PllPreset45MHz <= max_rate) begin
                safe_default_preset = PllPreset45MHz;
            end else if (PllPreset40MHz <= max_rate) begin
                safe_default_preset = PllPreset40MHz;
            end else begin
                safe_default_preset = PllPresetNone;
            end
        end
    endfunction

    // Whether the static PLL, as this build compiled it, needs correcting
    // down to something MaxAdcRateMHz actually allows - and what to correct
    // it to, when it does. Combinational: both are facts about the two
    // parameters above and never change while the design is running.
    wire [7:0] startup_safe_preset = safe_default_preset(MaxAdcRateMHz);
    wire       startup_correction_needed =
        (StaticDefaultMHz > MaxAdcRateMHz) && (startup_safe_preset != PllPresetNone);

    // The standard toggle synchroniser: two flops catch preset_request_toggle
    // into this clock domain, and an edge on the synchronised copy is what
    // preset_request has settled means a new value is ready to read. A raw
    // multi-bit synchroniser on preset_request itself would risk sampling a
    // torn value - some bits resolved to the new value, some still the old
    // one - for one cycle at exactly the moment this module would be
    // deciding what to do with it; a single synchronised bit cannot tear.
    reg [1:0] preset_toggle_sync;
    reg preset_toggle_sync_previous;

    always @(posedge clock, negedge reset_n) begin
        if (!reset_n) begin
            preset_toggle_sync          <= 2'b00;
            preset_toggle_sync_previous <= 1'b0;
        end else begin
            preset_toggle_sync          <= {preset_toggle_sync[0], preset_request_toggle};
            preset_toggle_sync_previous <= preset_toggle_sync[1];
        end
    end

    wire         preset_request_ready = preset_toggle_sync[1] != preset_toggle_sync_previous;

    // The preset a reconfiguration is currently in flight for, or has
    // most recently completed for. Latched when a new request is
    // accepted, so the ROM content presented mid-sequence cannot change
    // under a request that arrives while one is already running.
    reg  [  7:0] active_target;

    // Whether the power-on safety check above has run. Set the first time
    // this module reaches StateIdle after reset and never cleared again -
    // the check is about what the PLL is doing the moment configuration
    // finishes, not something to repeat, and repeating it would mean a
    // host-requested preset getting silently overridden the next time this
    // state is passed through.
    reg          self_check_done;

    wire [143:0] active_preset_bits = preset_bits(active_target);
    assign rom_data_in = active_preset_bits[rom_address_out];

    localparam [2:0] StateIdle = 3'd0;
    localparam [2:0] StateLoadRom = 3'd1;
    localparam [2:0] StateWaitLoadBusy = 3'd2;
    localparam [2:0] StateWaitLoadDone = 3'd3;
    localparam [2:0] StateReconfigure = 3'd4;
    localparam [2:0] StateWaitApplyBusy = 3'd5;
    localparam [2:0] StateWaitApplyDone = 3'd6;

    reg [2:0] state;

    always @(posedge clock, negedge reset_n) begin
        if (!reset_n) begin
            state             <= StateIdle;
            active_target     <= PllPresetNone;
            self_check_done   <= 1'b0;
            reset_rom_address <= 1'b0;
            write_from_rom    <= 1'b0;
            reconfig          <= 1'b0;
        end else begin
            reset_rom_address <= 1'b0;
            reconfig          <= 1'b0;

            case (state)
                StateIdle: begin
                    if (!self_check_done) begin
                        // Runs exactly once, and before anything a host
                        // could have asked for is even considered - see
                        // the header comment for why this cannot wait for
                        // preset_request.
                        self_check_done <= 1'b1;
                        if (startup_correction_needed) begin
                            active_target     <= startup_safe_preset;
                            reset_rom_address <= 1'b1;
                            state             <= StateLoadRom;
                        end
                        // Only a synchronised, named preset change starts a
                        // sequence - PllPresetNone is never acted on, and a
                        // request equal to the one already active is not a
                        // change worth another pass through the sequence.
                    end else if (preset_request_ready && preset_request != PllPresetNone &&
                                 preset_request != active_target) begin
                        active_target     <= preset_request;
                        reset_rom_address <= 1'b1;
                        state             <= StateLoadRom;
                    end
                end

                // One cycle for pllReconfig to see reset_rom_address and
                // return its address counter to zero before the load
                // begins.
                StateLoadRom: begin
                    write_from_rom <= 1'b1;
                    state          <= StateWaitLoadBusy;
                end

                // rom_data_in is driven combinationally from
                // active_target and rom_address_out throughout, so there
                // is nothing to do here but wait for pllReconfig to take
                // the whole 144 bits and report it by raising busy.
                StateWaitLoadBusy: begin
                    if (busy) begin
                        state <= StateWaitLoadDone;
                    end
                end

                StateWaitLoadDone: begin
                    if (!busy) begin
                        write_from_rom <= 1'b0;
                        state          <= StateReconfigure;
                    end
                end

                // The load only filled pllReconfig's cache. reconfig is
                // what scans that cache into the live PLL.
                StateReconfigure: begin
                    reconfig <= 1'b1;
                    state    <= StateWaitApplyBusy;
                end

                StateWaitApplyBusy: begin
                    if (busy) begin
                        state <= StateWaitApplyDone;
                    end
                end

                StateWaitApplyDone: begin
                    if (!busy) begin
                        state <= StateIdle;
                    end
                end

                default: state <= StateIdle;
            endcase
        end
    end

endmodule
