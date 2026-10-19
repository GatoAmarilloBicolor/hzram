/*
 * zram_kernel.h - Kernel module API for zram
 *
 * This header defines the interface between the zram kernel module
 * and the rest of the Haiku kernel. It uses platform operations
 * to abstract the host's memory/locking primitives.
 */

#ifndef ZRAM_KERNEL_H
#define ZRAM_KERNEL_H

#include <haiku/Haiju.h>
#include <haiku/VMAnonymousCache.h>
#include <haiku/Task.h>
#include <haiku/Process.h>
#include <haiku/Process.h>
#include <haiku/Thread.h>
#include <haiku/Task.h>
#include <haiku/Thread.h>
#include <haiku/System.h>
#include <haiku/IO.h>
#include <haiku/IO.h>
#include <haiku/IO.h>
#include <haiku/IO.h>
#include <haiku/IO.h>

/*
 * Kernel module exports
 */
extern "C" {
    void* ZramKernel_Init(void* ctx);
    void* ZramKernel_Free(void* ctx);
    void* ZramKernel_Ready(void* ctx, void* page);
    void* ZramKernel_TeardownPage(void* ctx, void* page);
    void* ZramKernel_Compress(void* ctx, const void* in, void* out, uint64_t* out_len);
    void* ZramKernel_Decompress(void* ctx, const void* in, void* out, uint64_t* out_len);
    void* ZramKernel_Stats(void* ctx);
    void* ZramKernel_Stats_Set(void* ctx, const ZramStats* stats);
    void* ZramKernel_SetSims(void* ctx, const ZramSims* sims);
    void* ZramKernel_GetSims(void* ctx);
    void* ZramKernel_InitPool(void* ctx, uint64_t pool_limit);
    void* ZramKernel_FreePool(void* ctx);
    void* ZramKernel_CompressSmart(void* ctx, const void* in, void* out, uint64_t* out_len);
    void* ZramKernel_CompressSmartFallback(void* ctx, const void* in, void* out, uint64_t* out_len);
    void* ZramKernel_DecompressSmart(void* ctx, const void* in, void* out, uint64_t* out_len);
    void* ZramKernel_DecompressSmartFallback(void* ctx, const void* in, void* out, uint64_t* out_len);
}

/*
 * Platform operations
 */
extern "C" {
    void* ZramPlatform_Ops_Init(void* ctx);
    void* ZramPlatform_Ops_Free(void* ctx);
    void* ZramPlatform_Ready(void* ctx, void* page);
    void* ZramPlatform_TeardownPage(void* ctx, void* page);
    void* ZramPlatform_Compress(void* ctx, const void* in, void* out, uint64_t* out_len);
    void* ZramPlatform_Decompress(void* ctx, const void* in, void* out, uint64_t* out_len);
}

#endif /* ZRAM_KERNEL_H */
