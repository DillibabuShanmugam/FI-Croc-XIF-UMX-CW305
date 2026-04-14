// =============================================================================
// CROC + MX Coprocessor — CW305 Top Wrapper (LIVE State Monitor)
// =============================================================================
//
// Direct-wire readout: MX coprocessor registers are continuously
// sampled into USB-readable registers via CDC flip-flops.
// No scan chain shift needed — always shows live register values.
//
// USB Register Map:
//   0x00: core_status   (R)   — (return_code << 1) | eoc_flag
//   0x01: GPIO           (R)
//   0x02: magic "CROC"   (R)   — 0x434F5243
//   0x03: soft_reset     (W)   — bit[0]
//   0x04: identify       (R)   — 0x43573035 "CW05"
//   0x05: trigger        (W)   — bit[0] = trig_out
//   0x08: rs1_q[31:0]    (R)   — live MX operand A
//   0x09: rs2_q[31:0]    (R)   — live MX operand B
//   0x0A: mac_acc_q[31:0](R)   — live MX MAC accumulator
//   0x0B: {se_a, se_b, op, fmt, inf, rd, id} (R) — live MX control
//   0x0C: snapshot_ctrl  (W)   — bit[0] = freeze snapshot
//
// Snapshot mode: write 0x0C[0]=1 to freeze all live regs at current
// values.  Write 0x0C[0]=0 to resume live updates.  This provides
// a coherent multi-register read without scan chain overhead.
// =============================================================================

module croc_cw305 import croc_pkg::*; #(
  localparam int unsigned GpioCount   = 4,
  localparam int unsigned pADDR_WIDTH  = 21,
  localparam int unsigned pBYTECNT_SIZE = 7
) (
  input  logic  pll_clk1,
  input  logic  tio_clkin,
  input  logic  usb_clk,
  input  logic  rst_n,
  input  logic  j16_sel,
  input  logic  k16_sel,
  input  logic  k15_sel,
  input  logic  l14_sel,
  output logic  led1, led2, led3,
  output logic  uart_tx_o,
  input  logic  uart_rx_i,
  output logic  trig_out,
  output logic  ext_clock,
  input  logic  jtag_tck_i,
  input  logic  jtag_tms_i,
  input  logic  jtag_tdi_i,
  output logic  jtag_tdo_o,
  input  logic  jtag_trst_ni,
  input  logic  scan_en_i,
  input  logic  scan_capture_i,
  input  logic  scan_in_i,
  output logic  scan_out_o,
  inout  wire  [7:0] usb_data,
  input  logic [pADDR_WIDTH-1:0] usb_addr,
  input  logic       usb_rdn,
  input  logic       usb_wrn,
  input  logic       usb_cen,
  input  logic       usb_alen
);

  // =========================================================================
  // USB clock buffer
  // =========================================================================
  wire usb_clk_buf;
  BUFG i_usb_clk_buf (.I(usb_clk), .O(usb_clk_buf));

  // =========================================================================
  // Clock mux
  // =========================================================================
  wire soc_clk;
  BUFGMUX_CTRL i_clk_mux (
    .O(soc_clk), .I0(pll_clk1), .I1(tio_clkin), .S(j16_sel)
  );
  assign ext_clock = k16_sel ? soc_clk : 1'b0;

  // =========================================================================
  // Reset synchronizer
  // =========================================================================
  logic soft_reset_usb;
  logic soft_reset_sync1, soft_reset_sync2;
  logic rst_ff1, synced_rst_n;

  always_ff @(posedge soc_clk or negedge rst_n) begin
    if (!rst_n) begin
      soft_reset_sync1 <= 1'b0; soft_reset_sync2 <= 1'b0;
    end else begin
      soft_reset_sync1 <= soft_reset_usb; soft_reset_sync2 <= soft_reset_sync1;
    end
  end

  wire combined_rst_n = rst_n & ~soft_reset_sync2;

  always_ff @(posedge soc_clk or negedge combined_rst_n) begin
    if (!combined_rst_n) begin rst_ff1 <= 1'b0; synced_rst_n <= 1'b0; end
    else begin rst_ff1 <= 1'b1; synced_rst_n <= rst_ff1; end
  end

  // =========================================================================
  // GPIO + LEDs
  // =========================================================================
  logic [GpioCount-1:0] soc_gpio_i, soc_gpio_o, soc_gpio_out_en_o;
  assign soc_gpio_i = {3'b000, l14_sel};

  reg [24:0] usb_hb; always @(posedge usb_clk_buf) usb_hb <= usb_hb + 1;
  reg [22:0] soc_hb; always @(posedge soc_clk)     soc_hb <= soc_hb + 1;
  assign led1 = usb_hb[24];
  assign led2 = soc_hb[22];
  assign led3 = soc_gpio_o[2];

  // =========================================================================
  // Trigger
  // =========================================================================
  logic soc_status;
  reg usb_trigger;
  assign trig_out = usb_trigger;

  // =========================================================================
  // Live State Monitor — direct wire from MX coprocessor registers
  // =========================================================================
  // Hierarchy: i_croc_soc.i_croc.i_core_wrap.i_mx.*
  //
  // These are hierarchical references to internal flip-flop outputs.
  // Vivado resolves them at elaboration time.

  wire [31:0] mx_rs1_q   = i_croc_soc.i_croc.i_core_wrap.i_mx.rs1_q;
  wire [31:0] mx_rs2_q   = i_croc_soc.i_croc.i_core_wrap.i_mx.rs2_q;
  wire [31:0] mx_mac_acc = i_croc_soc.i_croc.i_core_wrap.i_mx.mac_acc;
  wire [7:0]  mx_se_a    = i_croc_soc.i_croc.i_core_wrap.i_mx.se_a;
  wire [7:0]  mx_se_b    = i_croc_soc.i_croc.i_core_wrap.i_mx.se_b;
  wire [3:0]  mx_op      = i_croc_soc.i_croc.i_core_wrap.i_mx.instr_op_q;
  wire [2:0]  mx_fmt     = i_croc_soc.i_croc.i_core_wrap.i_mx.instr_fmt_q;
  wire        mx_inf     = i_croc_soc.i_croc.i_core_wrap.i_mx.instr_inflight_q;
  wire [4:0]  mx_rd      = i_croc_soc.i_croc.i_core_wrap.i_mx.instr_rd_q;
  wire [3:0]  mx_id      = i_croc_soc.i_croc.i_core_wrap.i_mx.instr_id_q;

  // Pack control fields into 32 bits:
  // [31:24]=se_a [23:16]=se_b [15:12]=op [11:9]=fmt [8]=inf [7:3]=rd [2:0]=id[3:1]
  // Actually let's keep it simple: byte-aligned
  // REG 0x0B byte 0 = se_a, byte 1 = se_b, byte 2 = {op,fmt,inf}, byte 3 = {rd,id[2:0]}
  wire [31:0] mx_ctrl = {3'b0, mx_rd, mx_id,       // byte 3: rd[4:0], id[3:1] + 0
                          mx_op, mx_fmt, mx_inf,     // byte 2: op[3:0], fmt[2:0], inf
                          mx_se_b,                   // byte 1: se_b[7:0]
                          mx_se_a};                  // byte 0: se_a[7:0]

  // CDC: soc_clk → usb_clk (2-stage synchronizer)
  // Snapshot control: when frozen, stop updating CDC registers
  reg snapshot_freeze;

  reg [31:0] live_rs1_s1, live_rs1_s2;
  reg [31:0] live_rs2_s1, live_rs2_s2;
  reg [31:0] live_mac_s1, live_mac_s2;
  reg [31:0] live_ctrl_s1, live_ctrl_s2;

  always @(posedge usb_clk_buf) begin
    if (!snapshot_freeze) begin
      live_rs1_s1  <= mx_rs1_q;   live_rs1_s2  <= live_rs1_s1;
      live_rs2_s1  <= mx_rs2_q;   live_rs2_s2  <= live_rs2_s1;
      live_mac_s1  <= mx_mac_acc; live_mac_s2  <= live_mac_s1;
      live_ctrl_s1 <= mx_ctrl;    live_ctrl_s2 <= live_ctrl_s1;
    end
  end

  // =========================================================================
  // USB register bridge
  // =========================================================================
  wire [7:0] usb_dout;
  wire       usb_isout;
  wire [pADDR_WIDTH-1:pBYTECNT_SIZE] reg_address;
  wire [pBYTECNT_SIZE-1:0]           reg_bytecnt;
  wire [7:0] reg_datao, reg_datai;
  wire       reg_read, reg_write, reg_addrvalid;

  cw305_usb_reg_fe #(
    .pADDR_WIDTH(pADDR_WIDTH), .pBYTECNT_SIZE(pBYTECNT_SIZE), .pREG_RDDLY_LEN(3)
  ) i_usb_reg_fe (
    .rst(~rst_n), .usb_clk(usb_clk_buf),
    .usb_din(usb_data), .usb_dout(usb_dout),
    .usb_rdn(usb_rdn), .usb_wrn(usb_wrn), .usb_cen(usb_cen),
    .usb_alen(usb_alen), .usb_addr(usb_addr),
    .usb_isout(usb_isout), .reg_address(reg_address), .reg_bytecnt(reg_bytecnt),
    .reg_datao(reg_datao), .reg_datai(reg_datai),
    .reg_read(reg_read), .reg_write(reg_write), .reg_addrvalid(reg_addrvalid)
  );
  assign usb_data = usb_isout ? usb_dout : 8'bZ;

  // CDC: core_status + gpio
  wire [31:0] core_status_full = i_croc_soc.i_croc.i_soc_ctrl.core_status_q;
  reg [31:0] cs_s1, cs_s2;
  reg [3:0] gpio_s1, gpio_s2;
  always @(posedge usb_clk_buf) begin
    cs_s1 <= core_status_full; cs_s2 <= cs_s1;
    gpio_s1 <= soc_gpio_o; gpio_s2 <= gpio_s1;
  end

  // Read mux
  reg [7:0] read_data;
  always @(*) begin
    read_data = 8'h00;
    case (reg_address)
      14'h00: case (reg_bytecnt[1:0])
                2'd0: read_data = cs_s2[7:0];
                2'd1: read_data = cs_s2[15:8];
                2'd2: read_data = cs_s2[23:16];
                2'd3: read_data = cs_s2[31:24];
              endcase
      14'h01: read_data = {4'b0, gpio_s2};
      14'h02: case (reg_bytecnt[1:0])
                2'd0: read_data = 8'h43; 2'd1: read_data = 8'h52;
                2'd2: read_data = 8'h4F; 2'd3: read_data = 8'h43;
              endcase
      14'h03: read_data = {7'b0, soft_reset_usb};
      14'h04: case (reg_bytecnt[1:0])
                2'd0: read_data = 8'h43; 2'd1: read_data = 8'h57;
                2'd2: read_data = 8'h30; 2'd3: read_data = 8'h35;
              endcase
      // ── Live MX state monitor registers ──
      14'h08: case (reg_bytecnt[1:0])
                2'd0: read_data = live_rs1_s2[7:0];
                2'd1: read_data = live_rs1_s2[15:8];
                2'd2: read_data = live_rs1_s2[23:16];
                2'd3: read_data = live_rs1_s2[31:24];
              endcase
      14'h09: case (reg_bytecnt[1:0])
                2'd0: read_data = live_rs2_s2[7:0];
                2'd1: read_data = live_rs2_s2[15:8];
                2'd2: read_data = live_rs2_s2[23:16];
                2'd3: read_data = live_rs2_s2[31:24];
              endcase
      14'h0A: case (reg_bytecnt[1:0])
                2'd0: read_data = live_mac_s2[7:0];
                2'd1: read_data = live_mac_s2[15:8];
                2'd2: read_data = live_mac_s2[23:16];
                2'd3: read_data = live_mac_s2[31:24];
              endcase
      14'h0B: case (reg_bytecnt[1:0])
                2'd0: read_data = live_ctrl_s2[7:0];   // se_a
                2'd1: read_data = live_ctrl_s2[15:8];  // se_b
                2'd2: read_data = live_ctrl_s2[23:16]; // op,fmt,inf
                2'd3: read_data = live_ctrl_s2[31:24]; // rd,id
              endcase
      default: read_data = 8'h00;
    endcase
  end

  reg [7:0] read_data_r;
  always @(posedge usb_clk_buf) read_data_r <= read_data;
  assign reg_datai = read_data_r;

  // Write logic
  always @(posedge usb_clk_buf) begin
    if (~rst_n) begin
      soft_reset_usb  <= 1'b0;
      usb_trigger     <= 1'b0;
      snapshot_freeze <= 1'b0;
    end else if (reg_write) begin
      case (reg_address)
        14'h03: if (reg_bytecnt == 0) soft_reset_usb  <= reg_datao[0];
        14'h05: if (reg_bytecnt == 0) usb_trigger     <= reg_datao[0];
        14'h0C: if (reg_bytecnt == 0) snapshot_freeze <= reg_datao[0];
        default: ;
      endcase
    end
  end

  // =========================================================================
  // CROC SoC
  // =========================================================================
  croc_soc #(
    .GpioCount(GpioCount),
    .SramInitFile0("bank0_init.mem"),
    .SramInitFile1("bank1_init.mem")
  ) i_croc_soc (
    .clk_i(soc_clk), .rst_ni(synced_rst_n), .ref_clk_i(soc_clk),
    .testmode_i(1'b0), .fetch_en_i(k15_sel), .status_o(soc_status),
    .jtag_tck_i, .jtag_tdi_i, .jtag_tdo_o, .jtag_tms_i, .jtag_trst_ni,
    .uart_rx_i, .uart_tx_o,
    .gpio_i(soc_gpio_i), .gpio_o(soc_gpio_o), .gpio_out_en_o(soc_gpio_out_en_o),
    .scan_en_i, .scan_capture_i, .scan_in_i, .scan_out_o
  );

endmodule
