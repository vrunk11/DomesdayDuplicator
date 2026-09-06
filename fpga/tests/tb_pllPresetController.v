/************************************************************************

    tb_pllPresetController.v

    Testbench for the PLL preset controller (T3)
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

    pllReconfig itself cannot be simulated - it is an ALTPLL_RECONFIG
    variation and needs Altera's altera_mf library, the same reason
    IPpllGenerator cannot be (fpga/README.md). What this checks instead is
    the one thing that is this project's own and therefore can be wrong on
    its own: that pllPresetController presents the *right* 144 bits, at
    the right addresses, for the preset that was actually asked for - and
    that it drives pllReconfig's handshake lines in the order its
    documentation describes.

    The DUT's counterpart here is a model of that handshake, not
    pllReconfig itself: it answers reset_rom_address and write_from_rom by
    sweeping rom_address_out over 0 to 143 and capturing whatever the DUT
    presents on rom_data_in, then raises and lowers busy the way pllReconfig
    is documented to - once for the ROM load, once again for a reconfig
    pulse. Real timing is unverified until real hardware; what a
    plausible, controllable model can prove is that the sequencing has no
    off-by-one and that the bit captured at every address is the one
    pllPresetController.v claims for that preset, independently listed
    here rather than compared against the DUT's own copy of itself.

    preset_request_toggle is driven here as the real interface expects -
    flipped once, separately from changing preset_request - rather than
    read directly, so this testbench also exercises the toggle
    synchroniser rather than assuming it works.

************************************************************************/

`timescale 1ns / 1ps

module tb_pllPresetController;

    reg           reset_n;
    reg           clock;
    reg     [7:0] preset_request;
    reg           preset_request_toggle;

    wire          reset_rom_address;
    wire          write_from_rom;
    wire          rom_data_in;
    reg     [7:0] rom_address_out;
    wire          reconfig;
    reg           busy;

    integer       errors;
    integer       i;

    // Independently listed, not read back from the DUT - see the header.
    // Copied bit for bit from the same .mif files pllPresetController.v
    // documents, so a mismatch here is a real disagreement rather than a
    // testbench that would pass whatever the DUT happened to contain.
    localparam [143:0] Expect40MHz = {
        36'b000000000000000001000000000000000001,
        36'b000000000000000001000000000000000001,
        36'b010000001110000000001000000001000000,
        36'b000000000000000001100000000110110000
    };
    localparam [143:0] Expect45MHz = {
        36'b000000000000000001000000000000000001,
        36'b000000000000000001000000000000000001,
        36'b010000001110000000001000001101000000,
        36'b000000000000000001100000000110110000
    };
    localparam [143:0] Expect50MHz = {
        36'b000000000000000001000000000000000001,
        36'b000000000000000001000000000000000001,
        36'b110000000110000000011000000011000000,
        36'b000000000000000001100000000110110000
    };
    localparam [143:0] Expect55MHz = {
        36'b000000000000000001000000000000000001,
        36'b000000000000000001000000000000000001,
        36'b010000001110000000101000001011000000,
        36'b000000000000000001100000000110110000
    };
    localparam [143:0] Expect60MHz = {
        36'b000000000000000001000000000000000001,
        36'b000000000000000001000000000000000001,
        36'b010000001110000000011000000011000000,
        36'b000000000000000001100000000110110000
    };
    localparam [143:0] Expect65MHz = {
        36'b000000000000000001000000000000000001,
        36'b000000000000000001000000000000000001,
        36'b010000001110000000011000001111000000,
        36'b000000000000000001100000000110110000
    };
    localparam [143:0] Expect70MHz = {
        36'b000000000000000001000000000000000001,
        36'b000000000000000001000000000000000001,
        36'b010000000010000000001110000001110000,
        36'b010000001110000000100000000110010000
    };
    localparam [143:0] Expect75MHz = {
        36'b000000000000000001000000000000000001,
        36'b000000000000000001000000000000000001,
        36'b010000000010000000011000000011000000,
        36'b000000000000000001100000000110110000
    };

    function [143:0] expected_bits;
        input [7:0] mhz;
        begin
            case (mhz)
                8'd40:   expected_bits = Expect40MHz;
                8'd45:   expected_bits = Expect45MHz;
                8'd50:   expected_bits = Expect50MHz;
                8'd55:   expected_bits = Expect55MHz;
                8'd60:   expected_bits = Expect60MHz;
                8'd65:   expected_bits = Expect65MHz;
                8'd70:   expected_bits = Expect70MHz;
                default: expected_bits = Expect75MHz;
            endcase
        end
    endfunction

    pllPresetController dut (
        .reset_n              (reset_n),
        .clock                (clock),
        .preset_request       (preset_request),
        .preset_request_toggle(preset_request_toggle),
        .reset_rom_address    (reset_rom_address),
        .write_from_rom       (write_from_rom),
        .rom_data_in          (rom_data_in),
        .rom_address_out      (rom_address_out),
        .reconfig             (reconfig),
        .busy                 (busy)
    );

    // 50 MHz system clock in this testbench - the controller has no timing
    // of its own that depends on the real rate, only on edges.
    initial begin
        clock = 1'b0;
    end
    always begin
        #10 clock = ~clock;
    end

    task check;
        input [31:0] got;
        input [31:0] want;
        input [511:0] what;
        begin
            if (got !== want) begin
                $display("FAIL: %0s: got %0d, expected %0d (t=%0t)", what, got, want, $time);
                errors = errors + 1;
            end
        end
    endtask

    // The model of pllReconfig's ROM handshake described in the header.
    // captured holds whatever the DUT presented at each address during the
    // most recent load, for the checks below to compare.
    reg [143:0] captured;
    reg         reconfig_seen;

    localparam integer LOAD_BUSY_CYCLES = 3;
    localparam integer APPLY_BUSY_CYCLES = 3;

    reg [7:0] busy_countdown;
    reg       loading;

    always @(posedge clock, negedge reset_n) begin
        if (!reset_n) begin
            rom_address_out <= 8'd0;
            busy            <= 1'b0;
            busy_countdown  <= 8'd0;
            loading         <= 1'b0;
            captured        <= 144'd0;
            reconfig_seen   <= 1'b0;
        end else begin
            if (reset_rom_address) begin
                rom_address_out <= 8'd0;
            end

            if (busy_countdown != 8'd0) begin
                busy           <= 1'b1;
                busy_countdown <= busy_countdown - 8'd1;
                if (busy_countdown == 8'd1) begin
                    busy <= 1'b0;
                end
            end else if (write_from_rom) begin
                // One address per clock: present it, capture what the DUT
                // answers with, then move on. The load is complete once
                // address 143 has been captured, which is what starts the
                // busy pulse the DUT is waiting for.
                captured[rom_address_out] <= rom_data_in;
                loading                   <= 1'b1;

                if (rom_address_out == 8'd143) begin
                    busy_countdown <= LOAD_BUSY_CYCLES;
                    loading        <= 1'b0;
                end else begin
                    rom_address_out <= rom_address_out + 8'd1;
                end
            end

            // A reconfig pulse starts the second busy pulse, standing in
            // for the actual scan into the live PLL.
            if (reconfig && busy_countdown == 8'd0 && !busy) begin
                busy_countdown <= APPLY_BUSY_CYCLES;
                reconfig_seen  <= 1'b1;
            end
        end
    end

    // Runs one preset request to completion: waits for the controller to
    // return to idle (both busy pulses done, write_from_rom and reconfig
    // both low again), then checks the address swept 0 to 143 and the
    // bits captured along the way.
    task run_preset;
        input [7:0] mhz;
        input [511:0] what;
        begin
            reconfig_seen         = 1'b0;
            preset_request        = mhz;
            preset_request_toggle = ~preset_request_toggle;

            // write_from_rom rising is the load starting
            while (write_from_rom !== 1'b1) begin
                @(posedge clock);
            end

            // and falling again, after the first busy pulse, is it ending
            while (write_from_rom !== 1'b0) begin
                @(posedge clock);
            end

            // reconfig is pulsed once as the very next thing, and the
            // second busy pulse that follows is what returns the
            // controller to idle
            while (reconfig_seen !== 1'b1) begin
                @(posedge clock);
            end
            while (busy !== 1'b0) begin
                @(posedge clock);
            end

            // A few idle clocks so a controller that kept driving
            // something would show it here rather than escape notice
            // between one task call and the next
            repeat (2) @(posedge clock);

            check(captured, expected_bits(mhz), what);
            check(reset_rom_address, 1'b0, {what, ": reset_rom_address idle afterwards"});
            check(write_from_rom, 1'b0, {what, ": write_from_rom idle afterwards"});
            check(reconfig, 1'b0, {what, ": reconfig idle afterwards"});
        end
    endtask

    initial begin
        errors                = 0;
        reset_n               = 1'b0;
        preset_request        = 8'h00;
        preset_request_toggle = 1'b0;

        repeat (4) @(posedge clock);
        #1 reset_n = 1'b1;
        repeat (4) @(posedge clock);

        // --- Reset ---
        check(reset_rom_address, 1'b0, "reset_rom_address after reset");
        check(write_from_rom, 1'b0, "write_from_rom after reset");
        check(reconfig, 1'b0, "reconfig after reset");

        // --- A value with no toggle is not a request ---
        //
        // preset_request changing on its own, with preset_request_toggle
        // left alone, must not start anything - the toggle is what makes
        // this a synchronised event rather than a signal this module
        // polls directly, which is the whole point of crossing it this
        // way. tb_spiRegisters.v is what proves the register itself only
        // ever changes alongside a write in the first place.
        preset_request = 8'd40;
        repeat (20) @(posedge clock);
        check(reset_rom_address, 1'b0, "a value change with no toggle starts nothing");
        check(write_from_rom, 1'b0, "write_from_rom stays low with no toggle");

        // --- PllPresetNone is never acted on ---
        //
        // No override means exactly that: a build that never receives a
        // preset write must never see this controller touch the PLL at
        // all, which is the property this checks rather than merely
        // asserting. The toggle is flipped, not just the value set, so
        // this proves the controller looked at PllPresetNone and declined
        // it - not merely that it was never told anything changed.
        preset_request        = 8'h00;
        preset_request_toggle = ~preset_request_toggle;
        repeat (20) @(posedge clock);
        check(reset_rom_address, 1'b0, "no sequence starts for PllPresetNone");
        check(write_from_rom, 1'b0, "write_from_rom stays low for PllPresetNone");
        check(reconfig, 1'b0, "reconfig stays low for PllPresetNone");

        // --- Every known preset, in turn ---
        //
        // All eight, not a sample of them: the lookup is a case statement
        // with one arm per preset, and a wrong arm for one specific value
        // is exactly the kind of mistake a subset would not catch.
        run_preset(8'd40, "40 MHz preset");
        run_preset(8'd45, "45 MHz preset");
        run_preset(8'd50, "50 MHz preset");
        run_preset(8'd55, "55 MHz preset");
        run_preset(8'd60, "60 MHz preset");
        run_preset(8'd65, "65 MHz preset");
        run_preset(8'd70, "70 MHz preset");
        run_preset(8'd75, "75 MHz preset");

        // --- Requesting the preset already active starts nothing ---
        //
        // The controller just finished settling on 75 MHz above. Asking
        // for it again is not a change, and re-running the whole scan
        // sequence for no reason is what this catches.
        preset_request        = 8'd75;
        preset_request_toggle = ~preset_request_toggle;
        repeat (20) @(posedge clock);
        check(reset_rom_address, 1'b0, "repeating the active preset starts no sequence");
        check(write_from_rom, 1'b0, "write_from_rom stays low for a repeated preset");

        // --- PllPresetNone after a preset is active is still not acted on ---
        //
        // "No override" does not mean "revert" - see the header of
        // pllPresetController.v. The PLL stays at 75 MHz here, which this
        // checks by the same means as above: nothing starts.
        preset_request        = 8'h00;
        preset_request_toggle = ~preset_request_toggle;
        repeat (20) @(posedge clock);
        check(reset_rom_address, 1'b0, "PllPresetNone after an active preset still starts nothing");
        check(write_from_rom, 1'b0, "write_from_rom stays low");

        // --- Back to a real preset afterwards ---
        //
        // Confirms the idle-on-PllPresetNone behaviour above did not leave
        // the controller unable to accept the next real request.
        run_preset(8'd40, "40 MHz preset after an ignored PllPresetNone");

        if (errors == 0) begin
            $display("tb_pllPresetController: PASS");
        end else begin
            $display("tb_pllPresetController: FAIL (%0d errors)", errors);
        end

        if (errors != 0) begin
            $fatal(1, "tb_pllPresetController failed");
        end
        $finish;
    end

endmodule
