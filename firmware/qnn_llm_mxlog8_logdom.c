// qnn_llm_mxlog8_logdom.c — Quantized Transformer Decoder (MXLOG8-Logdomain)
//
// QNN-LLM: 2-layer GPT-style language model, MXLOG8 log-domain arithmetic
// Key difference: LOGDOM DOT4 stays in log-domain (multiply=add, accum=log_add)
// CVT_LIN converts back to linear for softmax / re-quantization
//
// Authors: Dillibabu Shanmugam, Patrick Schaumont (WPI)

#include "uart.h"
#include "print.h"
#include "mx.h"
#include "weights/weights_mxlog8_logdom_qnn_llm.h"

static uint32_t kv_k[QNN_NLAYERS][QNN_CTXLEN][QNN_DW];
static uint32_t kv_v[QNN_NLAYERS][QNN_CTXLEN][QNN_DW];

static inline uint8_t clip8(int32_t v) {
    if (v > 127) v = 127;
    if (v < -128) v = -128;
    return (uint8_t)(v & 0xFF);
}

static inline uint8_t requant_logdom(int32_t v, int shift) {
    int32_t scaled = v >> shift;
    int sign = 0;
    if (scaled < 0) { sign = 1; scaled = -scaled; }
    uint32_t enc;
    MXLOGDOM_CVT_ENC(enc, (uint32_t)scaled);
    return (sign << 7) | (enc & 0x7F);
}

static inline int32_t dot8_logdom(const uint32_t x[2], const uint32_t w[2]) {
    uint32_t d0, d1;
    MXLOGDOM_DOT4(d0, x[0], w[0]);
    MXLOGDOM_DOT4(d1, x[1], w[1]);
    return (int32_t)d0 + (int32_t)d1;
}

static void matvec8_logdom(const uint32_t *W, const uint32_t x[2],
                            uint32_t out[2], int shift) {
    int32_t r[8];
    for (int i = 0; i < 8; i++)
        r[i] = dot8_logdom(x, &W[i * 2]);
    out[0] = MX_PACK4(requant_logdom(r[0],shift), requant_logdom(r[1],shift),
                       requant_logdom(r[2],shift), requant_logdom(r[3],shift));
    out[1] = MX_PACK4(requant_logdom(r[4],shift), requant_logdom(r[5],shift),
                       requant_logdom(r[6],shift), requant_logdom(r[7],shift));
}

static inline void bn8_logdom(uint32_t x[2], const uint32_t *bn) {
    uint32_t s0, s1;
    MXLOGDOM_MUL4(s0, x[0], bn[0]); MXLOGDOM_MUL4(s1, x[1], bn[1]);
    MXLOGDOM_ADD4(x[0], s0, bn[2]); MXLOGDOM_ADD4(x[1], s1, bn[3]);
}

int main() {
    uart_init();
    printf("QNN_LLM_MXLOG8_LOGDOM\n");
    uart_write_flush();

    uint32_t t0 = mx_rdcycle();
    uint32_t tmp;
    MX_SET_SE(tmp, MX_SE_PAIR(MX_SE_NEUTRAL, MX_SE_NEUTRAL));
    const uint32_t *W = qw_mxlog8_logdom;

    uint8_t tokens[QNN_CTXLEN];
    uint32_t pp = W[QW_PROMPT];
    for (int i = 0; i < QNN_PLEN; i++) tokens[i] = MX_LANE(pp, i);

    for (int pos = 0; pos < QNN_PLEN + QNN_GLEN; pos++) {
        uint8_t tok = tokens[pos];
        uint32_t x[2];
        MXLOGDOM_ADD4(x[0], W[QW_EMB+tok*QNN_DW], W[QW_POS+pos*QNN_DW]);
        MXLOGDOM_ADD4(x[1], W[QW_EMB+tok*QNN_DW+1], W[QW_POS+pos*QNN_DW+1]);

        for (int l = 0; l < QNN_NLAYERS; l++) {
            int lb = QW_L(l);
            uint32_t residual[2] = {x[0], x[1]};

            uint32_t q[2], k[2], v[2];
            matvec8_logdom(&W[lb+QL_WQ], x, q, 2);
            matvec8_logdom(&W[lb+QL_WK], x, k, 2);
            matvec8_logdom(&W[lb+QL_WV], x, v, 2);
            kv_k[l][pos][0]=k[0]; kv_k[l][pos][1]=k[1];
            kv_v[l][pos][0]=v[0]; kv_v[l][pos][1]=v[1];

            uint32_t attn_out[2];
            for (int h = 0; h < QNN_NHEADS; h++) {
                int32_t scores[QNN_CTXLEN]; int32_t max_s = -32768;
                for (int p = 0; p <= pos; p++) {
                    uint32_t d;
                    MXLOGDOM_DOT4(d, q[h], kv_k[l][p][h]);
                    scores[p] = (int32_t)d;
                    if (scores[p] > max_s) max_s = scores[p];
                }
                uint32_t aw[QNN_CTXLEN]; uint32_t esum = 0;
                for (int p = 0; p <= pos; p++) {
                    int32_t sh = scores[p] - max_s + 64;
                    if (sh < 0) sh = 0; if (sh > 127) sh = 127;
                    aw[p] = (uint32_t)sh; esum += aw[p];
                }
                if (esum > 0)
                    for (int p = 0; p <= pos; p++)
                        aw[p] = (aw[p] << 8) / esum;

                // Weighted sum: convert V from log-domain to linear via CVT_LIN
                int32_t ctx[QNN_HDIM];
                for (int d = 0; d < QNN_HDIM; d++) ctx[d] = 0;
                for (int p = 0; p <= pos; p++) {
                    uint32_t val = kv_v[l][p][h];
                    for (int d = 0; d < QNN_HDIM; d++) {
                        uint8_t vb = MX_LANE(val, d);
                        uint32_t lin;
                        MXLOGDOM_CVT_LIN(lin, (uint32_t)vb);
                        int32_t lv = (int32_t)(lin & 0xFFFF);
                        if (vb & 0x80) lv = -lv;
                        ctx[d] += ((int32_t)aw[p] * lv) >> 8;
                    }
                }
                attn_out[h] = MX_PACK4(
                    requant_logdom(ctx[0],0), requant_logdom(ctx[1],0),
                    requant_logdom(ctx[2],0), requant_logdom(ctx[3],0));
            }

            uint32_t o[2];
            matvec8_logdom(&W[lb+QL_WO], attn_out, o, 2);
            bn8_logdom(o, &W[lb+QL_BNA]);
            MXLOGDOM_ADD4(x[0], residual[0], o[0]);
            MXLOGDOM_ADD4(x[1], residual[1], o[1]);

            uint32_t ffn_res[2] = {x[0], x[1]};
            int32_t hidden[QNN_DFFN];
            for (int j = 0; j < QNN_DFFN; j++) {
                hidden[j] = dot8_logdom(x, &W[lb+QL_WUP+j*QNN_DW]);
                if (hidden[j] < 0) hidden[j] = 0;
            }
            int32_t ffn[8];
            for (int d = 0; d < QNN_DMODEL; d++) {
                ffn[d] = 0;
                for (int g = 0; g < QNN_DFFN/4; g++) {
                    uint32_t hp = MX_PACK4(
                        requant_logdom(hidden[g*4],2), requant_logdom(hidden[g*4+1],2),
                        requant_logdom(hidden[g*4+2],2), requant_logdom(hidden[g*4+3],2));
                    uint32_t dot;
                    MXLOGDOM_DOT4(dot, hp, W[lb+QL_WDN+d*(QNN_DFFN/4)+g]);
                    ffn[d] += (int32_t)dot;
                }
            }
            uint32_t f[2];
            f[0] = MX_PACK4(requant_logdom(ffn[0],2), requant_logdom(ffn[1],2),
                             requant_logdom(ffn[2],2), requant_logdom(ffn[3],2));
            f[1] = MX_PACK4(requant_logdom(ffn[4],2), requant_logdom(ffn[5],2),
                             requant_logdom(ffn[6],2), requant_logdom(ffn[7],2));
            bn8_logdom(f, &W[lb+QL_BNF]);
            MXLOGDOM_ADD4(x[0], ffn_res[0], f[0]);
            MXLOGDOM_ADD4(x[1], ffn_res[1], f[1]);
        }

        if (pos >= QNN_PLEN-1 && pos < QNN_PLEN+QNN_GLEN-1) {
            int32_t best_s = -2147483647; uint8_t best_t = 0;
            for (int t = 0; t < QNN_VOCAB; t++) {
                int32_t s = dot8_logdom(x, &W[QW_LM+t*QNN_DW]);
                if (s > best_s) { best_s = s; best_t = t; }
            }
            tokens[pos+1] = best_t;
        }
    }

    uint32_t t1 = mx_rdcycle();
    printf("PROMPT:");
    for (int i = 0; i < QNN_PLEN; i++)
        printf(" %d", tokens[i]);
    printf("\n");
    printf("PREDICT:");
    for (int i = 0; i < QNN_GLEN; i++)
        printf(" %d", tokens[QNN_PLEN+i]);
    printf("\n");
    printf("GEN:");
    for (int i = 0; i < QNN_GLEN; i++) printf("0x%x ", tokens[QNN_PLEN+i]);
    printf("\n");
    printf("CYCLES:0x%x\n", t1-t0);
    printf("TOK0:0x%x\n", tokens[QNN_PLEN+0]);
    printf("TOK1:0x%x\n", tokens[QNN_PLEN+1]);
    printf("TOK2:0x%x\n", tokens[QNN_PLEN+2]);
    printf("TOK3:0x%x\n", tokens[QNN_PLEN+3]);
    uart_write_flush();
    return tokens[QNN_PLEN];
}
