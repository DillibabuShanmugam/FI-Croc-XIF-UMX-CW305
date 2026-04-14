// mx.h — Unified MX Coprocessor inline assembly macros for RISC-V
//
// Single custom-0 opcode (0x0B) with format encoded in funct7[4:2].
// Supports all 5 OCP MX element formats through one CV-X-IF port.
//
// Instruction encoding (R-type):
//   31:25  24:20 19:15 14:12 11:7  6:0
//   funct7  rs2   rs1  funct3  rd  0001011
//
// funct7 = {2'b00, format[2:0], 1'b0, group}
// Operation = {group, funct3} → 4-bit selector
//
// Authors: Dillibabu Shanmugam, Patrick Schaumont (WPI)

#ifndef MX_H
#define MX_H

#include <stdint.h>

// =====================================================================
// Pack / unpack helpers
// =====================================================================

// Pack 4 elements (bytes) into a 32-bit register
#define MX_PACK4(e0, e1, e2, e3) \
    ((uint32_t)(uint8_t)(e0)       | ((uint32_t)(uint8_t)(e1) << 8) | \
     ((uint32_t)(uint8_t)(e2) << 16) | ((uint32_t)(uint8_t)(e3) << 24))

// Pack shared exponent pair: se_a in [7:0], se_b in [15:8]
#define MX_SE_PAIR(se_a, se_b) \
    ((uint32_t)(uint8_t)(se_a) | ((uint32_t)(uint8_t)(se_b) << 8))

// Extract lane i (0-3) from packed 32-bit register
#define MX_LANE(packed, i) (((packed) >> ((i) * 8)) & 0xFF)

// Neutral shared exponent (2^(127-127) = 2^0 = 1, no scaling)
#define MX_SE_NEUTRAL  127

// Sign-extend 8-bit two's complement to 32-bit signed
static inline int32_t mx_int8_decode(uint8_t v) {
    return (int32_t)(int8_t)v;
}

// Simple argmax over int32_t array
static inline int mx_argmax_i32(const int32_t *arr, int n) {
    int best = 0;
    for (int i = 1; i < n; i++) {
        if (arr[i] > arr[best]) best = i;
    }
    return best;
}

// Read mcycle CSR (cycle counter)
static inline uint32_t mx_rdcycle(void) {
    uint32_t c;
    __asm__ volatile ("csrr %0, mcycle" : "=r"(c));
    return c;
}

// =====================================================================
// funct7 values for each format
// =====================================================================
//
//  Format         funct7_base (group=0)  funct7_ext (group=1)
//  ─────────────  ────────────────────   ────────────────────
//  MXINT8          0x00                   0x01
//  MXFP8-E4M3     0x04                   0x05
//  MXFP8-E5M2     0x08                   0x09
//  MXLOG8          0x0C                   0x0D
//  MXLOG8-LOGDOM   0x10                   0x11

// =====================================================================
// MXINT8 — format=000, funct7: base=0x00, ext=0x01
// Element: 1S.7I (signed 8-bit integer, two's complement)
// =====================================================================

// Base operations (group=0)
#define MXINT8_DOT4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 0, 0x00, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXINT8_MUL4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 1, 0x00, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXINT8_ADD4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 2, 0x00, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXINT8_SUB4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 3, 0x00, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXINT8_RELU4(rd, rs1)      __asm__ volatile (".insn r 0x0B, 4, 0x00, %0, %1, x0" : "=r"(rd) : "r"(rs1))
#define MXINT8_ABS4(rd, rs1)       __asm__ volatile (".insn r 0x0B, 5, 0x00, %0, %1, x0" : "=r"(rd) : "r"(rs1))
#define MXINT8_MIN4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 6, 0x00, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXINT8_MAX4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 7, 0x00, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))

// Extended operations (group=1)
#define MXINT8_MACS(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 0, 0x01, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXINT8_MACC(rd)            __asm__ volatile (".insn r 0x0B, 1, 0x01, %0, x0, x0"  : "=r"(rd))
#define MXINT8_SET_SE(rd, rs1)     __asm__ volatile (".insn r 0x0B, 2, 0x01, %0, %1, x0"  : "=r"(rd) : "r"(rs1))
#define MXINT8_DECODE(rd, rs1, rs2) __asm__ volatile (".insn r 0x0B, 3, 0x01, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXINT8_CVT_LIN(rd, rs1)    __asm__ volatile (".insn r 0x0B, 4, 0x01, %0, %1, x0"  : "=r"(rd) : "r"(rs1))
#define MXINT8_CVT_ENC(rd, rs1)    __asm__ volatile (".insn r 0x0B, 5, 0x01, %0, %1, x0"  : "=r"(rd) : "r"(rs1))
#define MXINT8_CMP4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 6, 0x01, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXINT8_MACC_RD(rd)         __asm__ volatile (".insn r 0x0B, 7, 0x01, %0, x0, x0"  : "=r"(rd))

// =====================================================================
// MXFP8_E4M3 — format=001, funct7: base=0x04, ext=0x05
// Element: 1S.4E.3M (float, bias=7, no Inf, NaN=0xFF, max=448)
// =====================================================================

#define MXE4M3_DOT4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 0, 0x04, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXE4M3_MUL4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 1, 0x04, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXE4M3_ADD4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 2, 0x04, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXE4M3_SUB4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 3, 0x04, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXE4M3_RELU4(rd, rs1)      __asm__ volatile (".insn r 0x0B, 4, 0x04, %0, %1, x0" : "=r"(rd) : "r"(rs1))
#define MXE4M3_ABS4(rd, rs1)       __asm__ volatile (".insn r 0x0B, 5, 0x04, %0, %1, x0" : "=r"(rd) : "r"(rs1))
#define MXE4M3_MIN4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 6, 0x04, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXE4M3_MAX4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 7, 0x04, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))

#define MXE4M3_MACS(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 0, 0x05, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXE4M3_MACC(rd)            __asm__ volatile (".insn r 0x0B, 1, 0x05, %0, x0, x0"  : "=r"(rd))
#define MXE4M3_SET_SE(rd, rs1)     __asm__ volatile (".insn r 0x0B, 2, 0x05, %0, %1, x0"  : "=r"(rd) : "r"(rs1))
#define MXE4M3_DECODE(rd, rs1, rs2) __asm__ volatile (".insn r 0x0B, 3, 0x05, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXE4M3_CVT_LIN(rd, rs1)    __asm__ volatile (".insn r 0x0B, 4, 0x05, %0, %1, x0"  : "=r"(rd) : "r"(rs1))
#define MXE4M3_CVT_ENC(rd, rs1)    __asm__ volatile (".insn r 0x0B, 5, 0x05, %0, %1, x0"  : "=r"(rd) : "r"(rs1))
#define MXE4M3_CMP4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 6, 0x05, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXE4M3_MACC_RD(rd)         __asm__ volatile (".insn r 0x0B, 7, 0x05, %0, x0, x0"  : "=r"(rd))

// =====================================================================
// MXFP8_E5M2 — format=010, funct7: base=0x08, ext=0x09
// Element: 1S.5E.2M (float, bias=15, has Inf/NaN, max=57344)
// =====================================================================

#define MXE5M2_DOT4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 0, 0x08, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXE5M2_MUL4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 1, 0x08, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXE5M2_ADD4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 2, 0x08, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXE5M2_SUB4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 3, 0x08, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXE5M2_RELU4(rd, rs1)      __asm__ volatile (".insn r 0x0B, 4, 0x08, %0, %1, x0" : "=r"(rd) : "r"(rs1))
#define MXE5M2_ABS4(rd, rs1)       __asm__ volatile (".insn r 0x0B, 5, 0x08, %0, %1, x0" : "=r"(rd) : "r"(rs1))
#define MXE5M2_MIN4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 6, 0x08, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXE5M2_MAX4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 7, 0x08, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))

#define MXE5M2_MACS(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 0, 0x09, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXE5M2_MACC(rd)            __asm__ volatile (".insn r 0x0B, 1, 0x09, %0, x0, x0"  : "=r"(rd))
#define MXE5M2_SET_SE(rd, rs1)     __asm__ volatile (".insn r 0x0B, 2, 0x09, %0, %1, x0"  : "=r"(rd) : "r"(rs1))
#define MXE5M2_DECODE(rd, rs1, rs2) __asm__ volatile (".insn r 0x0B, 3, 0x09, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXE5M2_CVT_LIN(rd, rs1)    __asm__ volatile (".insn r 0x0B, 4, 0x09, %0, %1, x0"  : "=r"(rd) : "r"(rs1))
#define MXE5M2_CVT_ENC(rd, rs1)    __asm__ volatile (".insn r 0x0B, 5, 0x09, %0, %1, x0"  : "=r"(rd) : "r"(rs1))
#define MXE5M2_CMP4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 6, 0x09, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXE5M2_MACC_RD(rd)         __asm__ volatile (".insn r 0x0B, 7, 0x09, %0, x0, x0"  : "=r"(rd))

// =====================================================================
// MXLOG8 — format=011, funct7: base=0x0C, ext=0x0D
// Element: 1S.4FI.3FE (logarithmic, LUT-based fractional decode)
// =====================================================================

#define MXLOG8_DOT4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 0, 0x0C, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXLOG8_MUL4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 1, 0x0C, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXLOG8_ADD4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 2, 0x0C, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXLOG8_SUB4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 3, 0x0C, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXLOG8_RELU4(rd, rs1)      __asm__ volatile (".insn r 0x0B, 4, 0x0C, %0, %1, x0" : "=r"(rd) : "r"(rs1))
#define MXLOG8_ABS4(rd, rs1)       __asm__ volatile (".insn r 0x0B, 5, 0x0C, %0, %1, x0" : "=r"(rd) : "r"(rs1))
#define MXLOG8_MIN4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 6, 0x0C, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXLOG8_MAX4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 7, 0x0C, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))

#define MXLOG8_MACS(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 0, 0x0D, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXLOG8_MACC(rd)            __asm__ volatile (".insn r 0x0B, 1, 0x0D, %0, x0, x0"  : "=r"(rd))
#define MXLOG8_SET_SE(rd, rs1)     __asm__ volatile (".insn r 0x0B, 2, 0x0D, %0, %1, x0"  : "=r"(rd) : "r"(rs1))
#define MXLOG8_DECODE(rd, rs1, rs2) __asm__ volatile (".insn r 0x0B, 3, 0x0D, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXLOG8_CVT_LIN(rd, rs1)    __asm__ volatile (".insn r 0x0B, 4, 0x0D, %0, %1, x0"  : "=r"(rd) : "r"(rs1))
#define MXLOG8_CVT_ENC(rd, rs1)    __asm__ volatile (".insn r 0x0B, 5, 0x0D, %0, %1, x0"  : "=r"(rd) : "r"(rs1))
#define MXLOG8_CMP4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 6, 0x0D, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXLOG8_MACC_RD(rd)         __asm__ volatile (".insn r 0x0B, 7, 0x0D, %0, x0, x0"  : "=r"(rd))

// =====================================================================
// MXLOG8_LOGDOM — format=100, funct7: base=0x10, ext=0x11
// Same element encoding as MXLOG8, but DOT4/MACS stay in log-domain
// =====================================================================

#define MXLOGDOM_DOT4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 0, 0x10, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXLOGDOM_MUL4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 1, 0x10, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXLOGDOM_ADD4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 2, 0x10, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXLOGDOM_SUB4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 3, 0x10, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXLOGDOM_RELU4(rd, rs1)      __asm__ volatile (".insn r 0x0B, 4, 0x10, %0, %1, x0" : "=r"(rd) : "r"(rs1))
#define MXLOGDOM_ABS4(rd, rs1)       __asm__ volatile (".insn r 0x0B, 5, 0x10, %0, %1, x0" : "=r"(rd) : "r"(rs1))
#define MXLOGDOM_MIN4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 6, 0x10, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXLOGDOM_MAX4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 7, 0x10, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))

#define MXLOGDOM_MACS(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 0, 0x11, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXLOGDOM_MACC(rd)            __asm__ volatile (".insn r 0x0B, 1, 0x11, %0, x0, x0"  : "=r"(rd))
#define MXLOGDOM_SET_SE(rd, rs1)     __asm__ volatile (".insn r 0x0B, 2, 0x11, %0, %1, x0"  : "=r"(rd) : "r"(rs1))
#define MXLOGDOM_DECODE(rd, rs1, rs2) __asm__ volatile (".insn r 0x0B, 3, 0x11, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXLOGDOM_CVT_LIN(rd, rs1)    __asm__ volatile (".insn r 0x0B, 4, 0x11, %0, %1, x0"  : "=r"(rd) : "r"(rs1))
#define MXLOGDOM_CVT_ENC(rd, rs1)    __asm__ volatile (".insn r 0x0B, 5, 0x11, %0, %1, x0"  : "=r"(rd) : "r"(rs1))
#define MXLOGDOM_CMP4(rd, rs1, rs2)  __asm__ volatile (".insn r 0x0B, 6, 0x11, %0, %1, %2" : "=r"(rd) : "r"(rs1), "r"(rs2))
#define MXLOGDOM_MACC_RD(rd)         __asm__ volatile (".insn r 0x0B, 7, 0x11, %0, x0, x0"  : "=r"(rd))

// =====================================================================
// Format-agnostic control operations
// SET_SE, MACC, MACC_RD behavior is identical across formats.
// These aliases use MXINT8 encoding for convenience.
// =====================================================================

#define MX_SET_SE(rd, se_pair)  MXINT8_SET_SE(rd, se_pair)
#define MX_MACC(rd)             MXINT8_MACC(rd)
#define MX_MACC_RD(rd)          MXINT8_MACC_RD(rd)

#endif // MX_H
