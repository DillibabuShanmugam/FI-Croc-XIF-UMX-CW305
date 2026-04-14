// tiny_llm_mxint8_fi.c — TinyLLM for FI campaigns (MXINT8)
// Fixed copy with compatible macro names for v2 weight header
//
// Architecture: 2-layer transformer decoder, d_model=8, 2-head
// Prompt: 4 tokens in, 4 tokens generated
//
// Authors: Dillibabu Shanmugam, Patrick Schaumont (WPI)

#include "uart.h"
#include "print.h"
#include "mx.h"
#include "weights/weights_mxint8_llm.h"

#define LLM_N_LAYERS   LLM_NLAYERS
#define LLM_CTX_LEN    LLM_CTXLEN
#define LLM_PROMPT_LEN LLM_PROMPTLEN
#define LLM_GEN_LEN    LLM_GENLEN
#define LLM_D_MODEL    LLM_DMODEL
#define LLM_D_FFN      LLM_DFFN
#define LLM_VOCAB_SIZE LLM_VOCAB

#define OFF_EMB        LLM_OFF_EMB
#define OFF_WQ(l)      (LLM_OFF_L(l) + LO_WQ)
#define OFF_WK(l)      (LLM_OFF_L(l) + LO_WK)
#define OFF_WV(l)      (LLM_OFF_L(l) + LO_WV)
#define OFF_WO(l)      (LLM_OFF_L(l) + LO_WO)
#define OFF_WUP(l)     (LLM_OFF_L(l) + LO_WUP)
#define OFF_WDN(l)     (LLM_OFF_L(l) + LO_WDN)
#define OFF_LM_HEAD    LLM_OFF_LM

static uint32_t kv_k[LLM_N_LAYERS][LLM_CTX_LEN];
static uint32_t kv_v[LLM_N_LAYERS][LLM_CTX_LEN];

static inline uint8_t clip8(int32_t v) {
    if (v > 127) v = 127;
    if (v < -128) v = -128;
    return (uint8_t)(v & 0xFF);
}

static inline uint32_t matvec4_int8(const uint32_t *w, uint32_t x, int shift) {
    uint32_t d0, d1, d2, d3;
    MXINT8_DOT4(d0, x, w[0]);
    MXINT8_DOT4(d1, x, w[1]);
    MXINT8_DOT4(d2, x, w[2]);
    MXINT8_DOT4(d3, x, w[3]);
    return MX_PACK4(
        clip8((int32_t)d0 >> shift),
        clip8((int32_t)d1 >> shift),
        clip8((int32_t)d2 >> shift),
        clip8((int32_t)d3 >> shift)
    );
}

int main() {
    uart_init();
    printf("TINY_LLM_MXINT8_FI\n");
    uart_write_flush();

    uint32_t t0 = mx_rdcycle();
    uint32_t tmp;

    MX_SET_SE(tmp, MX_SE_PAIR(MX_SE_NEUTRAL, MX_SE_NEUTRAL));

    const uint32_t *W = llm_w_mxint8;

    uint8_t tokens[LLM_CTX_LEN];
    for (int i = 0; i < LLM_PROMPT_LEN; i++)
        tokens[i] = llm_prompt[i];

    for (int pos = 0; pos < LLM_PROMPT_LEN + LLM_GEN_LEN; pos++) {
        uint8_t tok = tokens[pos];
        uint32_t x = W[OFF_EMB + tok];

        for (int l = 0; l < LLM_N_LAYERS; l++) {
            uint32_t x_residual = x;

            uint32_t q = matvec4_int8(&W[OFF_WQ(l)], x, 2);
            uint32_t k = matvec4_int8(&W[OFF_WK(l)], x, 2);
            uint32_t v = matvec4_int8(&W[OFF_WV(l)], x, 2);

            kv_k[l][pos] = k;
            kv_v[l][pos] = v;

            int32_t attn_scores[LLM_CTX_LEN];
            int32_t max_score = -32768;
            for (int p = 0; p <= pos; p++) {
                uint32_t dot;
                MXINT8_DOT4(dot, q, kv_k[l][p]);
                attn_scores[p] = (int32_t)dot;
                if (attn_scores[p] > max_score)
                    max_score = attn_scores[p];
            }

            uint32_t attn_w[LLM_CTX_LEN];
            uint32_t exp_sum = 0;
            for (int p = 0; p <= pos; p++) {
                int32_t shifted = attn_scores[p] - max_score + 64;
                if (shifted < 0) shifted = 0;
                if (shifted > 127) shifted = 127;
                attn_w[p] = (uint32_t)shifted;
                exp_sum += attn_w[p];
            }
            if (exp_sum > 0) {
                for (int p = 0; p <= pos; p++)
                    attn_w[p] = (attn_w[p] << 8) / exp_sum;
            }

            int32_t context[LLM_D_MODEL];
            for (int d = 0; d < LLM_D_MODEL; d++) context[d] = 0;
            for (int p = 0; p <= pos; p++) {
                for (int d = 0; d < LLM_D_MODEL; d++) {
                    int8_t vd = (int8_t)MX_LANE(kv_v[l][p], d);
                    context[d] += ((int32_t)attn_w[p] * vd) >> 8;
                }
            }

            uint32_t ctx_packed = MX_PACK4(
                clip8(context[0]), clip8(context[1]),
                clip8(context[2]), clip8(context[3]));
            uint32_t attn_out = matvec4_int8(&W[OFF_WO(l)], ctx_packed, 2);

            uint32_t x_post_attn;
            MXINT8_ADD4(x_post_attn, x_residual, attn_out);
            uint32_t x_ffn_residual = x_post_attn;

            int32_t hidden[LLM_D_FFN];
            for (int h = 0; h < LLM_D_FFN; h++) {
                uint32_t dot;
                MXINT8_DOT4(dot, x_post_attn, W[OFF_WUP(l) + h]);
                hidden[h] = (int32_t)dot;
                if (hidden[h] < 0) hidden[h] = 0;
            }

            int32_t ffn_out[LLM_D_MODEL];
            for (int d = 0; d < LLM_D_MODEL; d++) {
                ffn_out[d] = 0;
                for (int g = 0; g < LLM_D_FFN / 4; g++) {
                    uint32_t h_packed = MX_PACK4(
                        clip8(hidden[g*4+0] >> 2),
                        clip8(hidden[g*4+1] >> 2),
                        clip8(hidden[g*4+2] >> 2),
                        clip8(hidden[g*4+3] >> 2));
                    uint32_t dot;
                    MXINT8_DOT4(dot, h_packed, W[OFF_WDN(l) + d*4 + g]);
                    ffn_out[d] += (int32_t)dot;
                }
            }

            uint32_t ffn_packed = MX_PACK4(
                clip8(ffn_out[0] >> 2), clip8(ffn_out[1] >> 2),
                clip8(ffn_out[2] >> 2), clip8(ffn_out[3] >> 2));
            MXINT8_ADD4(x, x_ffn_residual, ffn_packed);
        }

        if (pos >= LLM_PROMPT_LEN - 1 && pos < LLM_PROMPT_LEN + LLM_GEN_LEN - 1) {
            int32_t best_score = -2147483647;
            uint8_t best_tok = 0;
            for (int t = 0; t < LLM_VOCAB_SIZE; t++) {
                uint32_t dot;
                MXINT8_DOT4(dot, x, W[OFF_LM_HEAD + t]);
                int32_t score = (int32_t)dot;
                if (score > best_score) {
                    best_score = score;
                    best_tok = (uint8_t)t;
                }
            }
            tokens[pos + 1] = best_tok;
        }
    }

    uint32_t t1 = mx_rdcycle();

    printf("GEN:");
    for (int i = 0; i < LLM_GEN_LEN; i++)
        printf("0x%x ", tokens[LLM_PROMPT_LEN + i]);
    printf("\n");
    printf("CYCLES:0x%x\n", t1 - t0);
    printf("TOK0:0x%x\n", tokens[LLM_PROMPT_LEN + 0]);

    return tokens[LLM_PROMPT_LEN];
}
