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
    pllReconfig itself, and it is modelled on the generated pllReconfig.v
    rather than on its documentation. The first version of this testbench
    modelled what the documentation seemed to say - a load that took each
    bit on the clock its address was presented, and ignored write_from_rom
    once it had started - and the controller written to pass it never
    retuned a PLL on the bench. The model now has the three properties
    the real state machine has:
      - write_from_rom seen while idle starts a load at address 0, and
        seen again on the first clock back in idle starts another;
      - the bit for an address is taken two clocks after the address is
        presented (addr_from_rom, then addr_from_rom2);
      - reconfig is acted on only while idle, and busy is high whenever
        the model is not idle.
    So it checks that the bit taken at every address is the one
    pllPresetController.v claims for that preset - independently listed
    here rather than compared against the DUT's own copy of itself - and
    that each request produces exactly one load and one reconfigure.

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

    // The whole scan chain. check() above is 32 bits wide, and passing it
    // these vectors compared only their low 32 bits - a chain shifted
    // anywhere above that passed.
    task check_bits;
        input [143:0] got;
        input [143:0] want;
        input [511:0] what;
        begin
            if (got !== want) begin
                $display("FAIL: %0s: got %h, expected %h (t=%0t)", what, got, want, $time);
                errors = errors + 1;
            end
        end
    endtask

    // The model of pllReconfig's side of the handshake described in the
    // header. captured holds the bit taken at each address during the most
    // recent load; loads_started and reconfig_seen are what show a request
    // produced exactly one of each.
    reg     [143:0] captured;
    reg             reconfig_seen;
    integer         loads_started;

    localparam integer APPLY_BUSY_CYCLES = 3;

    reg       issuing;
    reg [7:0] address_delayed;
    reg [7:0] address_delayed_twice;
    reg       valid_delayed;
    reg       valid_delayed_twice;
    reg [7:0] apply_countdown;

    always @(posedge clock, negedge reset_n) begin
        if (!reset_n) begin
            rom_address_out       <= 8'd0;
            busy                  <= 1'b0;
            captured              <= 144'd0;
            reconfig_seen         <= 1'b0;
            loads_started         <= 0;
            issuing               <= 1'b0;
            address_delayed       <= 8'd0;
            address_delayed_twice <= 8'd0;
            valid_delayed         <= 1'b0;
            valid_delayed_twice   <= 1'b0;
            apply_countdown       <= 8'd0;
        end else begin
            // The two-clock read latency: the bit arriving now belongs to
            // the address presented two clocks ago.
            if (valid_delayed_twice) begin
                captured[address_delayed_twice] <= rom_data_in;
            end
            address_delayed_twice <= address_delayed;
            valid_delayed_twice   <= valid_delayed;
            address_delayed       <= rom_address_out;
            valid_delayed         <= issuing;

            if (issuing) begin
                // One address per clock, 0 to 143
                if (rom_address_out == 8'd143) begin
                    issuing <= 1'b0;
                end else begin
                    rom_address_out <= rom_address_out + 8'd1;
                end
            end else if (busy && apply_countdown == 8'd0 && !valid_delayed &&
                         !valid_delayed_twice) begin
                // The last address has been answered: the load is done
                busy <= 1'b0;
            end

            // A stand-in for the scan into the live PLL and its relock
            if (apply_countdown != 8'd0) begin
                apply_countdown <= apply_countdown - 8'd1;
                if (apply_countdown == 8'd1) begin
                    busy <= 1'b0;
                end
            end

            // Both requests are only acted on while idle - and a
            // write_from_rom still high on the first clock back in idle
            // starts another load, exactly as pllReconfig's does
            if (!busy) begin
                if (write_from_rom) begin
                    rom_address_out <= 8'd0;
                    issuing         <= 1'b1;
                    busy            <= 1'b1;
                    loads_started   <= loads_started + 1;
                end else if (reconfig) begin
                    apply_countdown <= APPLY_BUSY_CYCLES;
                    busy            <= 1'b1;
                    reconfig_seen   <= 1'b1;
                end
            end
        end
    end

    // Runs one preset request to completion: waits for the model to have
    // seen a reconfigure and to be idle again, then checks the bits taken
    // during the load and that the request produced exactly one load.
    //
    // Bounded, because the failure this exists to catch is a reconfig pulse
    // that pllReconfig never sees - a controller still holding
    // write_from_rom when the load finishes starts a second one, and the
    // reconfig lands while that is busy - and waiting on it unbounded would
    // hang rather than fail.
    localparam integer SEQUENCE_TIMEOUT_CLOCKS = 2000;

    integer loads_before;
    integer waited;

    task run_preset;
        input [7:0] mhz;
        input [511:0] what;
        begin
            reconfig_seen         = 1'b0;
            loads_before          = loads_started;
            preset_request        = mhz;
            preset_request_toggle = ~preset_request_toggle;

            waited                = 0;
            while (reconfig_seen !== 1'b1 && waited < SEQUENCE_TIMEOUT_CLOCKS) begin
                @(posedge clock);
                waited = waited + 1;
            end
            check(reconfig_seen, 1'b1, {what, ": reconfig reached pllReconfig while idle"});

            while (busy !== 1'b0 && waited < SEQUENCE_TIMEOUT_CLOCKS) begin
                @(posedge clock);
                waited = waited + 1;
            end

            // A few idle clocks so a controller that kept driving
            // something would show it here rather than escape notice
            // between one task call and the next
            repeat (4) @(posedge clock);

            check_bits(captured, expected_bits(mhz), what);
            check(loads_started - loads_before, 1, {what, ": exactly one load"});
            check(reset_rom_address, 1'b0, {what, ": reset_rom_address never driven"});
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
        loads_before          = loads_started;
        preset_request        = 8'd75;
        preset_request_toggle = ~preset_request_toggle;
        repeat (20) @(posedge clock);
        check(loads_started - loads_before, 0, "repeating the active preset starts no load");
        check(write_from_rom, 1'b0, "write_from_rom stays low for a repeated preset");

        // --- PllPresetNone after a preset is active is still not acted on ---
        //
        // "No override" does not mean "revert" - see the header of
        // pllPresetController.v. The PLL stays at 75 MHz here, which this
        // checks by the same means as above: nothing starts.
        loads_before          = loads_started;
        preset_request        = 8'h00;
        preset_request_toggle = ~preset_request_toggle;
        repeat (20) @(posedge clock);
        check(loads_started - loads_before, 0,
              "PllPresetNone after an active preset still starts nothing");
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
