/*
 * zram_core.h - Standalone zram core (no kernel dependencies)
 *
 * This is the standalone core implementation of zram. It has NO dependencies
 * on the Haiku kernel. It can be compiled and tested as a standalone library
 * on any host system with g++.
 *
 * The kernel module (zram_kernel.cpp) uses this via a platform_ops abstraction.
 */

#ifndef ZRAM_CORE_H
#define ZRAM_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Constants
 */
#define ZRAM_PAGE_SIZE      4096u
#define ZRAM_SPAN_BYTES     (64u * 1024u)
#define ZRAM_CHUNK_BYTES    (1024u * 1024u)       // 16 spans per chunk
#define ZRAM_STRIPES        64u
#define ZRAM_MAX_COMPRESSED 3072u
#define ZRAM_CLASS_COUNT    10u

static const uint32_t kZramClassSize[ZRAM_CLASS_COUNT] =
    { 256, 512, 768, 1024, 1280, 1536, 2048, 2560, 3072, 4096 };
#define ZRAM_CLASS_RAW 9u

enum ZramState : uint8_t {
    ZRAM_EMPTY = 0,
    ZRAM_SAME = 1,
    ZRAM_RAW = 2,
    ZRAM_LZ4 = 3,
};

/*
 * ZramSlot - exactly 16 bytes (static_asserted)
 *
 * Layout:
 *   [0:8]   data (void*)     -> RAW or LZ4: pointer to object
 *          pattern (uint64_t) -> SAME: repeated pattern word
 *   [8:12]  size (uint32_t)  -> bytes compressed (LZ4); 4096 (RAW); 0 otherwise
 *   [12:13] state (uint8_t)  -> ZramState
 *   [13:14] cls (uint8_t)    -> class index (RAW or LZ4 class)
 *   [14:16] reserved (uint16_t)
 */
struct ZramSlot {
    union {
        void*    data;       /* RAW or LZ4: pointer to object */
        uint64_t pattern;    /* SAME: repeated pattern word */
    };
    uint32_t size;           /* bytes compressed (LZ4); 4096 (RAW); 0 in other cases */
    uint8_t  state;          /* ZramState */
    uint8_t  cls;            /* class index */
    uint16_t reserved;
};

/*
 * ZramConfig - configuration passed at creation time
 */
struct ZramConfig {
    uint64_t disk_bytes;     /* nominal size; slots = disk_bytes / 4096 */
    uint64_t pool_limit;     /* max physical memory of the pool (multiple of CHUNK) */
};

/*
 * ZramStats - runtime statistics
 */
struct ZramStats {
    uint64_t pages_same;
    uint64_t pages_raw;
    uint64_t pages_lz4;
    uint64_t orig_bytes;      /* bytes uncompressed represented */
    uint64_t stored_bytes;    /* bytes of objects currently in use (sum of classes) */
    uint64_t pool_bytes;      /* physical memory taken from platform */
    uint64_t writes;
    uint64_t reads;
    uint64_t frees;
    uint64_t write_nomem;     /* writes that failed due to no memory */
    uint64_t read_corrupt;    /* decompression failures */
    uint64_t read_empty;      /* reads from empty slots */
};

/*
 * ZramCtx - opaque context per device.
 *
 * Contains: LZ4 state + 4096-byte output buffer + 4096-byte input buffer.
 * The 4096-byte buffers are reserved via zram_ctx_page().
 */
struct ZramCtx;

/*
 * Platform operations - abstracts the host's memory/locking primitives.
 *
 * The kernel module (zram_kernel.cpp) implements this for the target platform.
 * The standalone core (zram_core.cpp) uses a default implementation.
 *
 * Rules:
 *   - mem_alloc: may block; only called during create/destroy.
 *   - mem_free: must free the given block.
 *   - chunk_alloc: MUST NOT block. Returns NULL if no memory available.
 *   - chunk_free: frees a chunk.
 *   - mutex_*: standard mutex ops.
 *   - sem_*: counting semaphore.
 *   - cpu_count: number of logical CPUs.
 *   - request_refill: wakes the refill thread (called by core, not by caller).
 *
 * All functions are called with the caller holding the appropriate lock
 * (see the lock ordering rules in zram_core.cpp).
 */
struct ZramPlatformOps {
    void* (*mem_alloc)(size_t);
    void  (*mem_free)(void*);
    void* (*chunk_alloc)(size_t bytes);
    void  (*chunk_free)(void*, size_t);
    void* (*mutex_new)(void);
    void  (*mutex_lock)(void*);
    void  (*mutex_unlock)(void*);
    void  (*mutex_delete)(void*);
    void* (*sem_new)(uint32_t count);
    void  (*sem_acquire)(void*);
    void  (*sem_release)(void*);
    void  (*sem_delete)(void*);
    uint32_t (*cpu_count)(void);
    void  (*request_refill)(void* user);
    void* user;
};

/*
 * Public API
 */

/* Create a zram device with the given config and platform ops.
 * Returns: 0 on success, -1 on invalid config, -2 on memory allocation failure.
 */
int zram_create(const ZramConfig* cfg, const ZramPlatformOps* ops, ZramDevice** out);

/* Destroy a zram device and release all resources.
 * Must be called when the device is no longer needed.
 */
void zram_destroy(ZramDevice* dev);

/* Get a context for writing.
 * Blocks until a context is available (waits on the semaphore).
 * Caller must call zram_ctx_put() when done.
 */
ZramCtx* zram_ctx_get(ZramDevice* dev);

/* Release a context back to the pool.
 * May block if the semaphore is empty.
 */
void zram_ctx_put(ZramDevice* dev, ZramCtx* ctx);

/* Write a page (4096 bytes) to the given slot.
 *
 * slot: index into the slot table (0..slot_count-1).
 * page: pointer to 4096 bytes of data to compress/store.
 *
 * Returns:
 *   0  -> OK, page stored in the slot.
 *  -1  -> ZRAM_E_INVAL: slot out of range or page is NULL.
 *  -2  -> ZRAM_E_NOMEM: no memory available (page remains in RAM).
 *
 * On success, the slot's state is updated and the object is stored.
 * On failure, the slot is NOT modified.
 */
int zram_write_page(ZramDevice* dev, ZramCtx* ctx, uint32_t slot, const void* page);

/* Read a page (4096 bytes) from the given slot.
 *
 * slot: index into the slot table (0..slot_count-1).
 * dst:  pointer to 4096 bytes where the page will be written.
 *
 * Returns:
 *   0  -> OK, page read successfully.
 *  -1  -> ZRAM_E_INVAL: slot out of range.
 *  -2  -> ZRAM_E_NOMEM: no memory available (for RAW slots that need reallocation).
 *  -3  -> ZRAM_E_CORRUPT: decompression failed (data corrupted).
 *
 * On EMPTY, dst is filled with zeros.
 * On SAME, dst is filled with the repeated pattern.
 * On RAW,  dst is a copy of the stored data.
 * On LZ4,  dst is the decompressed data.
 */
int zram_read_page(ZramDevice* dev, uint32_t slot, void* dst);

/* Free a range of slots [first, first + count).
 *
 * For each slot in the range:
 *   - Sets state to EMPTY.
 *   - Frees the object if the slot was not EMPTY.
 *
 * Slots outside the range are ignored.
 * Calling with count == 0 is a no-op.
 *
 * This is idempotent: calling it twice on the same range is safe.
 */
void zram_free_slots(ZramDevice* dev, uint32_t first, uint32_t count);

/* Check if the device can accept new writes.
 *
 * Returns true if there is at least one free span in the pool,
 * or if pool_bytes + ZRAM_CHUNK_BYTES <= pool_limit.
 *
 * This is a non-blocking check (uses atomic reads).
 */
bool zram_can_accept(ZramDevice* dev);

/* Request the refill thread to run.
 *
 * The refill thread will run zram_refill() in a loop.
 * This should be called when the pool is getting low on free spans.
 */
void zram_refill(ZramDevice* dev);

/* Get current statistics.
 *
 * Must be called with the device lock held (or from a context where
 * the lock is already held).
 */
void zram_get_stats(ZramDevice* dev, ZramStats* out);

/* Check internal invariants.
 *
 * For testing purposes only. Checks:
 *   - slot table consistency
 *   - class allocator invariants
 *   - no overlapping objects
 *   - slot count matches expected
 *
 * Returns true if all invariants hold.
 */
bool zram_check_invariants(ZramDevice* dev);

#endif /* ZRAM_CORE_H */