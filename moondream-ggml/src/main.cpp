#include <cstdio>
#include <cstring>
#include <cassert>
#include <cmath>
#include <fstream>
#include <vector>
#include "ggml/ggml.h"

#define MD_TEXT_MODEL_FNAME "moondream2-text-model-f16.gguf"
#define MD_MMPROJ_FNAME "moondream2-mmproj-f16.gguf"
#define DATA_PATH_MAX_LEN 512
#define ARCH_PREFIX(t) ("phi2." t)
#define LLAMA_MAX_NODES   8192
// Corresponds to LLAMA_ROPE_TYPE_NEOX from llama.cpp which is what is used for phi2
#define MOONDREAM_ROPE_TYPE 2

struct moondream_layer {
    // normalization
    struct ggml_tensor * attn_norm;
    struct ggml_tensor * attn_norm_b;
    struct ggml_tensor * attn_norm_2;
    struct ggml_tensor * attn_norm_2_b;
    struct ggml_tensor * attn_q_norm;
    struct ggml_tensor * attn_q_norm_b;
    struct ggml_tensor * attn_k_norm;
    struct ggml_tensor * attn_k_norm_b;
    struct ggml_tensor * attn_out_norm;
    struct ggml_tensor * attn_out_norm_b;
    struct ggml_tensor * attn_q_a_norm;
    struct ggml_tensor * attn_kv_a_norm;

    // attention
    struct ggml_tensor * wq;
    struct ggml_tensor * wk;
    struct ggml_tensor * wv;
    struct ggml_tensor * wo;
    struct ggml_tensor * wqkv;
    struct ggml_tensor * wq_a;
    struct ggml_tensor * wq_b;
    struct ggml_tensor * wkv_a_mqa;
    struct ggml_tensor * wkv_b;

    // attention bias
    struct ggml_tensor * bq;
    struct ggml_tensor * bk;
    struct ggml_tensor * bv;
    struct ggml_tensor * bo;
    struct ggml_tensor * bqkv;

    // normalization
    struct ggml_tensor * ffn_norm;
    struct ggml_tensor * ffn_norm_b;
    struct ggml_tensor * layer_out_norm;
    struct ggml_tensor * layer_out_norm_b;
    struct ggml_tensor * ffn_norm_exps;

    // ff
    struct ggml_tensor * ffn_gate; // w1
    struct ggml_tensor * ffn_down; // w2
    struct ggml_tensor * ffn_up;   // w3

    // ff bias
    struct ggml_tensor * ffn_gate_b = nullptr;
    struct ggml_tensor * ffn_down_b = nullptr; // b2
    struct ggml_tensor * ffn_up_b = nullptr; // b3
    struct ggml_tensor * ffn_act;
};

struct moondream_hparams {
    int n_embd;
    int n_ff;
    int n_layer; // I think this is the same as n_block
    int n_rot;
    int n_ctx_train;
    int n_head;
    int n_head_kv;
    int n_embd_head_k;
    int n_embd_k_gqa;
    int n_embd_head_v;
    int n_embd_v_gqa;
    
    float f_norm_eps;
    float f_norm_rms_eps;

    // this doesn't seem to be present in the model
    float rope_freq_base_train;
    int rope_attn_factor;

    // max bias for attention, not sure if it's used for anything else
    float f_max_alibi_bias;
};

struct moondream_cparams {
    uint32_t n_ctx; // context size used during inference
    uint32_t n_batch;
    uint32_t n_ubatch;
    uint32_t n_seq_max;
    uint32_t n_threads; // number of threads to use for generation
    uint32_t n_threads_batch; // number of threads to use for batch processing

    float rope_freq_base;
    float rope_freq_scale;

    uint32_t n_ctx_orig_yarn;
    // These hyperparameters are not exposed in GGUF, because all
    // existing YaRN models use the same values for them.
    float yarn_ext_factor;
    float yarn_attn_factor;
    float yarn_beta_fast;
    float yarn_beta_slow;
    float defrag_thold;

    bool embeddings;
    bool causal_attn;
    bool offload_kqv;
    bool flash_attn;

    //enum llama_pooling_type pooling_type;

    //ggml_backend_sched_eval_callback cb_eval;
    //void * cb_eval_user_data;
};

struct moondream_model {
    moondream_hparams hparams;
    moondream_cparams cparams;
    std::vector<moondream_layer> layers;
    ggml_context * ctx;

    // Tensors
    ggml_tensor * tok_embd;
    
    ggml_tensor * output_norm;
    ggml_tensor * output_norm_b;
    ggml_tensor * output;
    ggml_tensor * output_b;
};

// Arrays must have size of n_tokens
struct moondream_batch {
    int32_t n_tokens;
    // The token ids of the input (used when embd is NULL)
    int32_t * token;
    // The token embeddings (used when token is NULL)
    float * embd;
    // The positions of the respective tokens in the sequence
    int32_t * pos;
    // The sequence to which the respective token belongs
    int32_t ** seq_id;
    // If zero, the logits for the respective token will not be output
    //int8_t * logits;
};

/* NOTE:
 * Parts of moondream_context that have been used (outdated):
 * - inp_tokens
 * - inp_embd
 * - inp_pos
 * - inp_KQ_mask 
 */

struct moondream_context {
    moondream_cparams cparams;

    int n_outputs;
     // Number of tokens sampled
    int32_t n_sample = 0;

    // Input tensors
    struct ggml_tensor * inp_tokens;    // I32 [n_batch]
    struct ggml_tensor * inp_embd;      // F32 [n_embd, n_batch]
    struct ggml_tensor * inp_pos;       // I32 [n_batch]
    struct ggml_tensor * inp_out_ids;   // I32 [n_outputs]
    struct ggml_tensor * inp_KQ_mask;   // F32 [kv_size, n_batch]
    struct ggml_tensor * inp_K_shift;   // I32 [kv_size]
    struct ggml_tensor * inp_mean;      // F32 [n_batch, n_batch]
    struct ggml_tensor * inp_cls;       // I32 [n_batch]
    struct ggml_tensor * inp_s_copy;    // I32 [kv_size]
    struct ggml_tensor * inp_s_mask;    // F32 [1, n_kv]
    struct ggml_tensor * inp_s_seq;     // I32 [n_kv, n_batch]
};

struct moondream_kv_cache {
    bool has_shift = false;
    bool do_defrag = false;
    bool do_copy = false;
    // whether or not the value tensor is transposed
    bool v_trans = true;

    uint32_t head = 0;
    uint32_t size = 0;
    uint32_t used = 0;

    // computed before each graph build
    // what does it mean though?
    uint32_t n = 0;

    ggml_type type_k = GGML_TYPE_F16;
    ggml_type type_v = GGML_TYPE_F16;
    
    // per layer k and v caches
    std::vector<struct ggml_tensor *> k_l; // per layer
    std::vector<struct ggml_tensor *> v_l;

    // TODO: there is some extra stuff that I've omitted, make sure that it's not necessary
    // or add it if it is (i.e. ctxs and bufs)
};

// NOTE: skipping the usage of llm_build_cb (build callback) because I have a feeling
// it won't be necessary, may need to revisit this though
ggml_tensor * llm_build_inp_embd(
    ggml_context * ctx, 
    moondream_context & mctx,
    const moondream_hparams & hparams,
    const moondream_batch & batch,
    ggml_tensor * tok_embd
) {
    // TODO: what does the L stand for?
    ggml_tensor * inpL;
    
    // If batch has tokens (integers) then set inp_tokens to the input and 
    // take the embeddings from tok_embd, otherwise use the token embeddings
    // (inp_embd) and set them as the input
    if (batch.token) {
        mctx.inp_tokens = ggml_new_tensor_1d(ctx, GGML_TYPE_I32, batch.n_tokens);
        ggml_set_input(mctx.inp_tokens);
        inpL = ggml_get_rows(ctx, tok_embd, mctx.inp_tokens);
    } else {
        mctx.inp_embd = ggml_new_tensor_2d(ctx, GGML_TYPE_F32, hparams.n_embd, batch.n_tokens);
        inpL = mctx.inp_embd;
        ggml_set_input(mctx.inp_embd);
    }
    return inpL;
}

// NOTE: version of build_inp_pos without build callback
ggml_tensor * build_inp_pos(ggml_context * ctx, moondream_context & mctx, moondream_batch & batch) {
    mctx.inp_pos = ggml_new_tensor_1d(ctx, GGML_TYPE_I32, batch.n_tokens);
    ggml_set_input(mctx.inp_pos);
    return mctx.inp_pos;
}

// NOTE: version of build_inp_KQ_mask without build callback
ggml_tensor * build_inp_KQ_mask(
    ggml_context * ctx, 
    moondream_context & mctx, 
    moondream_batch & batch,
    moondream_cparams & cparams,
    int32_t n_kv
) {
    // How does the causal branch differ from the non-causal branch?
    if (cparams.causal_attn) {
        mctx.inp_KQ_mask = ggml_new_tensor_2d(
            ctx, GGML_TYPE_F32, n_kv, GGML_PAD(batch.n_tokens, GGML_KQ_MASK_PAD)
        );
    } else {
        mctx.inp_KQ_mask = ggml_new_tensor_2d(
            ctx, GGML_TYPE_F32, batch.n_tokens, GGML_PAD(batch.n_tokens, GGML_KQ_MASK_PAD)
        );
    }
    ggml_set_input(mctx.inp_KQ_mask);
    return cparams.flash_attn ? ggml_cast(ctx, mctx.inp_KQ_mask, GGML_TYPE_F16) : mctx.inp_KQ_mask;
};

/* start of llm enums */
enum llm_ffn_op_type {
    LLM_FFN_SILU,
    LLM_FFN_GELU,
    LLM_FFN_RELU,
    LLM_FFN_RELU_SQR,
};

enum llm_ffn_gate_type {
    LLM_FFN_SEQ,
    LLM_FFN_PAR, // ffn_gate is parallel to ffn_up
};

enum llm_norm_type {
    LLM_NORM,
    LLM_NORM_RMS,
};
/* end of llm enums */

// Note build callback seems important for layer names so it might be needed here
// What does cur mean? Can we find a more descriptive name for it?
ggml_tensor * llm_build_norm(
    ggml_context * ctx, 
    ggml_tensor * cur, 
    moondream_hparams & hparams,
    ggml_tensor * mw,
    ggml_tensor * mb,
    llm_norm_type type,
    int il // What does il mean?
) {
    switch(type) {
        case LLM_NORM:
            cur = ggml_norm(ctx, cur, hparams.f_norm_eps);
            break;
        case LLM_NORM_RMS:
            cur = ggml_rms_norm(ctx, cur, hparams.f_norm_rms_eps);
            break;
    }
    
    // weight
    if (mw) {
        cur = ggml_mul(ctx, cur, mw);
    }
    // bias
    if (mb) {
        cur = ggml_add(ctx, cur, mb);
    }
    return cur;
}

// Maybe this should be renamed to llm_build_kv_cache?
void llm_build_kv_store(
    ggml_context * ctx, 
    moondream_hparams & hparams, 
    moondream_cparams & cparams, 
    moondream_kv_cache & kv,
    ggml_cgraph * graph,
    ggml_tensor * k_cur,
    ggml_tensor * v_cur,
    int32_t n_tokens,
    int32_t kv_head,
    int il
) {
    const int64_t n_ctx = cparams.n_ctx;
    const int64_t n_embd_k_gqa = hparams.n_embd_k_gqa;
    const int64_t n_embd_v_gqa = hparams.n_embd_v_gqa;
    
    // Why use GGML_ASSERT here and the regular c assert below?
    GGML_ASSERT(kv.size == n_ctx);

    // NOTE: I think this creates a view into the key cache, copies the key for the current head
    // into it, then builds it into the graph, idk why the build is necessary here though
    ggml_tensor * k_cache_view = ggml_view_1d(
        ctx, kv.k_l[il], n_tokens*n_embd_k_gqa, 
        // why are there parentheses around ggml_row_size?
        (ggml_row_size(kv.k_l[il]->type, n_embd_k_gqa))*kv_head
    );
    ggml_build_forward_expand(graph, ggml_cpy(ctx, k_cur, k_cache_view));

    // What does ne stand for?
    // Apparently it's the number of elements... per element maybe?
    assert(v_cur->ne[0] == n_embd_v_gqa && v_cur->ne[1] == n_tokens);

    ggml_tensor * v_cache_view = nullptr;
    if (cparams.flash_attn) {
        v_cache_view = ggml_view_1d(
            ctx, kv.v_l[il], n_tokens*n_embd_v_gqa, 
            // why are there parantheses around kv_head?
            (kv_head)*ggml_row_size(kv.v_l[il]->type, n_embd_v_gqa)
        );
    } else {
        // TODO: figure out exactly what view 2d is doing under the hood
        // NOTE: the v cache is transposed when not using flash attention
        v_cache_view = ggml_view_2d(
            ctx, kv.v_l[il], n_tokens, n_embd_v_gqa, 
            (n_ctx)*ggml_element_size(kv.v_l[il]),
            (kv_head)*ggml_element_size(kv.v_l[il])
        );
        v_cur = ggml_transpose(ctx, v_cur);
    }
    ggml_build_forward_expand(graph, ggml_cpy(ctx, v_cur, v_cache_view));
}

ggml_tensor * llm_build_kqv(
    ggml_context * ctx,
    moondream_model & model,
    moondream_hparams & hparams,
    moondream_cparams & cparams,
    moondream_kv_cache & kv,
    ggml_cgraph * graph,
    ggml_tensor * wo,
    ggml_tensor * wo_b,
    ggml_tensor * q_cur,
    ggml_tensor * kq_mask,
    int32_t n_tokens,
    int32_t n_kv,
    float kq_scale,
    int il
) {
    const int64_t n_ctx = cparams.n_ctx;
    const int64_t n_head = hparams.n_head;
    const int64_t n_head_kv = hparams.n_head_kv;
    const int64_t n_embd_head_k = hparams.n_embd_head_k;
    const int64_t n_embd_k_gqa = hparams.n_embd_k_gqa;
    const int64_t n_embd_head_v = hparams.n_embd_head_v;
    const int64_t n_embd_v_gqa = hparams.n_embd_v_gqa;
    
    ggml_tensor * q = ggml_permute(ctx, q_cur, 0, 2, 1, 3);
    // TODO: figure out exactly how ggml_view_3d works under the hood
    ggml_tensor * k = ggml_view_3d(
        ctx, kv.k_l[il],
        n_embd_head_v, n_kv, n_head_kv,
        ggml_row_size(kv.k_l[il]->type, n_embd_k_gqa),
        ggml_row_size(kv.k_l[il]->type, n_embd_head_k),
        0
    );

    ggml_tensor * cur;
    if (cparams.flash_attn) {
        // llama uses GGML_UNUSED here but I'm not sure what it does
        // see llama.cpp line 6989 for more details

        // split cached v into n_head heads (not transposed)
        ggml_tensor * v = ggml_view_3d(
            ctx, kv.v_l[il], 
            n_embd_head_v, n_kv, n_head_kv,
            ggml_row_size(kv.v_l[il]->type, n_embd_v_gqa),
            ggml_row_size(kv.v_l[il]->type, n_embd_head_v),
            0
        );
        cur = ggml_flash_attn_ext(ctx, q, k, v, kq_mask, kq_scale, hparams.f_max_alibi_bias);
        // for phi2 the KQ multiplication must be done with F32 precision, otherwise we get NaNs
        // ref: https://github.com/ggerganov/llama.cpp/pull/4490#issuecomment-1859055847
        ggml_flash_attn_ext_set_prec(cur, GGML_PREC_F32);
        cur = ggml_reshape_2d(ctx, cur, n_embd_head_v*n_head, n_tokens);
    } else {
        ggml_tensor * kq = ggml_mul_mat(ctx, k, q);
        // for phi2 the KQ multiplication must be done with F32 precision, otherwise we get NaNs
        // ref: https://github.com/ggerganov/llama.cpp/pull/4490#issuecomment-1859055847
        ggml_mul_mat_set_prec(kq, GGML_PREC_F32);
        kq = ggml_soft_max_ext(ctx, kq, kq_mask, kq_scale, hparams.f_max_alibi_bias);
        GGML_ASSERT(kv.size == n_ctx);
        // split cached v into n_head heads
        ggml_tensor * v = ggml_view_3d(
            ctx, kv.v_l[il], 
            n_kv, n_embd_head_v, n_head_kv,
            ggml_element_size(kv.v_l[il])*n_ctx,
            ggml_element_size(kv.v_l[il])*n_ctx*n_embd_head_v,
            0
        );
        // TODO: go over caching and clarify what's happening
        ggml_tensor * kqv = ggml_mul_mat(ctx, v, kq);
        ggml_tensor * kqv_merged = ggml_permute(ctx, kqv, 0, 2, 1, 3);
        // make contiguous, with new shape
        cur = ggml_cont_2d(ctx, kqv_merged, n_embd_head_v*n_head, n_tokens);
    }
    
    ggml_build_forward_expand(graph, cur);
    cur = ggml_mul_mat(ctx, wo, cur);
    if (wo_b) {
        cur = ggml_add(ctx, cur, wo_b);
    }
    return cur;
}

ggml_tensor * llm_build_kv(
    ggml_context * ctx, 
    moondream_model & model, 
    moondream_hparams & hparams,
    moondream_cparams & cparams,
    moondream_kv_cache & kv,
    ggml_cgraph * graph,
    ggml_tensor * wo,
    ggml_tensor * wo_b,
    ggml_tensor * k_cur,
    ggml_tensor * v_cur,
    ggml_tensor * q_cur,
    ggml_tensor * kq_mask,
    // TODO: some of these can probably be replaced with the structs that contain them
    int32_t n_tokens,
    int32_t kv_head,
    int32_t n_kv,
    float kq_scale,
    int il
) {
    // these nodes are added to the graph together so that they are not reordered
    // by doing so, the number of splits in the graph is reduced
    ggml_build_forward_expand(graph, q_cur);
    ggml_build_forward_expand(graph, k_cur);
    ggml_build_forward_expand(graph, v_cur);

    llm_build_kv_store(ctx, hparams, cparams, kv, graph, k_cur, v_cur, n_tokens, kv_head, il);
    ggml_tensor * cur;
    cur = llm_build_kqv(
        ctx, model, hparams, cparams, kv, graph, wo, wo_b, 
        q_cur, kq_mask, n_tokens, n_kv, kq_scale, il
    );
    return cur;
}

ggml_tensor * build_inp_out_ids(ggml_context * ctx, moondream_context & mctx, int n_outputs) {
    mctx.inp_out_ids = ggml_new_tensor_1d(ctx, GGML_TYPE_I32, n_outputs);
    //cb(lctx.inp_out_ids, "inp_out_ids", -1);
    ggml_set_input(mctx.inp_out_ids);
    return mctx.inp_out_ids;
}

ggml_tensor * llm_build_ffn(
    ggml_context * ctx,
    ggml_tensor * cur,
    ggml_tensor * up,
    ggml_tensor * up_b,
    ggml_tensor * gate,
    ggml_tensor * gate_b,
    ggml_tensor * down,
    ggml_tensor * down_b,
    ggml_tensor * act_scales,
    // NOTE: these flags might not be necessary if they don't vary for phi2 models
    llm_ffn_op_type type_op,
    llm_ffn_gate_type type_gate,
    int il
) {
    ggml_tensor * tmp = up ? ggml_mul_mat(ctx, up, cur) : cur;
    if (up_b) {
        tmp = ggml_add(ctx, tmp, up_b);
    }
    if (gate) {
        switch (type_gate) {
            case LLM_FFN_SEQ: {
                cur = ggml_mul_mat(ctx, gate, tmp);
                break;
            }
            case LLM_FFN_PAR: {
                cur = ggml_mul_mat(ctx, gate, cur);
                break;
            }
        }
        if (gate_b) {
            cur = ggml_add(ctx, cur, gate_b);
        }
    } else {
        cur = tmp;
    }

    switch (type_op) {
        case LLM_FFN_SILU: {
            cur = ggml_silu(ctx, cur);
            break;
        }
        case LLM_FFN_GELU: {
            cur = ggml_gelu(ctx, cur);
            if (act_scales != NULL) {
                cur = ggml_div(ctx, cur, act_scales);
            }
            break;
        }
        case LLM_FFN_RELU: {
            cur = ggml_relu(ctx, cur);
            break;
        }
        case LLM_FFN_RELU_SQR: {
            cur = ggml_relu(ctx, cur);
            cur = ggml_sqr(ctx, cur);
            break;
        }
    }

    if (type_gate == LLM_FFN_PAR) {
        cur = ggml_mul(ctx, cur, tmp);
    }

    cur = ggml_mul_mat(ctx, down, cur);
    if (down_b) {
        cur = ggml_add(ctx, cur, down_b);
    }
    return cur;
}

/* 
NOTE: llama.cpp has an llm_build_context struct which encapsulates all the cgraph build functions,
we probably don't need that but we will need some of the member variables.

Reference for convenience: 

struct llm_build_context {
    const llama_model    & model;
          llama_context  & lctx;
    const llama_hparams  & hparams;
    const llama_cparams  & cparams;
    const llama_batch    & batch;
    const llama_kv_cache & kv_self;

    const int64_t n_embd;
    const int64_t n_layer;
    const int64_t n_rot;
    const int64_t n_ctx;       // user-specified context size (can be different from n_ctx_train)
    const int64_t n_head;
    const int64_t n_head_kv;
    const int64_t n_embd_head_k;
    const int64_t n_embd_k_gqa;
    const int64_t n_embd_head_v;
    const int64_t n_embd_v_gqa;
    const int64_t n_expert;
    const int64_t n_expert_used;

    const float freq_base;
    const float freq_scale;
    const float ext_factor;
    const float attn_factor;
    const float beta_fast;
    const float beta_slow;
    const float norm_eps;
    const float norm_rms_eps;

    const int32_t n_tokens;
    const int32_t n_kv;     // size of KV cache to consider (n_kv <= kv_self.size)
    const int32_t n_outputs;
    const int32_t kv_head;  // index of where we store new KV data in the cache
    const int32_t n_ctx_orig;

    const bool flash_attn;

    const enum llama_pooling_type pooling_type;
    const enum llama_rope_type    rope_type;

    const llm_build_cb & cb;

    std::vector<uint8_t> & buf_compute_meta;

    struct ggml_context * ctx0 = nullptr;

    // TODO: consider making the entire interface noexcept
    llm_build_context(
        llama_context  & lctx,
    const llama_batch  & batch,
    const llm_build_cb & cb,
                  bool   worst_case) :
        model            (lctx.model),
        lctx             (lctx),
        hparams          (model.hparams),
        cparams          (lctx.cparams),
        batch            (batch),
        kv_self          (lctx.kv_self),
        n_embd           (hparams.n_embd),
        n_layer          (hparams.n_layer),
        n_rot            (hparams.n_rot),
        n_ctx            (cparams.n_ctx),
        n_head           (hparams.n_head),
        n_head_kv        (hparams.n_head_kv),
        n_embd_head_k    (hparams.n_embd_head_k),
        n_embd_k_gqa     (hparams.n_embd_k_gqa()),
        n_embd_head_v    (hparams.n_embd_head_v),
        n_embd_v_gqa     (hparams.n_embd_v_gqa()),
        n_expert         (hparams.n_expert),
        n_expert_used    (hparams.n_expert_used),
        freq_base        (cparams.rope_freq_base),
        freq_scale       (cparams.rope_freq_scale),
        ext_factor       (cparams.yarn_ext_factor),
        attn_factor      (cparams.yarn_attn_factor),
        beta_fast        (cparams.yarn_beta_fast),
        beta_slow        (cparams.yarn_beta_slow),
        norm_eps         (hparams.f_norm_eps),
        norm_rms_eps     (hparams.f_norm_rms_eps),
        n_tokens         (batch.n_tokens),
        n_kv             (worst_case ? kv_self.size : kv_self.n),
        n_outputs        (worst_case ? n_tokens : lctx.n_outputs),
        kv_head          (worst_case ? (kv_self.recurrent ? 0 : kv_self.size - n_tokens) : kv_self.head),
        n_ctx_orig       (cparams.n_ctx_orig_yarn),
        flash_attn       (cparams.flash_attn),
        pooling_type     (cparams.pooling_type),
        rope_type        (hparams.rope_type),
        cb               (cb),
        buf_compute_meta (lctx.buf_compute_meta) {
            // all initializations should be done in init()
        }
*/

// modification of llama.cpp build_phi2
// ref: https://github.com/ggerganov/llama.cpp/blob/da799b41891e34aac86ce4e173f9c4c0afd4fab3/llama.cpp
// currently wip, compiles but not tested
#define MOONDREAM_BUILD_CGRAPH_WIP
#ifdef MOONDREAM_BUILD_CGRAPH_WIP
struct ggml_cgraph * build_phi2(
    ggml_context * ctx0, 
    moondream_model & model,
    // Can hparams be removed since it's also in moondream_model?
    moondream_hparams & hparams, 
    moondream_cparams & cparams,
    moondream_batch & batch,
    moondream_kv_cache & kv_cache, // TODO: add this to moondream_model or moondream_ctx
    moondream_context & mctx
) {
    // TODO: Fix all the inconsistent integer types

    // NOTE: I think the model tensors have to be loaded before the cgraph is created,
    // in other words, before this function is called

    struct ggml_cgraph * gf = ggml_new_graph_custom(ctx0, LLAMA_MAX_NODES, false);

    const int rope_type = MOONDREAM_ROPE_TYPE;
    int n_rot = hparams.n_rot;
    const int n_head = hparams.n_head;
    const int n_head_kv = hparams.n_head_kv;
    const int n_ctx = cparams.n_ctx;
    
    // NOTE: llama.cpp has some additional initialization logic for n_outputs
    const int n_outputs = mctx.n_outputs;

    // TODO: think about where to put this since it isn't in moondream_batch
    // also figure out where it comes from in the first place
    // NOTE: llama.cpp has some additional initialization logic for n_kv which may be relevant
    // REF:
    // n_kv (worst_case ? kv_self.size : kv_self.n)
    const int32_t n_kv = kv_cache.n;     // size of KV cache to consider (n_kv <= kv_self.size)
    // NOTE: llama.cpp has some additional initialization logic for kv_head which may be relevant
    // REF:
    // kv_head (worst_case ? (kv_self.recurrent ? 0 : kv_self.size - n_tokens) : kv_self.head)
    const int32_t kv_head = kv_cache.head;
    const int32_t n_tokens = batch.n_tokens;
    const int64_t n_layer = hparams.n_layer;
    const int64_t n_embd = hparams.n_embd;
    const int64_t n_embd_head = hparams.n_embd_head_v;
    //const int64_t n_embd_gqa  = hparams.n_embd_v_gqa();
    const int64_t n_embd_gqa = n_embd_head;
    GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);

    const uint32_t n_ctx_orig = cparams.n_ctx_orig_yarn;
    const float freq_base = cparams.rope_freq_base;
    const float freq_scale = cparams.rope_freq_scale;
    const float ext_factor = cparams.yarn_ext_factor;
    const float attn_factor = cparams.yarn_attn_factor;
    const float beta_fast = cparams.yarn_beta_fast;
    const float beta_slow = cparams.yarn_beta_slow;

    struct ggml_tensor * cur;
    struct ggml_tensor * attn_norm_output;
    struct ggml_tensor * ffn_output;
    struct ggml_tensor * inpL;

    // TODO: implement llm_build_inp_embd (see llama.cpp line 6654) - done but needs check
    //inpL = llm_build_inp_embd(ctx0, lctx, hparams, batch, model.tok_embd, cb);
    // NOTE: using a version of llm_build_inp_embd that doesn't use build cb
    inpL = llm_build_inp_embd(ctx0, mctx, hparams, batch, model.tok_embd);
    
    // TODO: implement build_inp_pos (see llama.cpp line 7346)
    // inp_pos - contains the positions
    // NOTE: using a version of llm_build_inp_embd that doesn't use build cb - done but needs check
    struct ggml_tensor * inp_pos = build_inp_pos(ctx0, mctx, batch);

    // TODO: implement build_inp_KQ_mask (see llama.cpp line 7371) - done but needs check
    // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
    struct ggml_tensor * KQ_mask = build_inp_KQ_mask(ctx0, mctx, batch, cparams, n_kv);

    for (int il = 0; il < n_layer; ++il) {
        // TODO: implement llm_build_norm (see llama.cpp line 6728) - done but needs check
        attn_norm_output = llm_build_norm(
            ctx0, inpL, hparams,
            model.layers[il].attn_norm,
            model.layers[il].attn_norm_b,
            // TODO: since LLM_NORM is hardcoded the arg might not be needed
            LLM_NORM, il
        );

        //cb(attn_norm_output, "attn_norm", il);

        // self-attention
        {
            struct ggml_tensor * Qcur = nullptr;
            struct ggml_tensor * Kcur = nullptr;
            struct ggml_tensor * Vcur = nullptr;

            if (model.layers[il].wqkv) {
                cur = ggml_mul_mat(ctx0, model.layers[il].wqkv, attn_norm_output);
                //cb(cur, "wqkv", il);

                cur = ggml_add(ctx0, cur, model.layers[il].bqkv);
                //cb(cur, "bqkv", il);

                Qcur = ggml_cont(
                    ctx0, ggml_view_2d(ctx0, cur, n_embd, n_tokens, cur->nb[1], 0*sizeof(float)*(n_embd))
                );
                Kcur = ggml_cont(
                    ctx0, ggml_view_2d(ctx0, cur, n_embd_gqa, n_tokens, cur->nb[1], 1*sizeof(float)*(n_embd))
                );
                Vcur = ggml_cont(
                    ctx0, 
                    ggml_view_2d(
                        ctx0, cur, n_embd_gqa, n_tokens, cur->nb[1], 1*sizeof(float)*(n_embd + n_embd_gqa)
                    )
                );
            } else {
                Qcur = ggml_add(
                    ctx0, ggml_mul_mat(ctx0, model.layers[il].wq, attn_norm_output), model.layers[il].bq
                );
                Kcur = ggml_add(
                    ctx0, ggml_mul_mat(ctx0, model.layers[il].wk, attn_norm_output), model.layers[il].bk
                );
                Vcur = ggml_add(
                    ctx0, ggml_mul_mat(ctx0, model.layers[il].wv, attn_norm_output), model.layers[il].bv
                );
            }

            //cb(Qcur, "Qcur", il);
            //cb(Kcur, "Kcur", il);
            //cb(Vcur, "Vcur", il);

            Qcur = ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head, n_tokens);
            Kcur = ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens);

            Qcur = ggml_rope_ext(
                ctx0, Qcur, inp_pos, nullptr, n_rot, rope_type, n_ctx, n_ctx_orig,
                freq_base, freq_scale, ext_factor, attn_factor, beta_fast, beta_slow
            );
            //cb(Qcur, "Qcur", il);

            // with phi2, we scale the Q to avoid precision issues
            // ref: https://github.com/ml-explore/mlx-examples/blob/08e862336ade809bc37d1035f94b359e7d1a5152/phi2/phi2.py#L64-L66
            Qcur = ggml_scale(ctx0, Qcur, 1.0f/sqrtf(float(n_embd_head)));
            //cb(Qcur, "Qcur", il);

            Kcur = ggml_rope_ext(
                ctx0, Kcur, inp_pos, nullptr, n_rot, rope_type, n_ctx, n_ctx_orig,
                freq_base, freq_scale, ext_factor, attn_factor, beta_fast, beta_slow
            );
            //cb(Kcur, "Kcur", il);

            // TODO: implement llm_build_kv (see llama.cpp line 7070) - done but needs check
            cur = llm_build_kv(
                ctx0, model, hparams, cparams, kv_cache, gf,
                model.layers[il].wo, model.layers[il].bo,
                Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f, il
            );
        }

        if (il == n_layer - 1) {
            // TODO: implement build_inp_out_ids (see llama.cpp line 7464) - done but needs check
            // skip computing output for unused tokens
            struct ggml_tensor * inp_out_ids = build_inp_out_ids(ctx0, mctx, n_outputs);
            cur = ggml_get_rows(ctx0, cur, inp_out_ids);
            inpL = ggml_get_rows(ctx0, inpL, inp_out_ids);
            attn_norm_output = ggml_get_rows(ctx0, attn_norm_output, inp_out_ids);
        }

        // FF
        {
            // TODO: implement llm_build_ffn (see llama.cpp line 6760) - done but needs check
            ffn_output = llm_build_ffn(
                ctx0, attn_norm_output,
                model.layers[il].ffn_up, model.layers[il].ffn_up_b,
                NULL, NULL, /* I guess this means that phi2 doesn't have a ff gate */
                model.layers[il].ffn_down, model.layers[il].ffn_down_b,
                NULL, LLM_FFN_GELU, LLM_FFN_SEQ, il
            );
            //cb(ffn_output, "ffn_out", il);
        }

        cur = ggml_add(ctx0, cur, ffn_output);
        //cb(cur, "l_out", il);

        cur = ggml_add(ctx0, cur, inpL);
        //cb(cur, "l_out", il);

        inpL = cur;
    }

    // TODO: implement llm_build_norm (see llama.cpp line 6728) - done but needs check
    cur = llm_build_norm(
        ctx0, inpL, hparams,
        model.output_norm,
        model.output_norm_b,
        LLM_NORM, -1
    );
    //cb(cur, "result_norm", -1);

    cur = ggml_mul_mat(ctx0, model.output, cur);
    //cb(cur, "result_output_no_bias", -1);

    cur = ggml_add(ctx0, cur, model.output_b);
    //cb(cur, "result_output", -1);
    ggml_build_forward_expand(gf, cur);
    return gf;
}
#endif // MOONDREAM_BUILD_CGRAPH_WIP

/*
TODO: remove this later
REFERENCE: phi2 layer names from llama.cpp:

        LLM_ARCH_PHI3,
        {
            { LLM_TENSOR_TOKEN_EMBD,         "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,        "output_norm" },
            { LLM_TENSOR_OUTPUT,             "output" },
            { LLM_TENSOR_ROPE_FACTORS_LONG,  "rope_factors_long" },
            { LLM_TENSOR_ROPE_FACTORS_SHORT, "rope_factors_short" },
            { LLM_TENSOR_ATTN_NORM,          "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_QKV,           "blk.%d.attn_qkv" },
            { LLM_TENSOR_ATTN_Q,             "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,             "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,             "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,           "blk.%d.attn_output" },
            { LLM_TENSOR_FFN_NORM,           "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_DOWN,           "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,             "blk.%d.ffn_up" },
*/
bool moondream_load_model(const char * gguf_file_path, moondream_model * model) {
    gguf_init_params init_params = {.no_alloc = true, .ctx = nullptr};
    gguf_context * ctx = gguf_init_from_file(gguf_file_path, init_params);
    int gguf_version = gguf_get_version(ctx);
    size_t gguf_alignment = gguf_get_alignment(ctx);
    size_t gguf_data_offset = gguf_get_data_offset(ctx);
    
    const char * model_arch = gguf_get_val_str(ctx, gguf_find_key(ctx, "general.architecture"));
    
    moondream_hparams hparams;
    const char * model_name = gguf_get_val_str(ctx, gguf_find_key(ctx, "general.name"));
    hparams.n_ctx_train = gguf_get_val_u32(ctx, gguf_find_key(ctx, ARCH_PREFIX("context_length")));
    hparams.n_embd = gguf_get_val_u32(ctx, gguf_find_key(ctx, ARCH_PREFIX("embedding_length")));
    hparams.n_rot = gguf_get_val_u32(ctx, gguf_find_key(ctx, ARCH_PREFIX("rope.dimension_count")));
    hparams.n_layer = gguf_get_val_u32(ctx, gguf_find_key(ctx, ARCH_PREFIX("block_count")));
    hparams.n_ff = gguf_get_val_u32(ctx, gguf_find_key(ctx, ARCH_PREFIX("block_count")));
    hparams.n_head = gguf_get_val_u32(ctx, gguf_find_key(ctx, ARCH_PREFIX("attention.head_count")));
    hparams.n_head_kv = gguf_get_val_u32(ctx, gguf_find_key(ctx, ARCH_PREFIX("attention.head_count_kv")));
    
    // n_head_k and n_head_v are not specified, so calculate them according to the gguf documentation
    // "If not specified, it will be `n_embd / n_head`"
    // TODO: remove these commented lines later, just keeping them as a reference for now
    //hparams.n_embd_head_k = gguf_get_val_u32(ctx, gguf_find_key(ctx, ARCH_PREFIX("attention.value_length")));
    //hparams.n_embd_head_k = gguf_get_val_u32(ctx, gguf_find_key(ctx, ARCH_PREFIX("attention.key_length")));
    hparams.n_embd_head_k = hparams.n_embd / hparams.n_head;
    hparams.n_embd_head_v = hparams.n_embd_head_k;

    printf("loaded %s from %s\n", model_name, gguf_file_path);
    printf("gguf version: %d\n", gguf_version);
    printf("gguf alignment: %ld\n", gguf_alignment);
    printf("gguf data offset: %ld\n", gguf_data_offset);
    printf("model architecture: %s\n", model_arch);
    printf("context length: %d\n", hparams.n_ctx_train);
    printf("embedding length: %d\n", hparams.n_embd);
    printf("block count: %d\n", hparams.n_layer);
    printf("feed forward length: %d\n", hparams.n_ff);
    printf("head count: %d\n", hparams.n_head);
    printf("head count kv: %d\n", hparams.n_head_kv);
    printf("n_embd_head_k: %d\n", hparams.n_embd_head_k);
    printf("n_embd_head_v: %d\n", hparams.n_embd_head_v);
    return true;
}

int main(int argc, char * argv[]) {
    if (argc < 2) {
        printf("incorrect number of arguments\n");
        return 1;
    }
    const char * data_path = argv[1];
    const size_t data_path_length = strlen(data_path);
    if (data_path_length > DATA_PATH_MAX_LEN) {
        printf("provided data path exceeded max length");
        return 1;
    }

    // resolve text model file path
    const char * text_model_fname = MD_TEXT_MODEL_FNAME;
    const size_t text_model_fname_length = strlen(text_model_fname);
    // add 1 to give space for null-terminator in concatenated string
    const size_t text_model_path_length = data_path_length + text_model_fname_length + 1;
    char text_model_path[text_model_path_length];
    snprintf(text_model_path, text_model_path_length, "%s%s", data_path, text_model_fname); 

    // resolve mmproj file path
    const char * mmproj_fname = MD_MMPROJ_FNAME;
    const size_t mmproj_fname_length = strlen(mmproj_fname);
    // add 1 to give space for null-terminator in concatenated string
    const size_t mmproj_path_length = data_path_length + text_model_fname_length + 1;
    char mmproj_path[text_model_path_length];
    snprintf(mmproj_path, mmproj_path_length, "%s%s", data_path, mmproj_fname); 

    printf("text model path: %s\n", text_model_path);
    printf("mmproj path: %s\n", mmproj_path);
    
    moondream_model model;
    bool result = moondream_load_model(text_model_path, &model);
    if (result == false) {
        printf("could not load model\n");
    } else {
        printf("succesfully loaded model\n");
    }
    return 0;
}
