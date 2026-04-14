// qnn_llm_mxint8.c — Quantized Transformer Decoder (MXINT8)
//
// QNN-LLM: 2-layer GPT-style language model, fully quantized 8-bit
// Architecture: embed + pos_enc → 2x(multi-head attn + FFN) → LM head
// Features: 2-head attention, KV cache, BN, ReLU, autoregressive decode
//
// MX coprocessor ops used: DOT4, ADD4, RELU4, MUL4, SET_SE
// IMEM budget: ~2180B weights + ~1800B code ≈ 4KB
//
// Authors: Dillibabu Shanmugam, Patrick Schaumont (WPI)

#include "uart.h"
#include "print.h"
#include "mx.h"
#include "weights/weights_mxint8_qnn_llm.h"

// ── KV cache (DMEM) ──
// [layer][position] → 2 packed words (8 int8 elements)
static uint32_t kv_k[QNN_NLAYERS][QNN_CTXLEN][QNN_DW];
static uint32_t kv_v[QNN_NLAYERS][QNN_CTXLEN][QNN_DW];

// ── Helpers ──

static inline uint8_t clip8(int32_t v) {
    if (v > 127) v = 127;
    if (v < -128) v = -128;
    return (uint8_t)(v & 0xFF);
}

// 8-element dot product via 2x DOT4
static inline int32_t dot8_int8(const uint32_t x[2], const uint32_t w[2]) {
    uint32_t d0, d1;
    MXINT8_DOT4(d0, x[0], w[0]);
    MXINT8_DOT4(d1, x[1], w[1]);
    return (int32_t)d0 + (int32_t)d1;
}

// 8x8 matrix-vector: out[8] = W[8x8] * x[8], requantized to int8
// W stored as 16 words: row i → W[2*i], W[2*i+1]
static void matvec8_int8(const uint32_t *W, const uint32_t x[2],
                         uint32_t out[2], int shift) {
    int32_t r[8];
    for (int i = 0; i < 8; i++)
        r[i] = dot8_int8(x, &W[i * 2]);
    out[0] = MX_PACK4(clip8(r[0] >> shift), clip8(r[1] >> shift),
                       clip8(r[2] >> shift), clip8(r[3] >> shift));
    out[1] = MX_PACK4(clip8(r[4] >> shift), clip8(r[5] >> shift),
                       clip8(r[6] >> shift), clip8(r[7] >> shift));
}

// Apply quantized batch-norm: out = MUL4(x, scale) then ADD4(+bias)
// bn[0]=scale_lo, bn[1]=scale_hi, bn[2]=bias_lo, bn[3]=bias_hi
static inline void bn8_int8(uint32_t x[2], const uint32_t *bn) {
    uint32_t s0, s1;
    MXINT8_MUL4(s0, x[0], bn[0]);
    MXINT8_MUL4(s1, x[1], bn[1]);
    MXINT8_ADD4(x[0], s0, bn[2]);
    MXINT8_ADD4(x[1], s1, bn[3]);
}

#define GPIO_DIR  (*(volatile uint32_t *)0x03005000)
#define GPIO_EN   (*(volatile uint32_t *)0x03005080)
#define GPIO_OUT  (*(volatile uint32_t *)0x03005180)
static inline void trigger_init(void) { GPIO_DIR |= 1u; GPIO_EN |= 1u; }
static inline void trigger_high(void) { GPIO_OUT |=  1u; }
static inline void trigger_low(void)  { GPIO_OUT &= ~1u; }

int main() {
    uart_init();
    trigger_init();
    printf("QNN_LLM_MXINT8\n");
    uart_write_flush();

    uint32_t t0 = mx_rdcycle();
    uint32_t tmp;

    trigger_high();
    MX_SET_SE(tmp, MX_SE_PAIR(MX_SE_NEUTRAL, MX_SE_NEUTRAL));
    trigger_low();

    const uint32_t *W = qw_mxint8;

    // Load prompt tokens
    uint8_t tokens[QNN_CTXLEN];
    uint32_t prompt_packed = W[QW_PROMPT];
    for (int i = 0; i < QNN_PLEN; i++)
        tokens[i] = MX_LANE(prompt_packed, i);

    // ══════════════════════════════════════════════════════════
    // Autoregressive generation: process prompt + generate
    // ══════════════════════════════════════════════════════════
    for (int pos = 0; pos < QNN_PLEN + QNN_GLEN; pos++) {
        uint8_t tok = tokens[pos];

        // ── Token embedding + position encoding ──
        // x[2] = emb[tok] + pos_enc[pos]  (8-dim, 2 packed words)
        uint32_t x[2];
        MXINT8_ADD4(x[0], W[QW_EMB + tok * QNN_DW + 0],
                          W[QW_POS + pos * QNN_DW + 0]);
        MXINT8_ADD4(x[1], W[QW_EMB + tok * QNN_DW + 1],
                          W[QW_POS + pos * QNN_DW + 1]);

        // ── 2 transformer decoder layers ──
        for (int l = 0; l < QNN_NLAYERS; l++) {
            int lb = QW_L(l);  // layer base offset

            uint32_t residual[2] = {x[0], x[1]};

            // ── Q, K, V projections (8x8 matvec each) ──
            uint32_t q[2], k[2], v[2];
            matvec8_int8(&W[lb + QL_WQ], x, q, 2);
            matvec8_int8(&W[lb + QL_WK], x, k, 2);
            matvec8_int8(&W[lb + QL_WV], x, v, 2);

            // Store K, V in cache
            kv_k[l][pos][0] = k[0]; kv_k[l][pos][1] = k[1];
            kv_v[l][pos][0] = v[0]; kv_v[l][pos][1] = v[1];

            // ── Multi-head self-attention (2 heads, head_dim=4) ──
            // Head 0 operates on low word [0], Head 1 on high word [1]
            uint32_t attn_out[2];
            for (int h = 0; h < QNN_NHEADS; h++) {
                // Attention scores: q_h · k_h[0..pos]
                int32_t scores[QNN_CTXLEN];
                int32_t max_s = -32768;
                for (int p = 0; p <= pos; p++) {
                    uint32_t d;
                    MXINT8_DOT4(d, q[h], kv_k[l][p][h]);
                    scores[p] = (int32_t)d;
                    if (scores[p] > max_s) max_s = scores[p];
                }

                // Softmax (controlled linear, identical across formats)
                uint32_t aw[QNN_CTXLEN];
                uint32_t esum = 0;
                for (int p = 0; p <= pos; p++) {
                    int32_t sh = scores[p] - max_s + 64;
                    if (sh < 0) sh = 0;
                    if (sh > 127) sh = 127;
                    aw[p] = (uint32_t)sh;
                    esum += aw[p];
                }
                if (esum > 0)
                    for (int p = 0; p <= pos; p++)
                        aw[p] = (aw[p] << 8) / esum;

                // Weighted sum of values for this head
                int32_t ctx[QNN_HDIM];
                for (int d = 0; d < QNN_HDIM; d++) ctx[d] = 0;
                for (int p = 0; p <= pos; p++) {
                    for (int d = 0; d < QNN_HDIM; d++) {
                        int8_t vd = (int8_t)MX_LANE(kv_v[l][p][h], d);
                        ctx[d] += ((int32_t)aw[p] * vd) >> 8;
                    }
                }
                attn_out[h] = MX_PACK4(clip8(ctx[0]), clip8(ctx[1]),
                                        clip8(ctx[2]), clip8(ctx[3]));
            }

            // ── Output projection (8x8) ──
            uint32_t o[2];
            matvec8_int8(&W[lb + QL_WO], attn_out, o, 2);

            // ── Batch-norm (attention) ──
            bn8_int8(o, &W[lb + QL_BNA]);

            // ── Residual connection ──
            MXINT8_ADD4(x[0], residual[0], o[0]);
            MXINT8_ADD4(x[1], residual[1], o[1]);

            uint32_t ffn_res[2] = {x[0], x[1]};

            // ── FFN up-project: 8 → 32 with ReLU ──
            int32_t hidden[QNN_DFFN];
            for (int j = 0; j < QNN_DFFN; j++) {
                hidden[j] = dot8_int8(x, &W[lb + QL_WUP + j * QNN_DW]);
                if (hidden[j] < 0) hidden[j] = 0;  // ReLU
            }

            // ── FFN down-project: 32 → 8 ──
            // 8 outputs, each dot product over 32 hidden (8 groups of 4)
            int32_t ffn[8];
            for (int d = 0; d < QNN_DMODEL; d++) {
                ffn[d] = 0;
                for (int g = 0; g < QNN_DFFN / 4; g++) {
                    uint32_t hp = MX_PACK4(
                        clip8(hidden[g*4+0] >> 2), clip8(hidden[g*4+1] >> 2),
                        clip8(hidden[g*4+2] >> 2), clip8(hidden[g*4+3] >> 2));
                    uint32_t dot;
                    MXINT8_DOT4(dot, hp, W[lb + QL_WDN + d * (QNN_DFFN/4) + g]);
                    ffn[d] += (int32_t)dot;
                }
            }

            // Requantize FFN output
            uint32_t f[2];
            f[0] = MX_PACK4(clip8(ffn[0] >> 2), clip8(ffn[1] >> 2),
                             clip8(ffn[2] >> 2), clip8(ffn[3] >> 2));
            f[1] = MX_PACK4(clip8(ffn[4] >> 2), clip8(ffn[5] >> 2),
                             clip8(ffn[6] >> 2), clip8(ffn[7] >> 2));

            // ── Batch-norm (FFN) ──
            bn8_int8(f, &W[lb + QL_BNF]);

            // ── FFN residual ──
            MXINT8_ADD4(x[0], ffn_res[0], f[0]);
            MXINT8_ADD4(x[1], ffn_res[1], f[1]);
        }

        // ── LM head: score 32 vocab tokens, pick best ──
        if (pos >= QNN_PLEN - 1 && pos < QNN_PLEN + QNN_GLEN - 1) {
            int32_t best_score = -2147483647;
            uint8_t best_tok = 0;
            for (int t = 0; t < QNN_VOCAB; t++) {
                int32_t score = dot8_int8(x, &W[QW_LM + t * QNN_DW]);
                if (score > best_score) {
                    best_score = score;
                    best_tok = (uint8_t)t;
                }
            }
            tokens[pos + 1] = best_tok;
        }
    }

    uint32_t t1 = mx_rdcycle();

    // ── Output ──
    printf("PROMPT:");
    for (int i = 0; i < QNN_PLEN; i++)
        printf(" %d", tokens[i]);
    printf("\n");
    printf("PREDICT:");
    for (int i = 0; i < QNN_GLEN; i++)
        printf(" %d", tokens[QNN_PLEN + i]);
    printf("\n");
    printf("GEN:");
    for (int i = 0; i < QNN_GLEN; i++)
        printf("0x%x ", tokens[QNN_PLEN + i]);
    printf("\n");
    printf("CYCLES:0x%x\n", t1 - t0);
    printf("TOK0:0x%x\n", tokens[QNN_PLEN + 0]);
    printf("TOK1:0x%x\n", tokens[QNN_PLEN + 1]);
    printf("TOK2:0x%x\n", tokens[QNN_PLEN + 2]);
    printf("TOK3:0x%x\n", tokens[QNN_PLEN + 3]);
    uart_write_flush();

    return tokens[QNN_PLEN];
}
