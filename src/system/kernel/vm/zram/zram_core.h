/*
 * zram_core.h - Optimized zram core with SIMD and intelligent compression
 *
 * Features:
 * - Intelligent compression evaluation (analyzes page contents)
 * - SIMD optimizations with safe fallbacks
 * - Pool-based memory management
 * - Statistics and monitoring
 */

#ifndef ZRAM_CORE_H
#define ZRAM_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <cstring>
#include <cstdint>

/*
 * Constants
 */
#define ZRAM_PAGE_SIZE      4096u
#define ZRAM_SPAN_BYTES     (64u * 1024u)
#define ZRAM_CHUNK_BYTES    (1024u * 1024u)
#define ZRAM_STRIPES        64u
#define ZRAM_MAX_COMPRESSED 3072u
#define ZRAM_CLASS_COUNT    10u

/*
 * SIMD optimization flags
 */
#define SIMD_SSE2       0x01
#define SIMD_AVX2       0x02
#define SIMD_NEON       0x04
#define SIMD_FALLBACK   0x08

/*
 * Compression quality levels
 */
#define ZRAM_COMPRESSION_LOW    0x01  /* Fast, low quality */
#define ZRAM_COMPRESSION_MED    0x02  /* Balanced */
#define ZRAM_COMPRESSION_HIGH   0x03  /* Best quality, slower */

/*
 * Compression quality flags
 */
#define COMPRESS_LOW        0x01
#define COMPRESS_MED        0x02
#define COMPRESS_HIGH       0x03

/*
 * Compression decisions
 */
#define COMPRESS_ALWAYS     0x01
#define COMPRESS_NEVER      0x02
#define COMPRESS_AUTO       0x03  /* Analyze content and decide */
#define COMPRESS_SMART       0x04  /* Only compress if beneficial */

/*
 * Compression quality decisions
 */
#define COMPRESS_QUALITY_LOW       0x01  /* Compress to low quality */
#define COMPRESS_QUALITY_MED       0x02  /* Compress to medium quality */
#define COMPRESS_QUALITY_HIGH      0x03  /* Compress to high quality */

/*
 * Compression decision flags
 */
#define DECIDE_COMPRESS         0x01
#define DECIDE_DECOMPRESS        0x02
#define DECIDE_NO_OP             0x03

/*
 * Pool management
 */
#define ZRAM_POOL_MIN_PAGES    16u
#define ZRAM_POOL_MAX_PAGES    256u

/*
 * Statistics
 */
struct ZramStats {
    uint64_t pages_same;
    uint64_t pages_raw;
    uint64_t pages_lz4;
    uint64_t orig_bytes;
    uint64_t stored_bytes;
    uint64_t pool_bytes;
    uint64_t writes;
    uint64_t reads;
    uint64_t frees;
    uint64_t write_nomem;
    uint64_t read_corrupt;
    uint64_t read_empty;
    uint64_t compression_savings;
    uint64_t decompression_savings;
    uint64_t simd_ops;
    uint64_t simd_fallbacks;
    uint64_t compression_time;
    uint64_t decompression_time;
};

/*
 * SIMD capabilities
 */
struct ZramSims {
    bool sse2_available;
    bool avx2_available;
    bool neon_available;
    bool fallback_available;
};

/*
 * Compression quality settings
 */
struct ZramCompress {
    uint8_t quality;  /* ZRAM_COMPRESSION_LOW/MED/HIGH */
    uint8_t decision; /* COMPRESS_ALWAYS/NEVER/AUTO/SMART */
    uint8_t fallback; /* SIMD fallback level */
};

/*
 * ZramSlot - exactly 16 bytes (static_asserted)
 */
struct ZramSlot {
    union {
        void*    data;       /* RAW or LZ4: pointer to object */
        uint64_t pattern;    /* SAME: repeated pattern word */
    };
    uint32_t size;           /* bytes compressed (LZ4); 4096 (RAW); 0 otherwise */
    uint8_t  state;          /* ZramState */
    uint8_t  cls;            /* class index */
    uint16_t reserved;
};

/*
 * ZramConfig - configuration passed at creation time
 */
struct ZramConfig {
    uint64_t disk_bytes;
    uint64_t pool_limit;
    uint64_t compression_quality;
    uint64_t compression_decision;
};

/*
 * ZramCtx - opaque context per device
 */
struct ZramCtx;

/*
 * Platform operations
 */
extern "C" {
    void* ZramCtx_Init(void* ctx, uint64_t pool_limit, const ZramConfig* config);
    void* ZramCtx_Free(void* ctx);
    void* ZramCtx_Ready(void* ctx, void* page);
    void* ZramCtx_TeardownPage(void* ctx, void* page);
    void* ZramCtx_Compress(void* ctx, const void* in, void* out, uint64_t* out_len);
    void* ZramCtx_Decompress(void* ctx, const void* in, void* out, uint64_t* out_len);
    void* ZramCtx_Stats(void* ctx);
    void* ZramCtx_Stats_Set(void* ctx, const ZramStats* stats);
    void* ZramCtx_Stats_Get(void* ctx);
    void* ZramCtx_SetSims(void* ctx, const ZramSims* sims);
    void* ZramCtx_GetSims(void* ctx);
    void* ZramCtx_InitPool(void* ctx, uint64_t pool_limit);
    void* ZramCtx_FreePool(void* ctx);
    void* ZramCtx_CompressSmart(void* ctx, const void* in, void* out, uint64_t* out_len);
    void* ZramCtx_CompressSmartFallback(void* ctx, const void* in, void* out, uint64_t* out_len);
    void* ZramCtx_DecompressSmart(void* ctx, const void* in, void* out, uint64_t* out_len);
    void* ZramCtx_DecompressSmartFallback(void* ctx, const void* in, void* out, uint64_t* out_len);
}

#endif /* ZRAM_CORE_H */
