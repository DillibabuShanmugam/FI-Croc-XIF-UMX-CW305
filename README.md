# MX-FI-Artifacts

Reproducibility artifacts for fault injection on the CROC SoC (CVE2 Ibex RV32IMC + Unified MX Coprocessor via CV-X-IF). Evaluates three OCP Microscaling formats (MXINT8, MXFP8-E4M3, MXFP8-E5M2) and two proposed logarithmic formats (MXLOG8, MXLOG8-LOGDOM) under DVFS and clock glitch attack.

## Hardware Setup

- **FPGA:** Xilinx Artix-7 XC7A35T-2FTG256 on ChipWhisperer CW305
- **Clock:** 20 MHz (CDCE906 PLL), WNS = 9.919 ns
- **Glitch source:** ChipWhisperer CW-Lite (clock_xor via tio_clkin)
- **Voltage control:** CW305 on-board USB regulator (target.vccint_set)

| DIP Switch | DVFS (J16=0) | Clock Glitch (J16=1) |
|------------|-------------|---------------------|
| J16 | 0 (PLL clock) | 1 (CW-Lite clock) |
| K16, K15, L14 | 1, 1, 1 | 1, 1, 1 |

## Design Utilization (cw305_live, post-place)

| Module | LUTs | FFs | BRAM36 |
|--------|------|-----|--------|
| Full SoC (croc_cw305) | 11,621 (55.9%) | 4,790 (11.5%) | 2 (4.0%) |
| CVE2 CPU (cve2_core) | 3,421 | 1,951 | 0 |
| MX Coprocessor (mx_coprocessor) | 4,860 | 255 | 0 |
| State monitor overhead | 7 | 383 | 0 |

## 129-Bit State Monitor

Direct-wire CDC readout from MX coprocessor internals to USB registers.

| Field | Width | USB Register | Description |
|-------|-------|-------------|-------------|
| rs1_q | 32 | REG 0x08 | Operand A (CV-X-IF) |
| rs2_q | 32 | REG 0x09 | Operand B |
| mac_acc_q | 32 | REG 0x0A | MAC accumulator |
| se_a_q | 8 | REG 0x0B[7:0] | Shared exponent A (E8M0, neutral=127) |
| se_b_q | 8 | REG 0x0B[15:8] | Shared exponent B |
| op, fmt, inf, rd, id | 17 | REG 0x0B[31:16] | Control fields |

## Experiment Matrix

| | DVFS | Clock Glitch |
|---|---|---|
| **QNN × 5 formats** | 570 trials, SDC found | 450 trials + 3,065 earlier |
| **BitNet × 5 formats** | 570 trials, SDC found | 450 trials, 0 transient SDC |

### DVFS Results — Format Vulnerability Ordering

| Format | CPU-SDC Onset | SE-SDC Onset | Crash | Dead |
|--------|--------------|-------------|-------|------|
| MXFP8-E5M2 | **0.81V** | 0.67V | 0.77V | 0.63V |
| MXFP8-E4M3 | **0.80V** | 0.67V | 0.77V | 0.63V |
| MXINT8 | **0.78V** | 0.67V | 0.77V | 0.63V |
| MXLOG8 | 0.68V | 0.67V | 0.77V | 0.63V |
| MXLOG8-LOGDOM | 0.68V | 0.67V | 0.77V | 0.63V |

### Root-Cause Decomposition (State Monitor)

1. **CPU-path SDC** (0.78–0.81V): se_a=127 correct, ret wrong → fault in CVE2 pipeline
2. **SE corruption** (0.67–0.68V): se_a→255, ret→127 saturated → fault in MX coprocessor
3. **Crash zone** (0.72–0.76V): CPU halts (eoc=0), SE transitioning
4. **Dead** (≤0.63V): FPGA fabric non-functional (ret=0x7FFFFFFF)

### Clock Glitch Results

- **Persistent FPGA config faults:** 445/675 at w=-7 (SRAM bit-flip, survives soft reset)
- **Transient computation faults:** 0/3,965 with reprogram-per-trial
- CW-Lite integer-step width cannot bridge the 9.9 ns timing slack

## File Structure

```
MX-FI-Artifacts/
├── README.md
├── scripts/
│   ├── dvfs_5format_sweep.py       # DVFS: QNN × 5 formats
│   ├── dvfs_bitnet_5format.py      # DVFS: BitNet × 5 formats
│   ├── clkglitch_qnn_5format.py    # Clock glitch: QNN × 5 formats
│   ├── clkglitch_bitnet_5format.py # Clock glitch: BitNet × 5 formats
│   └── hex2mem.py                  # RISC-V hex → Xilinx BRAM .mem
├── firmware/
│   ├── include/mx.h                # MX ISA extension macros (5 formats)
│   ├── qnn_llm_{format}.c         # QNN workload (5 variants)
│   └── tiny_llm_{format}_fi.c     # BitNet workload (5 variants)
├── rtl/
│   ├── mx_coprocessor.sv          # Unified MX coprocessor (4,860 LUT)
│   ├── croc_cw305.sv              # Original CW305 wrapper
│   ├── croc_cw305_live.sv         # Live state monitor wrapper
│   ├── utilization_cw305_live.rpt # Post-place utilization report
│   └── timing_summary_cw305_live.rpt # Post-route timing report
└── datasets/
    ├── dvfs_qnn/                   # 570 rows (5 × 114)
    ├── dvfs_bitnet/                # 570 rows (5 × 114)
    └── clock_glitch/               # 900 rows (10 × 90)
```

## Reproducing

```bash
# DVFS on QNN (J16=0, CW305 only)
python3 scripts/dvfs_5format_sweep.py

# DVFS on BitNet (J16=0, requires patched bitstreams)
python3 scripts/dvfs_bitnet_5format.py

# Clock glitch on QNN (J16=1, CW-Lite + CW305)
python3 scripts/clkglitch_qnn_5format.py
```

## Authors

Dillibabu Shanmugam, Patrick Schaumont — Worcester Polytechnic Institute
