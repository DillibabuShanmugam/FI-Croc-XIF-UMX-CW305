// =============================================================================
// MX Unified Coprocessor — CV-X-IF Protocol Handler
// =============================================================================
//
// Connects to the CVE2 core via the Core-V eXtension Interface (CVXIF).
// Decodes custom-0 instructions, selects MX format and operation from funct7
// and funct3, latches operands, and drives the unified mx_alu.
//
// Protocol flow (CVE2 2-stage pipeline):
//   1. FIRST_CYCLE: Core presents instruction on x_issue_req, coprocessor
//      checks opcode/funct7 and responds combinationally with accept.
//   2. Core transitions to MULTI_CYCLE, stalls pipeline.
//   3. Coprocessor presents result (x_result_valid = 1).
//   4. Core consumes result, returns to FIRST_CYCLE.
//
// CVE2 specifics (2-stage pipeline, no speculation):
//   - x_commit_valid is always 1, commit_kill is always 0
//   - x_issue_req.id is always 0
//   - Register data (x_register.rs) is available same cycle as issue
//   - Core waits in MULTI_CYCLE until x_result_valid
//
// Writeback bug fix: x_issue_resp.writeback must stay HIGH during
// MULTI_CYCLE (while instr_inflight_q = 1) so CVE2's decoder keeps
// rf_we = 1.  Without this, the result is silently dropped.
//
// Adapted from: int8_simd_coprocessor.sv
// =============================================================================

module mx_coprocessor
  import mx_pkg::*;
  import cve2_pkg::*;
(
  input  logic clk_i,
  input  logic rst_ni,

  // ---- CV-X-IF Issue Interface ----
  input  logic                    x_issue_valid_i,
  output logic                    x_issue_ready_o,
  input  cve2_pkg::x_issue_req_t  x_issue_req_i,
  output cve2_pkg::x_issue_resp_t x_issue_resp_o,

  // ---- CV-X-IF Register Interface ----
  input  cve2_pkg::x_register_t   x_register_i,

  // ---- CV-X-IF Commit Interface ----
  input  logic                    x_commit_valid_i,
  input  cve2_pkg::x_commit_t     x_commit_i,

  // ---- CV-X-IF Result Interface ----
  output logic                    x_result_valid_o,
  input  logic                    x_result_ready_i,
  output cve2_pkg::x_result_t     x_result_o,

  // ---- Scan chain interface ----
  input  logic        scan_en_i,       // shift mode
  input  logic        scan_capture_i,  // parallel capture (priority)
  input  logic        scan_in_i,       // serial data in
  output logic        scan_out_o       // serial data out (MSB first)
);

  // =========================================================================
  // Instruction decode (combinational from issue request)
  // =========================================================================
  logic [31:0] instr;
  logic [6:0]  opcode;
  logic [2:0]  funct3;
  logic [6:0]  funct7;
  logic [4:0]  rd;

  assign instr  = x_issue_req_i.instr;
  assign opcode = instr[6:0];
  assign funct3 = instr[14:12];
  assign funct7 = instr[31:25];
  assign rd     = instr[11:7];

  // Check if this is our instruction:
  //   - Opcode must be custom-0 (0x0B)
  //   - funct7 reserved bits [6:5] and [1] must be 0
  //   - funct7[4:2] must encode a valid MX format (0..4)
  logic is_our_opcode;
  logic is_valid_encoding;
  logic is_our_instr;

  assign is_our_opcode     = (opcode == MX_OPCODE);
  assign is_valid_encoding = funct7_reserved_ok(funct7)
                           && is_valid_format(funct7[4:2]);
  assign is_our_instr      = is_our_opcode && is_valid_encoding;

  // Decode format and operation
  mx_fmt_e decoded_fmt;
  mx_op_e  decoded_op;

  assign decoded_fmt = get_format(funct7);
  assign decoded_op  = mx_op_e'({funct7[0], funct3});

  // Determine register read requirements
  logic op_needs_rs1;
  logic op_needs_rs2;

  assign op_needs_rs1 = is_our_instr && (decoded_op != MX_OP_MACC)
                                      && (decoded_op != MX_OP_MACC_RD);
  assign op_needs_rs2 = is_our_instr && needs_rs2(decoded_op);

  // =========================================================================
  // Issue response (combinational)
  // =========================================================================
  logic instr_accepted;
  assign instr_accepted = x_issue_valid_i && is_our_instr;

  logic instr_inflight_q;  // forward declaration for writeback fix

  always_comb begin
    x_issue_resp_o.accept        = 1'b0;
    x_issue_resp_o.writeback     = 1'b0;
    x_issue_resp_o.register_read = 2'b00;
    x_issue_ready_o              = 1'b1;  // always ready to accept

    if (x_issue_valid_i && is_our_instr) begin
      x_issue_resp_o.accept        = 1'b1;
      x_issue_resp_o.writeback     = 1'b1;
      x_issue_resp_o.register_read = {op_needs_rs2, op_needs_rs1};
    end else if (instr_inflight_q) begin
      // CVE2 writeback bug fix: keep writeback HIGH during MULTI_CYCLE.
      // In MULTI_CYCLE the decoder re-checks x_issue_resp.writeback to
      // decide rf_we.  If we let it fall to 0, the register write is lost.
      x_issue_resp_o.writeback     = 1'b1;
    end
  end

  // =========================================================================
  // Pipeline state: latch instruction metadata and operands on accept
  // =========================================================================
  logic [3:0]  instr_id_q;
  logic [4:0]  instr_rd_q;
  mx_op_e      instr_op_q;
  mx_fmt_e     instr_fmt_q;
  logic [31:0] rs1_q, rs2_q;

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      instr_inflight_q <= 1'b0;
      instr_id_q       <= 4'd0;
      instr_rd_q       <= 5'd0;
      instr_op_q       <= MX_OP_DOT4;
      instr_fmt_q      <= MX_FMT_INT8;
      rs1_q            <= 32'd0;
      rs2_q            <= 32'd0;
    end else begin
      if (instr_accepted) begin
        instr_inflight_q <= 1'b1;
        instr_id_q       <= x_issue_req_i.id;
        instr_rd_q       <= rd;
        instr_op_q       <= decoded_op;
        instr_fmt_q      <= decoded_fmt;
        rs1_q            <= x_register_i.rs[0];
        rs2_q            <= x_register_i.rs[1];
      end else if (instr_inflight_q && x_result_ready_i) begin
        instr_inflight_q <= 1'b0;
      end
    end
  end

  // =========================================================================
  // MAC commit/kill signals
  // =========================================================================
  // CVE2 2-stage pipeline always commits (commit_valid=1, commit_kill=0).
  // Gate on inflight + MACS operation to avoid spurious commits to the ALU.
  logic mac_commit;
  logic mac_kill;

  assign mac_commit = x_commit_valid_i && !x_commit_i.commit_kill
                    && instr_inflight_q && (instr_op_q == MX_OP_MACS);
  assign mac_kill   = x_commit_valid_i &&  x_commit_i.commit_kill
                    && instr_inflight_q && (instr_op_q == MX_OP_MACS);

  // =========================================================================
  // Unified MX ALU instantiation
  // =========================================================================
  logic [31:0] alu_result;
  logic [7:0]  se_a;       // ALU shared exponent A (for scan chain)
  logic [7:0]  se_b;       // ALU shared exponent B (for scan chain)
  logic [31:0] mac_acc;    // ALU MAC accumulator   (for scan chain)

  mx_alu i_alu (
    .clk_i,
    .rst_ni,

    .rs1_i        ( rs1_q          ),
    .rs2_i        ( rs2_q          ),
    .op_i         ( instr_op_q     ),
    .fmt_i        ( instr_fmt_q    ),
    .valid_i      ( instr_accepted ),
    .mac_commit_i ( mac_commit     ),
    .mac_kill_i   ( mac_kill       ),

    .result_o     ( alu_result     ),

    .se_a_o       ( se_a           ),
    .se_b_o       ( se_b           ),
    .mac_acc_o    ( mac_acc        )
  );

  // =========================================================================
  // Scan chain: 129-bit coprocessor state snapshot
  // =========================================================================
  //   {rs1_q[31:0], rs2_q[31:0], mac_acc[31:0], se_a[7:0], se_b[7:0],
  //    instr_op_q[3:0], instr_fmt_q[2:0], instr_inflight_q,
  //    instr_rd_q[4:0], instr_id_q[3:0]}
  //
  // Shift-out order: MSB first (rs1_q[31] first, instr_id_q[0] last)
  logic [MX_SCAN_WIDTH-1:0] scan_data;

  assign scan_data = { rs1_q,                        // [128:97]
                       rs2_q,                        // [96:65]
                       mac_acc,                      // [64:33]
                       se_a,                         // [32:25]
                       se_b,                         // [24:17]
                       4'(instr_op_q),               // [16:13]
                       3'(instr_fmt_q),              // [12:10]
                       instr_inflight_q,             // [9]
                       instr_rd_q,                   // [8:4]
                       instr_id_q };                 // [3:0]

  scan_chain #(
    .WIDTH ( MX_SCAN_WIDTH )
  ) i_scan (
    .clk_i,
    .rst_ni,
    .scan_en_i,
    .scan_capture_i,
    .scan_in_i,
    .scan_out_o,
    .data_i  ( scan_data )
  );

  // =========================================================================
  // Result interface
  // =========================================================================
  always_comb begin
    x_result_valid_o  = instr_inflight_q;
    x_result_o.hartid = '0;
    x_result_o.id     = instr_id_q;
    x_result_o.data   = alu_result;
    x_result_o.rd     = instr_rd_q;
    x_result_o.we     = 1'b1;
  end

endmodule
