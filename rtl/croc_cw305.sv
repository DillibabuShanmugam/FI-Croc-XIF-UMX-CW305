// =============================================================================
// CROC + Unified MX Coprocessor — CW305-A35T FPGA Top Wrapper
// =============================================================================
//
// Maps the CROC SoC (CVE2 + MX coprocessor) to the ChipWhisperer CW305
// Artix-7 FPGA board (XC7A35T-2FTG256) for fault injection analysis.
//
// Supported attack vectors:
//   - Clock glitch: CW Husky HS-IO → tio_clkin (N14) via BUFGMUX, selected
//     by DIP S2[J16].  Normal PLL clock on N13.
//   - Voltage glitch: SMA X3 crowbar on VCC-INT shunt low side (board-level,
//     no FPGA changes; trigger synchronization via trig_out).
//   - EMFI: X-Y table mounting holes on CW305 PCB (board-level; trigger sync).
//   - Power analysis: ext_clock output (M16) feeds CW ADC sampling clock for
//     synchronous capture.  Enabled by DIP S2[K16].
//
// DIP switch S2 mapping:
//   J16 — Clock source:  0 = PLL_CLK1 (normal),  1 = CW HS-IO (glitch clock)
//   K16 — HS-Out enable: 0 = disabled,           1 = drive ext_clock on M16
//   K15 — Fetch enable:  0 = core halted,         1 = core runs
//   L14 — Spare (active-high, directly available as soc_gpio_i[0])
//
// 20-pin connector:
//   IO1 (P16) — UART TX (FPGA → host)
//   IO2 (R16) — UART RX (host → FPGA)
//   IO4 (T14) — Trigger out (to CW capture hardware)
//
// JP3 header:
//   JTAG (5 pins): TCK=A13, TDI=B12, TDO=C11, TMS=B15, TRST=C14
//   Scan (4 pins): scan_en=A12, scan_capture=A14, scan_in=B14, scan_out=C13
//
// USB register map (CW305 convention, bytecount_size=7):
//   REG 0x00: core_status[31:0]  (R)  — (return_code << 1) | eoc_flag
//   REG 0x01: GPIO/LED           (R/W) — [3:0] gpio_o, write sets led override
//   REG 0x02: magic "CROC"       (R)  — 0x43524F43
//   REG 0x03: soft_reset         (W)  — bit[0] = assert SoC reset
//
// Python: target.fpga_read(0, 4)  → core_status
//         target.fpga_read(2, 4)  → "CROC" magic
//         target.fpga_write(3, [1]) → assert soft reset
//
// Adapted from: croc_xilinx.sv (Genesys2 target) +
//               CW305 reference AES target (cw305_top.v + cw305_usb_reg_fe.v)
// =============================================================================

module croc_cw305 import croc_pkg::*; #(
  localparam int unsigned GpioCount   = 4,
  localparam int unsigned pADDR_WIDTH  = 21,
  localparam int unsigned pBYTECNT_SIZE = 7
) (
  // ---- Clocks ----
  input  logic  pll_clk1,     // N13: CDCE906 PLL Ch1 (20 MHz, normal operation)
  input  logic  tio_clkin,    // N14: CW Husky HS-IO clock (glitch clock input)
  input  logic  usb_clk,      // F5:  SAM3U USB bus clock (96 MHz)

  // ---- Reset ----
  input  logic  rst_n,        // R1:  Push-button SW4 (active low)

  // ---- DIP switch S2 ----
  input  logic  j16_sel,      // J16: Clock source select (0=PLL, 1=CW HS-IO)
  input  logic  k16_sel,      // K16: HS-Out enable (drive ext_clock on M16)
  input  logic  k15_sel,      // K15: Fetch enable (core run/halt)
  input  logic  l14_sel,      // L14: Spare (routed to gpio_i[0])

  // ---- LEDs ----
  output logic  led1,         // T2
  output logic  led2,         // T3
  output logic  led3,         // T4

  // ---- UART (20-pin connector) ----
  output logic  uart_tx_o,    // P16: IO1 — FPGA TX → CW host
  input  logic  uart_rx_i,    // R16: IO2 — CW host → FPGA RX

  // ---- Trigger (20-pin connector) ----
  output logic  trig_out,     // T14: IO4 — to CW capture HW

  // ---- ADC sampling clock output ----
  output logic  ext_clock,    // M16: phase-matched SoC clock for CW ADC sync

  // ---- JTAG (JP3 header) ----
  input  logic  jtag_tck_i,   // A13
  input  logic  jtag_tms_i,   // B15
  input  logic  jtag_tdi_i,   // B12
  output logic  jtag_tdo_o,   // C11
  input  logic  jtag_trst_ni, // C14

  // ---- MX Coprocessor scan chain (JP3 header) ----
  input  logic  scan_en_i,       // A12
  input  logic  scan_capture_i,  // A14
  input  logic  scan_in_i,       // B14
  output logic  scan_out_o,      // C13

  // ---- SAM3U USB parallel bus (on-board, no external cable needed) ----
  inout  wire  [7:0] usb_data,    // USB_D[7:0]: A7,B6,D3,E3,F3,B5,K1,K2
  input  logic [pADDR_WIDTH-1:0] usb_addr,   // USB_A[20:0]
  input  logic       usb_rdn,     // USBRD (active low)
  input  logic       usb_wrn,     // USBWR (active low)
  input  logic       usb_cen,     // USBCE (active low)
  input  logic       usb_alen     // USBALE (active low)
);

  // =========================================================================
  // USB clock buffer
  // =========================================================================
  wire usb_clk_buf;
  BUFG i_usb_clk_buf (.I(usb_clk), .O(usb_clk_buf));

  // =========================================================================
  // Clock mux: PLL_CLK1 (normal) vs CW HS-IO (glitch clock)
  // =========================================================================
  wire soc_clk;

  BUFGMUX_CTRL i_clk_mux (
    .O  ( soc_clk    ),
    .I0 ( pll_clk1   ),   // S=0: PLL (normal)
    .I1 ( tio_clkin  ),   // S=1: CW Husky (glitch)
    .S  ( j16_sel    )    // DIP S2 bit 0
  );

  // =========================================================================
  // ADC sampling clock output: ext_clock on M16
  // =========================================================================
  assign ext_clock = k16_sel ? soc_clk : 1'b0;

  // =========================================================================
  // Reset synchronizer (2-FF) + USB soft-reset control
  // =========================================================================
  logic soft_reset_usb;  // in usb_clk domain
  logic soft_reset_sync1, soft_reset_sync2;  // CDC to soc_clk
  logic rst_ff1, synced_rst_n;

  // CDC: soft_reset from usb_clk → soc_clk
  always_ff @(posedge soc_clk or negedge rst_n) begin
    if (!rst_n) begin
      soft_reset_sync1 <= 1'b0;
      soft_reset_sync2 <= 1'b0;
    end else begin
      soft_reset_sync1 <= soft_reset_usb;
      soft_reset_sync2 <= soft_reset_sync1;
    end
  end

  wire combined_rst_n = rst_n & ~soft_reset_sync2;

  always_ff @(posedge soc_clk or negedge combined_rst_n) begin
    if (!combined_rst_n) begin
      rst_ff1      <= 1'b0;
      synced_rst_n <= 1'b0;
    end else begin
      rst_ff1      <= 1'b1;
      synced_rst_n <= rst_ff1;
    end
  end

  // =========================================================================
  // GPIO — minimal: 4-bit, directly drive LEDs
  // =========================================================================
  logic [GpioCount-1:0] soc_gpio_i;
  logic [GpioCount-1:0] soc_gpio_o;
  logic [GpioCount-1:0] soc_gpio_out_en_o;

  assign soc_gpio_i = {3'b000, l14_sel};

  // USB heartbeat on LED1, SoC heartbeat on LED2, GPIO on LED3
  reg [24:0] usb_heartbeat;
  always @(posedge usb_clk_buf) usb_heartbeat <= usb_heartbeat + 25'd1;
  assign led1 = usb_heartbeat[24];

  reg [22:0] soc_heartbeat;
  always @(posedge soc_clk) soc_heartbeat <= soc_heartbeat + 23'd1;
  assign led2 = soc_heartbeat[22];

  assign led3 = soc_gpio_o[2];

  // =========================================================================
  // Trigger output
  // =========================================================================
  logic soc_status;
  // Trigger driven by USB register 0x05 bit[0] (usb_clk domain).
  // Python sets trigger HIGH after reset release, CW-Lite ext_single fires.
  // No soc_clk dependency — avoids clock_xor deadlock.
  reg usb_trigger;
  assign trig_out = usb_trigger;

  // =========================================================================
  // SAM3U USB register bridge (using reference cw305_usb_reg_fe)
  // =========================================================================
  wire [7:0] usb_dout;
  wire       usb_isout;
  wire [pADDR_WIDTH-1:pBYTECNT_SIZE] reg_address;
  wire [pBYTECNT_SIZE-1:0]           reg_bytecnt;
  wire [7:0] reg_datao;  // write data from host
  wire [7:0] reg_datai;  // read data to host
  wire       reg_read;
  wire       reg_write;
  wire       reg_addrvalid;

  cw305_usb_reg_fe #(
    .pADDR_WIDTH    (pADDR_WIDTH),
    .pBYTECNT_SIZE  (pBYTECNT_SIZE),
    .pREG_RDDLY_LEN (3)
  ) i_usb_reg_fe (
    .rst          (~rst_n),
    .usb_clk      (usb_clk_buf),
    .usb_din      (usb_data),
    .usb_dout     (usb_dout),
    .usb_rdn      (usb_rdn),
    .usb_wrn      (usb_wrn),
    .usb_cen      (usb_cen),
    .usb_alen     (usb_alen),
    .usb_addr     (usb_addr),
    .usb_isout    (usb_isout),
    .reg_address  (reg_address),
    .reg_bytecnt  (reg_bytecnt),
    .reg_datao    (reg_datao),
    .reg_datai    (reg_datai),
    .reg_read     (reg_read),
    .reg_write    (reg_write),
    .reg_addrvalid(reg_addrvalid)
  );

  // Tristate data bus (reference pattern from cw305_top.v)
  assign usb_data = usb_isout ? usb_dout : 8'bZ;

  // =========================================================================
  // Register read/write logic (usb_clk domain)
  // =========================================================================
  // Register map (reg_address values, matching CW305 Python API):
  //   0x00: core_status[31:0]  (R)
  //   0x01: GPIO               (R/W)
  //   0x02: magic "CROC"       (R)
  //   0x03: soft_reset          (W)  — bit[0]
  //   0x04: REG_IDENTIFY       (R)  — 0x43573035 ("CW05")

  // CDC: core_status from soc_clk → usb_clk
  wire [31:0] core_status_full = i_croc_soc.i_croc.i_soc_ctrl.core_status_q;
  reg [31:0] core_status_sync1, core_status_sync2;
  always @(posedge usb_clk_buf) begin
    core_status_sync1 <= core_status_full;
    core_status_sync2 <= core_status_sync1;
  end

  // CDC: gpio from soc_clk → usb_clk
  reg [3:0] gpio_sync1, gpio_sync2;
  always @(posedge usb_clk_buf) begin
    gpio_sync1 <= soc_gpio_o;
    gpio_sync2 <= gpio_sync1;
  end

  // Read mux (usb_clk domain)
  reg [7:0] read_data;
  always @(*) begin
    read_data = 8'h00;
    case (reg_address)
      14'h00: // core_status
        case (reg_bytecnt[1:0])
          2'd0: read_data = core_status_sync2[7:0];
          2'd1: read_data = core_status_sync2[15:8];
          2'd2: read_data = core_status_sync2[23:16];
          2'd3: read_data = core_status_sync2[31:24];
        endcase
      14'h01: // GPIO
        read_data = {4'b0, gpio_sync2};
      14'h02: // Magic "CROC" = 0x43524F43
        case (reg_bytecnt[1:0])
          2'd0: read_data = 8'h43; // 'C'
          2'd1: read_data = 8'h52; // 'R'
          2'd2: read_data = 8'h4F; // 'O'
          2'd3: read_data = 8'h43; // 'C'
        endcase
      14'h03: // soft_reset readback
        read_data = {7'b0, soft_reset_usb};
      14'h04: // REG_IDENTIFY ("CW05")
        case (reg_bytecnt[1:0])
          2'd0: read_data = 8'h43; // 'C'
          2'd1: read_data = 8'h57; // 'W'
          2'd2: read_data = 8'h30; // '0'
          2'd3: read_data = 8'h35; // '5'
        endcase
      14'h07: // scan_status: [0]=scan_out, [1]=auto_done, [15:8]=count
        case (reg_bytecnt[1:0])
          2'd0: read_data = {6'b0, auto_scan_done_usb, scan_out_usb};
          2'd1: read_data = scan_count_usb;
          default: read_data = 8'h00;
        endcase
      14'h08: // scan_word0: bits [31:0]
        case (reg_bytecnt[1:0])
          2'd0: read_data = scan_sr_usb[7:0];
          2'd1: read_data = scan_sr_usb[15:8];
          2'd2: read_data = scan_sr_usb[23:16];
          2'd3: read_data = scan_sr_usb[31:24];
        endcase
      14'h09: // scan_word1: bits [63:32]
        case (reg_bytecnt[1:0])
          2'd0: read_data = scan_sr_usb[39:32];
          2'd1: read_data = scan_sr_usb[47:40];
          2'd2: read_data = scan_sr_usb[55:48];
          2'd3: read_data = scan_sr_usb[63:56];
        endcase
      14'h0A: // scan_word2: bits [95:64]
        case (reg_bytecnt[1:0])
          2'd0: read_data = scan_sr_usb[71:64];
          2'd1: read_data = scan_sr_usb[79:72];
          2'd2: read_data = scan_sr_usb[87:80];
          2'd3: read_data = scan_sr_usb[95:88];
        endcase
      14'h0B: // scan_word3: bits [127:96]
        case (reg_bytecnt[1:0])
          2'd0: read_data = scan_sr_usb[103:96];
          2'd1: read_data = scan_sr_usb[111:104];
          2'd2: read_data = scan_sr_usb[119:112];
          2'd3: read_data = scan_sr_usb[127:120];
        endcase
      14'h0C: read_data = {7'b0, scan_sr_usb[128]};
      default: read_data = 8'h00;
    endcase
  end

  // Pipeline read data to meet usb_clk timing (breaks combinational path
  // through read mux + OBUFT; pREG_RDDLY_LEN=3 provides margin for this stage)
  reg [7:0] read_data_r;
  always @(posedge usb_clk_buf)
    read_data_r <= read_data;

  assign reg_datai = read_data_r;

  // Write logic (usb_clk domain)
  always @(posedge usb_clk_buf) begin
    if (~rst_n) begin
      soft_reset_usb   <= 1'b0;
      usb_trigger      <= 1'b0;
      scan_capture_usb <= 1'b0;
      scan_shift_usb   <= 1'b0;
      scan_delay_usb   <= 16'd0;
    end else begin
      scan_capture_usb <= 1'b0;
      scan_shift_usb   <= 1'b0;
      if (reg_write) begin
        case (reg_address)
          14'h03: if (reg_bytecnt == 0) soft_reset_usb   <= reg_datao[0];
          14'h05: if (reg_bytecnt == 0) usb_trigger      <= reg_datao[0];
          14'h06: if (reg_bytecnt == 0) begin
                    scan_capture_usb <= reg_datao[0];
                    scan_shift_usb   <= reg_datao[1];
                  end
          14'h0D: case (reg_bytecnt[0])
                    1'b0: scan_delay_usb[7:0]  <= reg_datao;
                    1'b1: scan_delay_usb[15:8] <= reg_datao;
                  endcase
          default: ;
        endcase
      end
    end
  end

  // =========================================================================
  // Scan chain readout (USB-driven, works after clock glitch)
  // =========================================================================
  // Host sequence: hold reset → write 0x06[0]=1 (capture) → write 0x06[1]=1
  // x129 times (shift) → read 0x08..0x0C (129-bit scan data).
  // CDC pulse transfer: usb_clk toggle → soc_clk edge detect.

  localparam SCAN_BITS = 129;
  reg scan_capture_usb, scan_shift_usb;

  // Toggle-based CDC: usb_clk → soc_clk
  reg cap_tog_u, shf_tog_u;
  reg cap_tog_s1, cap_tog_s2, cap_tog_s3;
  reg shf_tog_s1, shf_tog_s2, shf_tog_s3;

  always @(posedge usb_clk_buf or negedge rst_n)
    if (!rst_n) begin cap_tog_u <= 0; shf_tog_u <= 0; end
    else begin
      if (scan_capture_usb) cap_tog_u <= ~cap_tog_u;
      if (scan_shift_usb)   shf_tog_u <= ~shf_tog_u;
    end

  always @(posedge soc_clk or negedge rst_n)
    if (!rst_n) begin
      cap_tog_s1<=0; cap_tog_s2<=0; cap_tog_s3<=0;
      shf_tog_s1<=0; shf_tog_s2<=0; shf_tog_s3<=0;
    end else begin
      cap_tog_s1<=cap_tog_u; cap_tog_s2<=cap_tog_s1; cap_tog_s3<=cap_tog_s2;
      shf_tog_s1<=shf_tog_u; shf_tog_s2<=shf_tog_s1; shf_tog_s3<=shf_tog_s2;
    end

  wire manual_scan_capture = cap_tog_s2 ^ cap_tog_s3;
  wire manual_scan_en      = shf_tog_s2 ^ shf_tog_s3;

  // ── Auto-capture FSM: capture scan at scan_delay cycles after trigger ──
  // REG 0x0D (W, 16-bit): scan_delay. Set to ext_offset+1 to capture
  // the MX state one cycle after the glitch. 0 = disabled.
  reg [15:0] scan_delay_usb;
  reg [15:0] scan_delay_soc;
  always @(posedge soc_clk) scan_delay_soc <= scan_delay_usb;

  // CDC: usb_trigger rising edge in soc_clk
  reg trig_s1, trig_s2, trig_prev;
  always @(posedge soc_clk or negedge rst_n)
    if (!rst_n) begin trig_s1<=0; trig_s2<=0; trig_prev<=0; end
    else begin trig_s1<=usb_trigger; trig_s2<=trig_s1; trig_prev<=trig_s2; end
  wire trig_rise = trig_s2 & ~trig_prev;

  // FSM
  localparam AS_IDLE=2'd0, AS_COUNT=2'd1, AS_CAP=2'd2, AS_SHIFT=2'd3;
  reg [1:0]  as_state;
  reg [15:0] as_counter;
  reg [7:0]  as_shift_cnt;
  reg auto_scan_capture, auto_scan_en, auto_scan_done;

  always @(posedge soc_clk or negedge rst_n) begin
    if (!rst_n) begin
      as_state<=AS_IDLE; as_counter<=0; as_shift_cnt<=0;
      auto_scan_capture<=0; auto_scan_en<=0; auto_scan_done<=0;
    end else begin
      auto_scan_capture <= 0;
      auto_scan_en      <= 0;
      case (as_state)
        AS_IDLE: begin
          auto_scan_done <= 0;
          if (trig_rise && scan_delay_soc != 0) begin
            as_state <= AS_COUNT; as_counter <= 0;
          end
        end
        AS_COUNT: begin
          as_counter <= as_counter + 1;
          if (as_counter == scan_delay_soc - 1) begin
            as_state <= AS_CAP; auto_scan_capture <= 1;
          end
        end
        AS_CAP: begin
          as_state <= AS_SHIFT; as_shift_cnt <= 0;
        end
        AS_SHIFT: begin
          auto_scan_en <= 1;
          as_shift_cnt <= as_shift_cnt + 1;
          if (as_shift_cnt == SCAN_BITS) begin
            as_state <= AS_IDLE; auto_scan_done <= 1; auto_scan_en <= 0;
          end
        end
      endcase
    end
  end

  // Combined scan control
  wire int_scan_capture = manual_scan_capture | auto_scan_capture;
  wire int_scan_en      = manual_scan_en      | auto_scan_en;

  wire scan_en_mux      = int_scan_en      | scan_en_i;
  wire scan_capture_mux = int_scan_capture  | scan_capture_i;
  wire scan_out_internal;
  assign scan_out_o = scan_out_internal;

  // Shift register (soc_clk domain)
  reg [SCAN_BITS-1:0] scan_sr;
  reg [7:0] scan_count;
  always @(posedge soc_clk or negedge rst_n)
    if (!rst_n) begin scan_sr <= '0; scan_count <= 0; end
    else if (int_scan_capture) scan_count <= 0;
    else if (int_scan_en) begin
      scan_sr    <= {scan_sr[SCAN_BITS-2:0], scan_out_internal};
      scan_count <= scan_count + 1;
    end

  // CDC: results to usb_clk
  reg [SCAN_BITS-1:0] scan_sr_usb;
  reg [7:0] scan_count_usb;
  reg scan_out_usb;
  reg auto_done_s1, auto_scan_done_usb;
  always @(posedge usb_clk_buf) begin
    scan_sr_usb       <= scan_sr;
    scan_count_usb    <= scan_count;
    scan_out_usb      <= scan_out_internal;
    auto_done_s1      <= auto_scan_done;
    auto_scan_done_usb <= auto_done_s1;
  end

  // =========================================================================
  // CROC SoC instantiation
  // =========================================================================
  croc_soc #(
    .GpioCount    ( GpioCount          ),
    .SramInitFile0( "bank0_init.mem"   ),
    .SramInitFile1( "bank1_init.mem"   )
  ) i_croc_soc (
    .clk_i          ( soc_clk        ),
    .rst_ni         ( synced_rst_n   ),
    .ref_clk_i      ( soc_clk        ),
    .testmode_i     ( 1'b0           ),
    .fetch_en_i     ( k15_sel        ),
    .status_o       ( soc_status     ),

    .jtag_tck_i,
    .jtag_tdi_i,
    .jtag_tdo_o,
    .jtag_tms_i,
    .jtag_trst_ni,

    .uart_rx_i,
    .uart_tx_o,

    .gpio_i         ( soc_gpio_i        ),
    .gpio_o         ( soc_gpio_o        ),
    .gpio_out_en_o  ( soc_gpio_out_en_o ),

    .scan_en_i      ( scan_en_mux       ),
    .scan_capture_i ( scan_capture_mux  ),
    .scan_in_i      ( scan_in_i         ),
    .scan_out_o     ( scan_out_internal )
  );

endmodule
