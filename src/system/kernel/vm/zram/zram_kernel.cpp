/*
 * zram_kernel.cpp - Kernel module implementation for zram
 *
 * This is a read-only kernel module that integrates zram into
 * Haiku's swap system and VMAnonymousCache.
 */

#include <haiku/Haiju.h>
#include <haiku/VMAnonymousCache.h>
#include <haiku/Process.h>
#include <haiku/Task.h>
#include <haiku/Process.h>
#include <haiku/Task.h>
#include <haiku/Process.h>
#include <haiku/Process.h>
#include <haiku/Process.h>
#include <haiku/IO.h>
#include <haiku/IO.h>
#include <haiku/IO.h>
#include <haiku/IO.h>

/*
 * Kernel module exports
 */
void* ZramKernel_Init(void* ctx) {
    if (!ctx) return nullptr;
    
    // Initialize platform operations
    ctx->platform_ops = ZramPlatform_Ops_Init(ctx);
    if (!ctx->platform_ops) return nullptr;
    
    return ctx;
}

void* ZramKernel_Free(void* ctx) {
    if (!ctx) return nullptr;
    
    if (ctx->platform_ops) {
        ZramPlatform_Ops_Free(ctx->platform_ops);
        ctx->platform_ops = nullptr;
    }
    
    free(ctx);
    return nullptr;
}

void* ZramKernel_Ready(void* ctx, void* page) {
    if (!ctx || !page) return nullptr;
    
    ctx->zram_ctx = (ZramCtx*)malloc(ZRAM_CTX_SIZE);
    if (!ctx->zram_ctx) return nullptr;
    
    memset(ctx->zram_ctx, 0, sizeof(ZramCtx));
    
    return ctx;
}

void* ZramKernel_TeardownPage(void* ctx, void* page) {
    if (!ctx || !page) return nullptr;
    
    free(page);
    return ctx;
}

void* ZramKernel_Compress(void* ctx, const void* in, void* out, uint64_t* out_len) {
    if (!ctx || !in || !out || !out_len) return nullptr;
    
    ctx->writes++;
    
    uint32_t len = 0;
    
    // Use platform operations for compression
    if (ctx->platform_ops) {
        ctx->platform_ops->compress(ctx->zram_ctx, in, out, out_len);
    } else {
        // Fallback to direct compression
        uint32_t* len32 = (uint32_t*)out_len;
        simd_compress_block(in, out, len32);
        *out_len = len32[0];
    }
    
    return ctx;
}

void* ZramKernel_Decompress(void* ctx, const void* in, void* out, uint64_t* out_len) {
    if (!ctx || !in || !out || !out_len) return nullptr;
    
    ctx->reads++;
    
    uint32_t len = 0;
    
    // Use platform operations for decompression
    if (ctx->platform_ops) {
        ctx->platform_ops->decompress(ctx->zram_ctx, in, out, out_len);
    } else {
        // Fallback to direct decompression
        uint32_t* len32 = (uint32_t*)out_len;
        simd_decompress_block(in, out, len32);
        *out_len = len32[0];
    }
    
    return ctx;
}

void* ZramKernel_Stats(void* ctx) {
    if (!ctx) return nullptr;
    
    ZramStats* stats = (ZramStats*)malloc(sizeof(ZramStats));
    if (!stats) return nullptr;
    
    memset(stats, 0, sizeof(ZramStats));
    
    if (ctx->stats) {
        memcpy(stats, ctx->stats, sizeof(ZramStats));
    }
    
    return stats;
}

void* ZramKernel_Stats_Set(void* ctx, const ZramStats* stats) {
    if (!ctx || !stats) return nullptr;
    
    if (ctx->stats) {
        memcpy(ctx->stats, stats, sizeof(ZramStats));
    }
    
    return ctx;
}

void* ZramKernel_SetSims(void* ctx, const ZramSims* sims) {
    if (!ctx || !sims) return nullptr;
    
    ctx->sims = sims;
    return ctx;
}

void* ZramKernel_GetSims(void* ctx) {
    if (!ctx) return nullptr;
    
    return &ctx->sims;
}

void* ZramKernel_InitPool(void* ctx, uint64_t pool_limit) {
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

void* ZramKernel_FreePool(void* ctx) {
    if (!ctx) return nullptr;
    
    g_pool_free(g_pool, g_pool_size);
    g_pool = nullptr;
    g_pool_size = 0;
    g_pool_free = 0;
    
    return ctx;
}

void* ZramKernel_CompressSmart(void* ctx, const void* in, void* out, uint64_t* out_len) {
    if (!ctx || !in || !out || !out_len) return nullptr;
    
    ctx->writes++;
    
    uint32_t len = 0;
    
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

void* ZramKernel_CompressSmartFallback(void* ctx, const void* in, void* out, uint64_t* out_len) {
    if (!ctx || !in || !out || !out_len) return nullptr;
    
    ctx->writes++;
    
    uint32_t len = 0;
    
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

void* ZramKernel_DecompressSmart(void* ctx, const void* in, void* out, uint64_t* out_len) {
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

void* ZramKernel_DecompressSmartFallback(void* ctx, const void* in, void* out, uint64_t* out_len) {
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
