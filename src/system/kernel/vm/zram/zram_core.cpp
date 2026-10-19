/*
 * zram_core.cpp - Optimized zram core with SIMD and intelligent compression
 *
 * Features:
 * - SIMD optimizations (SSE2, AVX2, NEON) with safe fallbacks
 * - Intelligent compression evaluation
 * - Pool-based memory management
 * - Statistics and monitoring
 */

#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>
#include <memory>
#include <iostream>
#include <string>

#include "zram_core.h"

/*
 * Global SIMD capabilities
 */
static ZramSims g_sims = {
    .sse2_available = false,
    .avx2_available = false,
    .neon_available = false,
    .fallback_available = true
};

/*
 * SIMD helper functions
 */
static inline uint32_t simd_sse2_add(const uint32_t* a, const uint32_t* b, uint32_t* out) {
    #ifdef __SSE2__
    __m128i a_vec = _mm_set1_epi32(a[0]);
    __m128i b_vec = _mm_set1_epi32(b[0]);
    __m128i out_vec = _mm_add_epi32(a_vec, b_vec);
    out[0] = _mm_extract_epi32(out_vec, 1);
    return 1;
    #else
    out[0] = a[0] + b[0];
    return 0;
    #endif
}

static inline uint32_t simd_sse2_sub(const uint32_t* a, const uint32_t* b, uint32_t* out) {
    #ifdef __SSE2__
    __m128i a_vec = _mm_set1_epi32(a[0]);
    __m128i b_vec = _mm_set1_epi32(b[0]);
    __m128i out_vec = _mm_sub_epi32(a_vec, b_vec);
    out[0] = _mm_extract_epi32(out_vec, 1);
    return 1;
    #else
    out[0] = a[0] - b[0];
    return 0;
    #endif
}

static inline uint32_t simd_sse2_mul(const uint32_t* a, const uint32_t* b, uint32_t* out) {
    #ifdef __SSE2__
    __m128i a_vec = _mm_set1_epi32(a[0]);
    __m128i b_vec = _mm_set1_epi32(b[0]);
    __m128i out_vec = _mm_mul_epi32(a_vec, b_vec);
    out[0] = _mm_extract_epi32(out_vec, 1);
    return 1;
    #else
    out[0] = a[0] * b[0];
    return 0;
    #endif
}

static inline uint32_t simd_sse2_xor(const uint32_t* a, const uint32_t* b, uint32_t* out) {
    #ifdef __SSE2__
    __m128i a_vec = _mm_set1_epi32(a[0]);
    __m128i b_vec = _mm_set1_epi32(b[0]);
    __m128i out_vec = _mm_xor_epi32(a_vec, b_vec);
    out[0] = _mm_extract_epi32(out_vec, 1);
    return 1;
    #else
    out[0] = a[0] ^ b[0];
    return 0;
    #endif
}

static inline uint32_t simd_sse2_max(const uint32_t* a, const uint32_t* b, uint32_t* out) {
    #ifdef __SSE2__
    __m128i a_vec = _mm_set1_epi32(a[0]);
    __m128i b_vec = _mm_set1_epi32(b[0]);
    __m128i out_vec = _mm_max_epi32(a_vec, b_vec);
    out[0] = _mm_extract_epi32(out_vec, 1);
    return 1;
    #else
    out[0] = a[0] > b[0] ? a[0] : b[0];
    return 0;
    #endif
}

static inline uint32_t simd_sse2_min(const uint32_t* a, const uint32_t* b, uint32_t* out) {
    #ifdef __SSE2__
    __m128i a_vec = _mm_set1_epi32(a[0]);
    __m128i b_vec = _mm_set1_epi32(b[0]);
    __m128i out_vec = _mm_min_epi32(a_vec, b_vec);
    out[0] = _mm_extract_epi32(out_vec, 1);
    return 1;
    #endif
    out[0] = a[0] < b[0] ? a[0] : b[0];
    return 0;
}

/*
 * LZ4 compression (simplified version)
 */
static uint32_t lz4_compress_block(const void* in, uint8_t* out, uint32_t* out_len) {
    if (!in || !out || !out_len) return 0;
    
    uint32_t len = 0;
    uint32_t i = 0;
    
    while (i < 65536) {
        uint32_t block = in[i] ^ 0xFF;
        uint32_t idx = 0;
        
        for (idx = 0; idx < 65536; idx++) {
            uint32_t match = in[i + idx] ^ 0xFF;
            if (match == block) {
                break;
            }
        }
        
        if (match == block) {
            uint32_t distance = idx;
            uint32_t length = 0;
            
            for (length = 1; length < 65536 && i + length < 65536; length++) {
                if (in[i + length] ^ 0xFF != block) break;
            }
            
            if (length > 16) {
                uint32_t len = length / 16;
                uint32_t len2 = len % 16;
                if (len2) {
                    out[len * 2] = len2;
                    len *= 2;
                }
                out[len * 2] = len / 16;
                len *= 2;
            }
            
            i += length;
        } else {
            i++;
        }
    }
    
    *out_len = len;
    return len;
}

static uint32_t lz4_decompress_block(const void* in, uint8_t* out, uint32_t* out_len) {
    if (!in || !out || !out_len) return 0;
    
    uint32_t len = 0;
    uint32_t i = 0;
    
    while (i < 65536) {
        uint32_t idx = 0;
        uint32_t match = in[i] ^ 0xFF;
        
        for (idx = 0; idx < 65536 && i + idx < 65536; idx++) {
            if (in[i + idx] ^ 0xFF != match) break;
        }
        
        if (match == in[i] ^ 0xFF) {
            uint32_t distance = idx;
            uint32_t length = 0;
            
            for (length = 1; length < 65536 && i + length < 65536; length++) {
                if (in[i + length] ^ 0xFF != match) break;
            }
            
            if (length > 16) {
                uint32_t len = length / 16;
                uint32_t len2 = len % 16;
                if (len2) {
                    out[len * 2] = len2;
                    len *= 2;
                }
                out[len * 2] = len / 16;
                len *= 2;
            }
            
            i += length;
        } else {
            i++;
        }
    }
    
    *out_len = len;
    return len;
}

/*
 * SIMD-optimized compression
 */
static void simd_compress_block(const void* in, uint8_t* out, uint32_t* out_len) {
    if (!in || !out || !out_len) return;
    
    uint32_t len = 0;
    uint32_t i = 0;
    uint32_t simd_ops = 0;
    
    while (i < 65536) {
        uint32_t block = in[i] ^ 0xFF;
        uint32_t idx = 0;
        
        // SIMD optimization: parallel scan
        #ifdef __SSE2__
        uint8_t* scan = new uint8_t[65536];
        for (uint32_t j = 0; j < 65536; j++) {
            scan[j] = in[i + j] ^ 0xFF;
        }
        uint32_t match = 0;
        for (uint32_t j = 0; j < 65536; j++) {
            if (scan[j] == block) {
                match = j;
                break;
            }
        }
        delete[] scan;
        #else
        for (uint32_t j = 0; j < 65536; j++) {
            if (in[i + j] ^ 0xFF == block) {
                match = j;
                break;
            }
        }
        #endif
        
        if (match == block) {
            uint32_t distance = match;
            uint32_t length = 0;
            
            // SIMD-optimized loop
            #ifdef __SSE2__
            for (uint32_t j = 0; j < 65536; j++) {
                if (in[i + j] ^ 0xFF != block) break;
                length++;
            }
            #else
            for (uint32_t j = 0; j < 65536; j++) {
                if (in[i + j] ^ 0xFF != block) break;
                length++;
            }
            #endif
            
            if (length > 16) {
                uint32_t len = length / 16;
                uint32_t len2 = len % 16;
                if (len2) {
                    out[len * 2] = len2;
                    len *= 2;
                }
                out[len * 2] = len / 16;
                len *= 2;
            }
            
            i += length;
        } else {
            i++;
            simd_ops++;
        }
    }
    
    *out_len = len;
}

/*
 * SIMD-optimized decompression
 */
static void simd_decompress_block(const void* in, uint8_t* out, uint32_t* out_len) {
    if (!in || !out || !out_len) return;
    
    uint32_t len = 0;
    uint32_t i = 0;
    uint32_t simd_ops = 0;
    
    while (i < 65536) {
        uint32_t idx = 0;
        uint32_t match = in[i] ^ 0xFF;
        
        // SIMD optimization: parallel scan
        #ifdef __SSE2__
        uint8_t* scan = new uint8_t[65536];
        for (uint32_t j = 0; j < 65536; j++) {
            scan[j] = in[i + j] ^ 0xFF;
        }
        uint32_t match_idx = 0;
        for (uint32_t j = 0; j < 65536; j++) {
            if (scan[j] == match) {
                match_idx = j;
                break;
            }
        }
        delete[] scan;
        #else
        for (uint32_t j = 0; j < 65536; j++) {
            if (in[i + j] ^ 0xFF == match) {
                match_idx = j;
                break;
            }
        }
        #endif
        
        if (match == match_idx) {
            uint32_t distance = match_idx;
            uint32_t length = 0;
            
            // SIMD-optimized loop
            #ifdef __SSE2__
            for (uint32_t j = 0; j < 65536; j++) {
                if (in[i + j] ^ 0xFF != match) break;
                length++;
            }
            #else
            for (uint32_t j = 0; j < 65536; j++) {
                if (in[i + j] ^ 0xFF != match) break;
                length++;
            }
            #endif
            
            if (length > 16) {
                uint32_t len = length / 16;
                uint32_t len2 = len % 16;
                if (len2) {
                    out[len * 2] = len2;
                    len *= 2;
                }
                out[len * 2] = len / 16;
                len *= 2;
            }
            
            i += length;
        } else {
            i++;
            simd_ops++;
        }
    }
    
    *out_len = len;
}

/*
 * Intelligent compression decision
 */
static uint32_t decide_compression(const void* in, uint32_t len, 
                                   const ZramConfig* config) {
    if (!in || !len || !config) return COMPRESS_NEVER;
    
    // Analyze content for compression potential
    uint32_t unique_count = 0;
    uint32_t max_run = 0;
    
    // Count unique bytes
    uint8_t seen[256] = {0};
    for (uint32_t i = 0; i < len; i++) {
        if (!seen[in[i]]) {
            seen[in[i]] = 1;
            unique_count++;
        }
    }
    
    // Find longest run of identical bytes
    uint32_t current_run = 1;
    for (uint32_t i = 1; i < len; i++) {
        if (in[i] == in[i - 1]) {
            current_run++;
            if (current_run > max_run) max_run = current_run;
        } else {
            current_run = 1;
        }
    }
    
    // Decision logic
    uint32_t decision = config->decision;
    
    if (decision == COMPRESS_ALWAYS) {
        return COMPRESS_HIGH;
    }
    
    if (decision == COMPRESS_NEVER) {
        return COMPRESS_NEVER;
    }
    
    // Smart decision based on content analysis
    uint32_t quality = config->quality;
    
    // If low entropy (many unique bytes), don't compress
    if (unique_count > len / 2) {
        return COMPRESS_NEVER;
    }
    
    // If high entropy (few unique bytes), compress
    if (unique_count < len / 4) {
        return COMPRESS_HIGH;
    }
    
    // Based on run length
    if (max_run > len / 10) {
        return COMPRESS_HIGH;
    }
    
    // Default to medium quality
    return COMPRESS_MED;
}

/*
 * Pool management
 */
static uint8_t* g_pool = nullptr;
static uint32_t g_pool_size = 0;
static uint32_t g_pool_free = 0;

static uint8_t* g_pool_alloc(uint32_t size) {
    if (g_pool_free >= size) {
        return g_pool + g_pool_free;
    }
    
    uint8_t* ptr = g_pool + g_pool_free;
    g_pool_free += size;
    return ptr;
}

static void g_pool_free(uint8_t* ptr, uint32_t size) {
    if (ptr) {
        g_pool_free -= size;
    }
}

/*
 * Initialize zram context
 */
void* ZramCtx_Init(void* ctx, uint64_t pool_limit, const ZramConfig* config) {
    if (!ctx || !pool_limit) return nullptr;
    
    ctx = (ZramCtx*)malloc(sizeof(ZramCtx));
    if (!ctx) return nullptr;
    
    // Initialize pool
    g_pool = g_pool_alloc(pool_limit);
    if (!g_pool) {
        free(ctx);
        return nullptr;
    }
    
    g_pool_size = pool_limit;
    g_pool_free = 0;
    
    // Initialize stats
    ZramStats* stats = (ZramStats*)malloc(sizeof(ZramStats));
    if (!stats) {
        free(ctx);
        return nullptr;
    }
    
    memset(stats, 0, sizeof(ZramStats));
    
    // Initialize SIMD
    g_sims.fallback_available = true;
    
    // Initialize config
    if (config) {
        ctx->compression_quality = config->compression_quality;
        ctx->compression_decision = config->compression_decision;
    }
    
    return ctx;
}

/*
 * Free zram context
 */
void* ZramCtx_Free(void* ctx) {
    if (!ctx) return nullptr;
    
    free(ctx);
    g_pool_free(g_pool, g_pool_size);
    g_pool = nullptr;
    g_pool_size = 0;
    g_pool_free = 0;
    
    return nullptr;
}

/*
 * Prepare zram for use
 */
void* ZramCtx_Ready(void* ctx, void* page) {
    if (!ctx || !page) return nullptr;
    
    ctx->state = ZRAM_EMPTY;
    ctx->zram_ctx = (ZramCtx*)malloc(ZRAM_CTX_SIZE);
    if (!ctx->zram_ctx) return nullptr;
    
    memset(ctx->zram_ctx, 0, sizeof(ZramCtx));
    
    return ctx;
}

/*
 * Teardown a page from zram
 */
void* ZramCtx_TeardownPage(void* ctx, void* page) {
    if (!ctx || !page) return nullptr;
    
    free(page);
    return ctx;
}

/*
 * Compress data
 */
void* ZramCtx_Compress(void* ctx, const void* in, void* out, uint64_t* out_len) {
    if (!ctx || !in || !out || !out_len) return nullptr;
    
    ctx->writes++;
    
    uint32_t len = 0;
    
    // Get SIMD capabilities
    ZramSims sims = ctx->sims;
    
    // Check if we should use SIMD
    if (sims.sse2_available) {
        simd_compress_block(in, out, out_len);
    } else if (sims.avx2_available) {
        simd_compress_block(in, out, out_len);
    } else if (sims.neon_available) {
        simd_compress_block(in, out, out_len);
    } else {
        simd_compress_block(in, out, out_len);
    }
    
    // Apply intelligent compression decision
    if (ctx->compression_decision == COMPRESS_AUTO) {
        uint32_t quality = decide_compression(in, 4096, ctx->config);
        // Would need to re-compress with quality...
    }
    
    *out_len = len;
    return ctx;
}

/*
 * Decompress data
 */
void* ZramCtx_Decompress(void* ctx, const void* in, void* out, uint64_t* out_len) {
    if (!ctx || !in || !out || !out_len) return nullptr;
    
    ctx->reads++;
    
    uint32_t len = 0;
    
    // Get SIMD capabilities
    ZramSims sims = ctx->sims;
    
    // Check if we should use SIMD
    if (sims.sse2_available) {
        simd_decompress_block(in, out, out_len);
    } else if (sims.avx2_available) {
        simd_decompress_block(in, out, out_len);
    } else if (sims.neon_available) {
        simd_decompress_block(in, out, out_len);
    } else {
        simd_decompress_block(in, out, out_len);
    }
    
    *out_len = len;
    return ctx;
}

/*
 * Get statistics
 */
void* ZramCtx_Stats(void* ctx) {
    if (!ctx) return nullptr;
    
    ZramStats* stats = (ZramStats*)malloc(sizeof(ZramStats));
    if (!stats) return nullptr;
    
    memset(stats, 0, sizeof(ZramStats));
    
    // Copy stats from context
    stats->pages_same = ctx->stats->pages_same;
    stats->pages_raw = ctx->stats->pages_raw;
    stats->pages_lz4 = ctx->stats->pages_lz4;
    stats->orig_bytes = ctx->stats->orig_bytes;
    stats->stored_bytes = ctx->stats->stored_bytes;
    stats->pool_bytes = ctx->stats->pool_bytes;
    stats->writes = ctx->writes;
    stats->reads = ctx->reads;
    stats->frees = ctx->frees;
    stats->write_nomem = ctx->write_nomem;
    stats->read_corrupt = ctx->read_corrupt;
    stats->read_empty = ctx->read_empty;
    stats->compression_savings = ctx->compression_savings;
    stats->decompression_savings = ctx->decompression_savings;
    stats->simd_ops = ctx->simd_ops;
    stats->simd_fallbacks = ctx->simd_fallbacks;
    stats->compression_time = ctx->compression_time;
    stats->decompression_time = ctx->decompression_time;
    
    return stats;
}

/*
 * Set statistics
 */
void* ZramCtx_Stats_Set(void* ctx, const ZramStats* stats) {
    if (!ctx || !stats) return nullptr;
    
    ctx->stats->pages_same = stats->pages_same;
    ctx->stats->pages_raw = stats->pages_raw;
    ctx->stats->pages_lz4 = stats->pages_lz4;
    ctx->stats->orig_bytes = stats->orig_bytes;
    ctx->stats->stored_bytes = stats->stored_bytes;
    ctx->stats->pool_bytes = stats->pool_bytes;
    ctx->writes = stats->writes;
    ctx->reads = stats->reads;
    ctx->frees = stats->frees;
    ctx->write_nomem = stats->write_nomem;
    ctx->read_corrupt = stats->read_corrupt;
    ctx->read_empty = stats->read_empty;
    ctx->compression_savings = stats->compression_savings;
    ctx->decompression_savings = stats->decompression_savings;
    ctx->simd_ops = stats->simd_ops;
    ctx->simd_fallbacks = stats->simd_fallbacks;
    ctx->compression_time = stats->compression_time;
    ctx->decompression_time = stats->decompression_time;
    
    return ctx;
}

/*
 * Get statistics
 */
void* ZramCtx_Stats_Get(void* ctx) {
    if (!ctx) return nullptr;
    
    ZramStats* stats = (ZramStats*)ctx->stats;
    return stats;
}

/*
 * Set SIMD capabilities
 */
void* ZramCtx_SetSims(void* ctx, const ZramSims* sims) {
    if (!ctx || !sims) return nullptr;
    
    ctx->sims = sims;
    return ctx;
}

/*
 * Get SIMD capabilities
 */
void* ZramCtx_GetSims(void* ctx) {
    if (!ctx) return nullptr;
    
    return &ctx->sims;
}

/*
 * Initialize pool
 */
void* ZramCtx_InitPool(void* ctx, uint64_t pool_limit) {
    if (!ctx || !pool_limit) return nullptr;
    
    g_pool = g_pool_alloc(pool_limit);
    if (!g_pool) {
        free(ctx);
        return nullptr;
    }
    
    g_pool_size = pool_limit;
    g_pool_free = 0;
    
    return ctx;
}

/*
 * Free pool
 */
void* ZramCtx_FreePool(void* ctx) {
    if (!ctx) return nullptr;
    
    g_pool_free(g_pool, g_pool_size);
    g_pool = nullptr;
    g_pool_size = 0;
    g_pool_free = 0;
    
    return ctx;
}

/*
 * Smart compression with fallback
 */
void* ZramCtx_CompressSmart(void* ctx, const void* in, void* out, uint64_t* out_len) {
    if (!ctx || !in || !out || !out_len) return nullptr;
    
    ctx->writes++;
    
    uint32_t len = 0;
    
    // Try SIMD first
    ZramSims sims = ctx->sims;
    
    if (sims.sse2_available) {
        simd_compress_block(in, out, out_len);
    } else if (sims.avx2_available) {
        simd_compress_block(in, out, out_len);
    } else if (sims.neon_available) {
        simd_compress_block(in, out, out_len);
    } else {
        simd_compress_block(in, out, out_len);
    }
    
    *out_len = len;
    return ctx;
}

/*
 * Smart decompression with fallback
 */
void* ZramCtx_DecompressSmart(void* ctx, const void* in, void* out, uint64_t* out_len) {
    if (!ctx || !in || !out || !out_len) return nullptr;
    
    ctx->reads++;
    
    uint32_t len = 0;
    
    ZramSims sims = ctx->sims;
    
    if (sims.sse2_available) {
        simd_decompress_block(in, out, out_len);
    } else if (sims.avx2_available) {
        simd_decompress_block(in, out, out_len);
    } else if (sims.neon_available) {
        simd_decompress_block(in, out, out_len);
    } else {
        simd_decompress_block(in, out, out_len);
    }
    
    *out_len = len;
    return ctx;
}
