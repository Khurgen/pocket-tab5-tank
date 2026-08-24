/* q4_model.c — see q4_model.h. Derived from karpathy/llama2.c runq.c (MIT),
 * restructured as a library over a memory-mapped 4-bit model. */
#include "q4_model.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

typedef struct { int8_t *q; float *s; } qact_t;           /* int8 activations */
typedef struct { const uint8_t *q; const uint16_t *s; } q4w_t;  /* 4-bit weights in flash */

struct q4_model {
    q4_config_t c;
    int gs;
    const float *rms_att, *rms_ffn, *rms_final;
    q4w_t emb, *wq, *wk, *wv, *wo, *w1, *w2, *w3, wcls;
    /* run state */
    float *x, *xb, *xb2, *hb, *hb2, *q, *att, *logits, *key_cache, *value_cache;
    qact_t xq, hq;
    /* batched-prefill state (allocated on first use): per-token activations */
    int    bmax;
    float *bx, *bxb, *bq, *bhb, *bhb2;      /* [bmax][dim] / [bmax][hidden] */
    int8_t *bxq_q; float *bxq_s;            /* [bmax][dim], [bmax][dim/gs] */
    int8_t *bhq_q; float *bhq_s;            /* [bmax][hidden], [bmax][hidden/gs] */
    void *(*alloc)(size_t);
};

static inline float half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16, exp = (h >> 10) & 0x1f, man = h & 0x3ff, f;
    if (exp == 0) { if (man == 0) f = sign; else { exp = 113; while (!(man & 0x400)) { man <<= 1; exp--; } man &= 0x3ff; f = sign | (exp << 23) | (man << 13); } }
    else if (exp == 31) f = sign | 0x7f800000 | (man << 13);
    else f = sign | ((exp + 112) << 23) | (man << 13);
    float out; memcpy(&out, &f, 4); return out;
}

static void map_q4(q4w_t *t, const uint8_t **p, int n, int numel, int gs) {
    for (int i = 0; i < n; i++) {
        t[i].q = *p; *p += numel / 2;
        t[i].s = (const uint16_t *)*p; *p += (numel / gs) * 2;
    }
}

q4_model_t *q4_model_open(const uint8_t *bin, size_t len, void *(*alloc)(size_t)) {
    if (len < 256) return NULL;
    uint32_t magic; memcpy(&magic, bin, 4);
    int version; memcpy(&version, bin + 4, 4);
    if (magic != 0x616b3432 || version != 3) return NULL;
    q4_model_t *m = alloc(sizeof *m); if (!m) return NULL; memset(m, 0, sizeof *m);
    m->alloc = alloc;
    memcpy(&m->c, bin + 8, sizeof m->c);
    uint8_t shared = bin[8 + 28];
    memcpy(&m->gs, bin + 8 + 28 + 1, 4);
    const q4_config_t *c = &m->c;
    const uint8_t *p = bin + 256;
    m->rms_att = (const float *)p; p += c->n_layers * c->dim * 4;
    m->rms_ffn = (const float *)p; p += c->n_layers * c->dim * 4;
    m->rms_final = (const float *)p; p += c->dim * 4;
    int kv_dim = c->dim * c->n_kv_heads / c->n_heads;
    map_q4(&m->emb, &p, 1, c->vocab_size * c->dim, m->gs);
    m->wq = alloc(c->n_layers * sizeof(q4w_t)); map_q4(m->wq, &p, c->n_layers, c->dim * c->dim, m->gs);
    m->wk = alloc(c->n_layers * sizeof(q4w_t)); map_q4(m->wk, &p, c->n_layers, c->dim * kv_dim, m->gs);
    m->wv = alloc(c->n_layers * sizeof(q4w_t)); map_q4(m->wv, &p, c->n_layers, c->dim * kv_dim, m->gs);
    m->wo = alloc(c->n_layers * sizeof(q4w_t)); map_q4(m->wo, &p, c->n_layers, c->dim * c->dim, m->gs);
    m->w1 = alloc(c->n_layers * sizeof(q4w_t)); map_q4(m->w1, &p, c->n_layers, c->dim * c->hidden_dim, m->gs);
    m->w2 = alloc(c->n_layers * sizeof(q4w_t)); map_q4(m->w2, &p, c->n_layers, c->hidden_dim * c->dim, m->gs);
    m->w3 = alloc(c->n_layers * sizeof(q4w_t)); map_q4(m->w3, &p, c->n_layers, c->dim * c->hidden_dim, m->gs);
    if (shared) m->wcls = m->emb; else map_q4(&m->wcls, &p, 1, c->dim * c->vocab_size, m->gs);
    if ((size_t)(p - bin) > len) return NULL;
    /* run state */
    m->x = alloc(c->dim * 4); m->xb = alloc(c->dim * 4); m->xb2 = alloc(c->dim * 4);
    m->hb = alloc(c->hidden_dim * 4); m->hb2 = alloc(c->hidden_dim * 4);
    m->q = alloc(c->dim * 4); m->att = alloc(c->n_heads * c->seq_len * 4);
    m->logits = alloc(c->vocab_size * 4);
    m->key_cache = alloc((size_t)c->n_layers * c->seq_len * kv_dim * 4);
    m->value_cache = alloc((size_t)c->n_layers * c->seq_len * kv_dim * 4);
    m->xq.q = alloc(c->dim); m->xq.s = alloc(c->dim / m->gs * 4);
    m->hq.q = alloc(c->hidden_dim); m->hq.s = alloc(c->hidden_dim / m->gs * 4);
    if (!m->x || !m->xb || !m->xb2 || !m->hb || !m->hb2 || !m->q || !m->att || !m->logits ||
        !m->key_cache || !m->value_cache || !m->xq.q || !m->xq.s || !m->hq.q || !m->hq.s)
        return NULL;   /* not enough RAM (e.g. no PSRAM): caller falls back to rules */
    return m;
}

const q4_config_t *q4_model_config(const q4_model_t *m) { return &m->c; }

static void rmsnorm(float *o, const float *x, const float *w, int n) {
    float ss = 0; for (int j = 0; j < n; j++) ss += x[j] * x[j];
    ss = 1.0f / sqrtf(ss / n + 1e-5f);
    for (int j = 0; j < n; j++) o[j] = w[j] * (ss * x[j]);
}
static void softmax(float *x, int n) {
    float mx = x[0]; for (int i = 1; i < n; i++) if (x[i] > mx) mx = x[i];
    float s = 0; for (int i = 0; i < n; i++) { x[i] = expf(x[i] - mx); s += x[i]; }
    for (int i = 0; i < n; i++) x[i] /= s;
}
static void quantize(qact_t *qx, const float *x, int n, int gs) {
    for (int g = 0; g < n / gs; g++) {
        float wmax = 0; for (int i = 0; i < gs; i++) { float v = fabsf(x[g * gs + i]); if (v > wmax) wmax = v; }
        float scale = wmax / 127.0f; qx->s[g] = scale;
        float inv = scale > 0 ? 1.0f / scale : 0;
        for (int i = 0; i < gs; i++) qx->q[g * gs + i] = (int8_t)lrintf(x[g * gs + i] * inv);
    }
}
/* xout[d] = W[d,n] @ x[n]; the hot loop — int8 activations x 4-bit weights */
static void matmul(float *xout, const qact_t *x, const q4w_t *w, int n, int d, int gs) {
    for (int i = 0; i < d; i++) {
        float val = 0; int in = i * n;
        for (int j = 0; j <= n - gs; j += gs) {
            int32_t ival = 0;
            const uint8_t *wq = w->q + ((in + j) >> 1);
            const int8_t *xq = x->q + j;
            for (int k = 0; k < gs; k += 2) {
                uint8_t b = wq[k >> 1];
                ival += (int32_t)xq[k] * ((int32_t)(b & 0x0f) - 8);
                ival += (int32_t)xq[k + 1] * ((int32_t)(b >> 4) - 8);
            }
            val += (float)ival * half_to_float(w->s[(in + j) / gs]) * x->s[j / gs];
        }
        xout[i] = val;
    }
}

float *q4_model_forward(q4_model_t *m, int token, int pos) {
    const q4_config_t *c = &m->c; int gs = m->gs;
    int dim = c->dim, kv_dim = dim * c->n_kv_heads / c->n_heads, kv_mul = c->n_heads / c->n_kv_heads;
    int hidden = c->hidden_dim, head_size = dim / c->n_heads;
    float *x = m->x;
    /* embedding row (dequantize) */
    for (int i = 0; i < dim; i++) {
        int idx = token * dim + i;
        int q = (int)((idx & 1) ? (m->emb.q[idx >> 1] >> 4) : (m->emb.q[idx >> 1] & 0x0f)) - 8;
        x[i] = q * half_to_float(m->emb.s[idx / gs]);
    }
    for (int l = 0; l < c->n_layers; l++) {
        rmsnorm(m->xb, x, m->rms_att + l * dim, dim);
        quantize(&m->xq, m->xb, dim, gs);
        int loff = l * c->seq_len * kv_dim;
        float *k = m->key_cache + loff + pos * kv_dim, *v = m->value_cache + loff + pos * kv_dim;
        matmul(m->q, &m->xq, &m->wq[l], dim, dim, gs);
        matmul(k, &m->xq, &m->wk[l], dim, kv_dim, gs);
        matmul(v, &m->xq, &m->wv[l], dim, kv_dim, gs);
        /* RoPE */
        for (int i = 0; i < dim; i += 2) {
            int hd = i % head_size;
            float freq = 1.0f / powf(10000.0f, hd / (float)head_size), val = pos * freq;
            float fcr = cosf(val), fci = sinf(val);
            int rotn = i < kv_dim ? 2 : 1;
            for (int vv = 0; vv < rotn; vv++) {
                float *vec = vv == 0 ? m->q : k;
                float v0 = vec[i], v1 = vec[i + 1];
                vec[i] = v0 * fcr - v1 * fci; vec[i + 1] = v0 * fci + v1 * fcr;
            }
        }
        /* attention */
        for (int h = 0; h < c->n_heads; h++) {
            float *q = m->q + h * head_size, *att = m->att + h * c->seq_len;
            for (int t = 0; t <= pos; t++) {
                float *kk = m->key_cache + loff + t * kv_dim + (h / kv_mul) * head_size, sc = 0;
                for (int i = 0; i < head_size; i++) sc += q[i] * kk[i];
                att[t] = sc / sqrtf((float)head_size);
            }
            softmax(att, pos + 1);
            float *xb = m->xb + h * head_size; memset(xb, 0, head_size * 4);
            for (int t = 0; t <= pos; t++) {
                float *vv = m->value_cache + loff + t * kv_dim + (h / kv_mul) * head_size, a = att[t];
                for (int i = 0; i < head_size; i++) xb[i] += a * vv[i];
            }
        }
        quantize(&m->xq, m->xb, dim, gs);
        matmul(m->xb2, &m->xq, &m->wo[l], dim, dim, gs);
        for (int i = 0; i < dim; i++) x[i] += m->xb2[i];
        /* ffn */
        rmsnorm(m->xb, x, m->rms_ffn + l * dim, dim);
        quantize(&m->xq, m->xb, dim, gs);
        matmul(m->hb, &m->xq, &m->w1[l], dim, hidden, gs);
        matmul(m->hb2, &m->xq, &m->w3[l], dim, hidden, gs);
        for (int i = 0; i < hidden; i++) { float val = m->hb[i]; val *= 1.0f / (1.0f + expf(-val)); m->hb[i] = val * m->hb2[i]; }
        quantize(&m->hq, m->hb, hidden, gs);
        matmul(m->xb, &m->hq, &m->w2[l], hidden, dim, gs);
        for (int i = 0; i < dim; i++) x[i] += m->xb[i];
    }
    rmsnorm(x, x, m->rms_final, dim);
    quantize(&m->xq, x, dim, gs);
    matmul(m->logits, &m->xq, &m->wcls, dim, c->vocab_size, gs);
    return m->logits;
}


/* ---- batched prefill: weights are read ONCE per layer for all n tokens ----
 * out[t*d + i] = W[i,:] . X_t  for t in [0,n). Each weight group's nibbles are
 * unpacked once and dotted against every token's int8 activations. This is the
 * on-device latency lever: a 44-token prompt costs ~1 weight pass, not 44. */
static void matmul_batch(float *out, const int8_t *xq, const float *xs, int n_tok,
                         const q4w_t *w, int n, int d, int gs) {
    int8_t wg[128];                               /* unpacked group (gs <= 128) */
    int ngroups = n / gs, sg = n / gs;            /* scale stride per token */
    for (int i = 0; i < d; i++) {
        int in = i * n;
        for (int t = 0; t < n_tok; t++) out[t * d + i] = 0;
        for (int g = 0; g < ngroups; g++) {
            const uint8_t *wq = w->q + ((in + g * gs) >> 1);
            for (int k = 0; k < gs; k += 2) {
                uint8_t b = wq[k >> 1];
                wg[k] = (int8_t)((b & 0x0f) - 8); wg[k + 1] = (int8_t)((b >> 4) - 8);
            }
            float ws = half_to_float(w->s[(in + g * gs) / gs]);
            for (int t = 0; t < n_tok; t++) {
                const int8_t *x = xq + t * n + g * gs; int32_t ival = 0;
                for (int k = 0; k < gs; k++) ival += (int32_t)x[k] * wg[k];
                out[t * d + i] += (float)ival * ws * xs[t * sg + g];
            }
        }
    }
}

static bool ensure_batch(q4_model_t *m, int n) {
    if (m->bmax >= n) return true;
    const q4_config_t *c = &m->c; int gs = m->gs;
    m->bmax = n;
    m->bx   = m->alloc((size_t)n * c->dim * 4);
    m->bxb  = m->alloc((size_t)n * c->dim * 4);
    m->bq   = m->alloc((size_t)n * c->dim * 4);
    m->bhb  = m->alloc((size_t)n * c->hidden_dim * 4);
    m->bhb2 = m->alloc((size_t)n * c->hidden_dim * 4);
    m->bxq_q = m->alloc((size_t)n * c->dim); m->bxq_s = m->alloc((size_t)n * (c->dim / gs) * 4);
    m->bhq_q = m->alloc((size_t)n * c->hidden_dim); m->bhq_s = m->alloc((size_t)n * (c->hidden_dim / gs) * 4);
    return m->bx && m->bxb && m->bq && m->bhb && m->bhb2 && m->bxq_q && m->bxq_s && m->bhq_q && m->bhq_s;
}

float *q4_model_prefill(q4_model_t *m, const int *toks, int n) {
    const q4_config_t *c = &m->c; int gs = m->gs;
    int dim = c->dim, kv_dim = dim * c->n_kv_heads / c->n_heads, kv_mul = c->n_heads / c->n_kv_heads;
    int hidden = c->hidden_dim, head_size = dim / c->n_heads;
    if (n <= 0 || n > c->seq_len - 1 || !ensure_batch(m, n)) return NULL;
    /* embeddings */
    for (int t = 0; t < n; t++)
        for (int i = 0; i < dim; i++) {
            int idx = toks[t] * dim + i;
            int q = (int)((idx & 1) ? (m->emb.q[idx >> 1] >> 4) : (m->emb.q[idx >> 1] & 0x0f)) - 8;
            m->bx[t * dim + i] = q * half_to_float(m->emb.s[idx / gs]);
        }
    qact_t qa; 
    for (int l = 0; l < c->n_layers; l++) {
        int loff = l * c->seq_len * kv_dim;
        for (int t = 0; t < n; t++) {
            rmsnorm(m->bxb + t * dim, m->bx + t * dim, m->rms_att + l * dim, dim);
            qa.q = m->bxq_q + t * dim; qa.s = m->bxq_s + t * (dim / gs);
            quantize(&qa, m->bxb + t * dim, dim, gs);
        }
        /* q for all tokens; k,v straight into the cache rows */
        matmul_batch(m->bq, m->bxq_q, m->bxq_s, n, &m->wq[l], dim, dim, gs);
        /* k/v: out must be contiguous per token at stride d=kv_dim -> cache rows are exactly that */
        matmul_batch(m->key_cache + loff,   m->bxq_q, m->bxq_s, n, &m->wk[l], dim, kv_dim, gs);
        matmul_batch(m->value_cache + loff, m->bxq_q, m->bxq_s, n, &m->wv[l], dim, kv_dim, gs);
        for (int t = 0; t < n; t++) {
            float *q = m->bq + t * dim, *k = m->key_cache + loff + t * kv_dim;
            for (int i = 0; i < dim; i += 2) {
                int hd = i % head_size;
                float freq = 1.0f / powf(10000.0f, hd / (float)head_size), val = t * freq;
                float fcr = cosf(val), fci = sinf(val);
                int rotn = i < kv_dim ? 2 : 1;
                for (int vv = 0; vv < rotn; vv++) {
                    float *vec = vv == 0 ? q : k;
                    float v0 = vec[i], v1 = vec[i + 1];
                    vec[i] = v0 * fcr - v1 * fci; vec[i + 1] = v0 * fci + v1 * fcr;
                }
            }
            /* causal attention for token t */
            for (int h = 0; h < c->n_heads; h++) {
                float *qh = q + h * head_size, *att = m->att + h * c->seq_len;
                for (int p = 0; p <= t; p++) {
                    float *kk = m->key_cache + loff + p * kv_dim + (h / kv_mul) * head_size, sc = 0;
                    for (int i = 0; i < head_size; i++) sc += qh[i] * kk[i];
                    att[p] = sc / sqrtf((float)head_size);
                }
                softmax(att, t + 1);
                float *xb = m->bxb + t * dim + h * head_size; memset(xb, 0, head_size * 4);
                for (int p = 0; p <= t; p++) {
                    float *vv = m->value_cache + loff + p * kv_dim + (h / kv_mul) * head_size, a = att[p];
                    for (int i = 0; i < head_size; i++) xb[i] += a * vv[i];
                }
            }
            qa.q = m->bxq_q + t * dim; qa.s = m->bxq_s + t * (dim / gs);
            quantize(&qa, m->bxb + t * dim, dim, gs);
        }
        matmul_batch(m->bq, m->bxq_q, m->bxq_s, n, &m->wo[l], dim, dim, gs);   /* reuse bq as xb2 */
        for (int t = 0; t < n; t++) {
            for (int i = 0; i < dim; i++) m->bx[t * dim + i] += m->bq[t * dim + i];
            rmsnorm(m->bxb + t * dim, m->bx + t * dim, m->rms_ffn + l * dim, dim);
            qa.q = m->bxq_q + t * dim; qa.s = m->bxq_s + t * (dim / gs);
            quantize(&qa, m->bxb + t * dim, dim, gs);
        }
        matmul_batch(m->bhb,  m->bxq_q, m->bxq_s, n, &m->w1[l], dim, hidden, gs);
        matmul_batch(m->bhb2, m->bxq_q, m->bxq_s, n, &m->w3[l], dim, hidden, gs);
        for (int t = 0; t < n; t++) {
            float *hb = m->bhb + t * hidden, *hb2 = m->bhb2 + t * hidden;
            for (int i = 0; i < hidden; i++) { float v = hb[i]; v *= 1.0f / (1.0f + expf(-v)); hb[i] = v * hb2[i]; }
            qa.q = m->bhq_q + t * hidden; qa.s = m->bhq_s + t * (hidden / gs);
            quantize(&qa, hb, hidden, gs);
        }
        matmul_batch(m->bxb, m->bhq_q, m->bhq_s, n, &m->w2[l], hidden, dim, gs);
        for (int t = 0; t < n; t++)
            for (int i = 0; i < dim; i++) m->bx[t * dim + i] += m->bxb[t * dim + i];
    }
    /* logits for the LAST token only; leave m->x holding it like forward() would */
    memcpy(m->x, m->bx + (n - 1) * dim, dim * 4);
    rmsnorm(m->x, m->x, m->rms_final, dim);
    quantize(&m->xq, m->x, dim, gs);
    matmul(m->logits, &m->xq, &m->wcls, dim, c->vocab_size, gs);
    return m->logits;
}

static int argmax(const float *l, int v) { int b = 0; for (int i = 1; i < v; i++) if (l[i] > l[b]) b = i; return b; }

int q4_model_generate(q4_model_t *m, const int *prompt, int n_prompt, int *out, int max_out) {
    int n = 0;
    float *logits = q4_model_prefill(m, prompt, n_prompt);      /* one pass over the weights */
    if (!logits) {                                                /* fallback: token by token */
        for (int p = 0; p < n_prompt; p++) logits = q4_model_forward(m, prompt[p], p);
    }
    int pos = n_prompt - 1, token = argmax(logits, m->c.vocab_size);
    while (1) {
        if (token == 1 || n >= max_out) break;                    /* BOS = stop */
        out[n++] = token;
        pos++; if (pos >= m->c.seq_len - 1) break;
        logits = q4_model_forward(m, token, pos);
        token = argmax(logits, m->c.vocab_size);
    }
    return n;
}

int q4_model_decide(q4_model_t *m, const int *prompt, int n_prompt,
                    const int *cand, int n_cand, float temp, uint32_t *rng,
                    float *probs_out, int *out, int max_out) {
    int n = 0;
    float *logits = q4_model_prefill(m, prompt, n_prompt);
    if (!logits) for (int p = 0; p < n_prompt; p++) logits = q4_model_forward(m, prompt[p], p);
    /* softmax over the candidates only (the closed goal set) */
    float p[16]; if (n_cand > 16) n_cand = 16;
    float inv = temp > 0 ? 1.0f / temp : 1.0f, mx = -1e30f;
    for (int i = 0; i < n_cand; i++) { float v = logits[cand[i]] * inv; p[i] = v; if (v > mx) mx = v; }
    float sum = 0; for (int i = 0; i < n_cand; i++) { p[i] = expf(p[i] - mx); sum += p[i]; }
    for (int i = 0; i < n_cand; i++) p[i] /= sum;
    if (probs_out) for (int i = 0; i < n_cand; i++) probs_out[i] = p[i];
    int pick = 0;
    if (temp > 0 && rng) {
        uint32_t x = *rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *rng = x;
        float r = (float)(x & 0xffffff) / 16777216.0f, acc = 0;
        for (int i = 0; i < n_cand; i++) { acc += p[i]; if (r < acc) { pick = i; break; } pick = i; }
    } else for (int i = 1; i < n_cand; i++) if (p[i] > p[pick]) pick = i;
    int pos = n_prompt - 1, token = cand[pick];
    while (1) {
        if (token == 1 || n >= max_out) break;
        out[n++] = token;
        pos++; if (pos >= m->c.seq_len - 1) break;
        logits = q4_model_forward(m, token, pos);
        token = argmax(logits, m->c.vocab_size);
    }
    return n;
}

/* reference path (no batching) for verification */
int q4_model_generate_seq(q4_model_t *m, const int *prompt, int n_prompt, int *out, int max_out) {
    int pos = 0, token = prompt[0], n = 0;
    while (pos < m->c.seq_len - 1) {
        float *logits = q4_model_forward(m, token, pos);
        int next;
        if (pos < n_prompt - 1) next = prompt[pos + 1];
        else next = argmax(logits, m->c.vocab_size);
        pos++;
        if (pos >= n_prompt) { if (next == 1 || n >= max_out) break; out[n++] = next; }
        token = next;
    }
    return n;
}
