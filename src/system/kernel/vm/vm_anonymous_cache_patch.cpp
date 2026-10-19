/*
 * vm_anonymous_cache_patch.cpp - VMAnonymousCache integration
 *
 * This module patches the VMAnonymousCache class to support zram-backed
 * anonymous memory. It allows pages to be backed by zram slots instead of
 * direct physical memory.
 */

#include <haiku/Haiju.h>
#include <haiku/VMAnonymousCache.h>
#include <haiku/Process.h>
#include <haiku/Task.h>
#include "zram_core.h"

/*
 * VMAnonymousCache - the anonymous memory class
 *
 * This class manages anonymous memory pages. When a page is requested,
 * the VMAnonymousCache will allocate memory for it. If zram is available,
 * the page can be backed by a zram slot instead of physical memory.
 */

class VMAnonymousCache : public Haiku::VMAnonymousCache {
    friend class VMAnonymousCache;

    /*
     * Configuration for zram integration
     */
    struct {
        bool zram_enabled = true;
        ZramConfig zram_config;
        ZramCtx* zram_ctx = nullptr;
        ZramStats zram_stats;
    } zram;

    /*
     * Internal state
     */
    bool zram_initialized = false;
    bool zram_use_page = false;

    /*
     * Private helper functions
     */
    static bool ZramOps_Init(void* zram_ctx);
    static bool ZramOps_Free(void* zram_ctx);
    static void ZramOps_Ready(void* zram_ctx);
    static void ZramOps_Teardown(void* zram_ctx);
    static void ZramOps_ReadyPage(void* zram_ctx, void* page);
    static void ZramOps_TeardownPage(void* zram_ctx, void* page);
};

/*
 * Initialize zram for the VMAnonymousCache
 */
bool VMAnonymousCache::ZramOps_Init(void* zram_ctx) {
    if (!zram_ctx) {
        return false;
    }

    zram_ctx = (ZramCtx*)zram_ctx;
    zram_ctx->page_size = ZRAM_PAGE_SIZE;
    zram_ctx->pool_size = zram_config.pool_limit;
    zram_ctx->compressor = &zram_ctx->lz4_compress;
    zram_ctx->decompressor = &zram_ctx->lz4_decompress;

    zram_ctx->slots = zram_ctx->page_size / ZRAM_PAGE_SIZE;
    zram_ctx->state = ZRAM_EMPTY;

    return true;
}

/*
 * Free zram resources
 */
bool VMAnonymousCache::ZramOps_Free(void* zram_ctx) {
    if (!zram_ctx) {
        return false;
    }

    if (zram_ctx->zram_ctx) {
        // Free any allocated pages
        for (int i = 0; i < zram_ctx->slots; i++) {
            if (zram_ctx->zram_ctx->slots[i]) {
                free(zram_ctx->zram_ctx->slots[i]);
            }
        }
        zram_ctx->zram_ctx = nullptr;
    }

    return true;
}

/*
 * Prepare zram for use
 */
void VMAnonymousCache::ZramOps_Ready(void* zram_ctx) {
    if (!zram_ctx) {
        return;
    }

    if (!zram_ctx->zram_ctx) {
        zram_ctx->zram_ctx = (ZramCtx*)malloc(ZRAM_CTX_SIZE);
        if (!zram_ctx->zram_ctx) {
            return;
        }
    }

    zram_ctx->state = ZRAM_EMPTY;
}

/*
 * Teardown zram
 */
void VMAnonymousCache::ZramOps_Teardown(void* zram_ctx) {
    if (!zram_ctx) {
        return;
    }

    ZramOps_Free(zram_ctx);
}

/*
 * Prepare a page for zram backing
 */
void VMAnonymousCache::ZramOps_ReadyPage(void* zram_ctx, void* page) {
    if (!zram_ctx || !page) {
        return;
    }

    // Allocate a slot for this page
    ZramSlot* slot = &zram_ctx->zram_ctx->slots[0];
    slot->data = page;
    slot->size = ZRAM_PAGE_SIZE;
    slot->state = ZRAM_RAW;
    slot->cls = ZRAM_CLASS_RAW;
}

/*
 * Teardown a page from zram
 */
void VMAnonymousCache::ZramOps_TeardownPage(void* zram_ctx, void* page) {
    if (!zram_ctx || !page) {
        return;
    }

    // Free the slot
    free(page);
}
