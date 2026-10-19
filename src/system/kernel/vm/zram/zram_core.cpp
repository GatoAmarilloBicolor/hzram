/*
 * zram_core.cpp - Standalone zram core implementation
 *
 * This file contains the full implementation of the zram core.
 * It has NO dependencies on the Haiku kernel.
 *
 * The kernel module (zram_kernel.cpp) provides a ZramPlatformOps
 * abstraction that maps these functions to kernel primitives.
 */

#include "zram_core.h"
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/time.h>

/* LZ4 include - must come after zram_core.h */
#include "lz4.h"

/*
 * ============================================================================
 * CLASS ALLOCATOR
 * ============================================================================
 *
 * We use a class-based allocator where each class has a fixed object size.
 * Objects are allocated from "spans" of 64 KiB.
 * Each chunk is 1 MiB and contains 16 spans.
 *
 * Classes:
 *   0: 256  bytes
 *   1: 512  bytes
 *   ...
 *   9: 4096 bytes (RAW class)
 *
 * Locking:
 *   - Each class has its own mutex (mutex_class[c]).
 *   - A separate "reserver" mutex protects the free span pool.
 *
 * Algorithm:
 *   - When a class needs an object and its free list is empty:
 *       1. Take a span from the reserver (acquire reserver lock).
 *       2. If no span available, return NULL.
 *       3. Carve the span into objects of the requested class size.
 *       4. Push all new objects onto the class's free list.
 *   - Otherwise, pop from the free list.
 *
 * The free list is an intrusive linked list. The first 8 bytes of a free
 * object point to the next free object. This avoids needing a separate list
 * of free objects.
 */

struct ZramClass {
    uint32_t size;       /* object size in bytes */
    uint32_t count;      /* number of free objects */
    uint32_t total;      /* total objects ever allocated */
    struct ZramClass* next;
};

struct ZramReserver {
    uint32_t count;      /* number of free spans */
    struct {
        void*    span;
        uint32_t  next;   /* index of next free span in the array */
    } spans[1];          /* variable length */
    uint32_t next_free;  /* index of next free span */
};

/*
 * Span: a 64 KiB block of memory carved into objects.
 * Layout:
 *   [0:8]  next_free: index of next free object in the class's free list
 *   [8:16] free_list: linked list of free objects (next pointers)
 *   [16:]  objects of the class size
 */
struct ZramSpan {
    uint32_t next_free;
    uint32_t free_list;
    uint8_t  objects[0];  /* flexible array member */
};

/*
 * Class allocator state.
 *
 * We have one ZramAllocator per class. Each allocator manages its own
 * free list. The reserver is shared across all classes.
 */
struct ZramAllocator {
    struct ZramClass class;
    struct ZramReserver reserver;
};

static struct ZramAllocator g_allocators[ZRAM_CLASS_COUNT];
static bool g_allocators_init = false;
static int g_allocators_inited = 0;

/*
 * Initialize the class allocator.
 *
 * Called once at startup. Creates the reserver and initializes all classes.
 */
static void zram_allocator_init(void)
{
    if (g_allocators_inited)
        return;

    /* Create the reserver with an initial span. */
    g_allocators[0].reserver.count = 1;
    g_allocators[0].reserver.next_free = 0;

    /* Initialize all classes. */
    for (uint32_t c = 0; c < ZRAM_CLASS_COUNT; c++) {
        g_allocators[c].class.size = kZramClassSize[c];
        g_allocators[c].class.count = 0;
        g_allocators[c].class.total = 0;
        g_allocators[c].class.next = NULL;
    }

    g_allocators_inited = 1;
    g_allocators_init = true;
}

/*
 * Carve a span into objects of the given class size.
 *
 * Returns the number of objects carved, or -1 on failure.
 */
static int zram_carve_span(void* span, uint32_t class_idx)
{
    uint32_t class_size = kZramClassSize[class_idx];
    uint32_t span_size = ZRAM_SPAN_BYTES;
    uint32_t offset = 0;
    uint32_t count = 0;

    /* Check that the class size divides the span size. */
    if (span_size % class_size != 0) {
        return -1;
    }

    /* Carve objects into the span.
     * Each object layout:
 *   [0:8]  next_free (pointer to next free object)
 *   [8:16] free_list (pointer to next object in the linked list)
 *   [16:]  data
     */
    while (offset + class_size <= span_size) {
        uint8_t* obj = (uint8_t*)span + offset;
        ((uint32_t*)obj)[0] = 0;      /* next_free = NULL */
        ((uint32_t*)obj)[1] = 0;      /* free_list = NULL */
        /* data is uninitialized (caller must fill it) */
        count++;
        offset += class_size;
    }

    /* Push all carved objects onto the class's free list. */
    if (count > 0) {
        g_allocators[class_idx].class.count += count;
        g_allocators[class_idx].class.total += count;

        /* Build the intrusive linked list. */
        struct ZramClass* head = &g_allocators[class_idx].class;
        struct ZramClass* tail = head;

        for (uint32_t i = 0; i < count; i++) {
            uint8_t* obj = (uint8_t*)span + offset + i * class_size;
            /* next pointer: first 8 bytes of the object */
            ((uint32_t*)obj)[0] = (uint32_t)tail;
            ((uint32_t*)obj)[1] = 0;  /* free_list = NULL */
            tail = (struct ZramClass*)obj;
        }

        /* Append to the class's free list. */
        tail->next = head->next;
        head->next = tail;
    }

    return count;
}

/*
 * Allocate an object of the given class.
 *
 * Caller must hold: mutex_class[class].
 *
 * Returns NULL if no memory is available.
 */
static void* zram_class_alloc(uint32_t class_idx)
{
    if (!g_allocators_init) {
        zram_allocator_init();
    }

    /* Lock the class mutex. */
    if (g_allocators[class_idx].class.next == NULL) {
        /* Need to carve a new span. */
        /* Acquire the reserver lock. */
        if (g_allocators[0].reserver.count <= 0) {
            return NULL;  /* no spans available */
        }

        /* Take a span from the reserver. */
        uint32_t idx = g_allocators[0].reserver.next_free;
        g_allocators[0].reserver.next_free =
            g_allocators[0].reserver.spans[idx].next;
        g_allocators[0].reserver.count--;

        /* Carve the span. */
        int carved = zram_carve_span(g_allocators[0].reserver.spans[idx].span, class_idx);
        if (carved <= 0) {
            return NULL;
        }

        /* Push the new head onto the class's free list. */
        g_allocators[class_idx].class.next =
            (struct ZramClass*)((uint8_t*)g_allocators[0].reserver.spans[idx].span + kZramClassSize[class_idx]);
        g_allocators[class_idx].class.count++;
    }

    /* Pop from the free list. */
    struct ZramClass* obj = g_allocators[class_idx].class.next;
    g_allocators[class_idx].class.next = ((struct ZramClass*)obj)->next;
    g_allocators[class_idx].class.count--;

    return obj;
}

/*
 * Free an object of the given class.
 *
 * Caller must hold: mutex_class[class].
 */
static void zram_class_free(uint32_t class_idx, void* obj)
{
    if (g_allocators[class_idx].class.next == NULL) {
        /* Need to carve a new span. */
        if (g_allocators[0].reserver.count <= 0) {
            return;  /* no spans available */
        }

        /* Take a span from the reserver. */
        uint32_t idx = g_allocators[0].reserver.next_free;
        g_allocators[0].reserver.next_free =
            g_allocators[0].reserver.spans[idx].next;
        g_allocators[0].reserver.count--;

        /* Carve the span. */
        int carved = zram_carve_span(g_allocators[0].reserver.spans[idx].span, class_idx);
        if (carved <= 0) {
            return;
        }

        /* Push the new head onto the class's free list. */
        g_allocators[class_idx].class.next =
            (struct ZramClass*)((uint8_t*)g_allocators[0].reserver.spans[idx].span + kZramClassSize[class_idx]);
        g_allocators[class_idx].class.count++;
    }

    /* Push onto the free list. */
    ((struct ZramClass*)obj)->next = g_allocators[class_idx].class.next;
    g_allocators[class_idx].class.next = (struct ZramClass*)obj;
    g_allocators[class_idx].class.count++;
}

/*
 * Request a new span from the platform.
 *
 * If the reserver has fewer than 2 free spans, request a new chunk.
 * The chunk is then carved into spans and added to the reserver.
 */
static void zram_request_refill(void* user)
{
    ZramDevice* dev = (ZramDevice*)user;

    /* Acquire the reserver lock. */
    if (g_allocators[0].reserver.count < 2) {
        /* Try to allocate a new chunk. */
        void* chunk = dev->ops.chunk_alloc(ZRAM_CHUNK_BYTES);
        if (chunk != NULL) {
            /* Carve the chunk into 16 spans of 64 KiB. */
            for (uint32_t i = 0; i < 16; i++) {
                g_allocators[0].reserver.spans[g_allocators[0].reserver.count].span =
                    (uint8_t*)chunk + i * ZRAM_SPAN_BYTES;
                g_allocators[0].reserver.spans[g_allocators[0].reserver.count].next = 0;
                g_allocators[0].reserver.count++;
            }
        }
    }
}

/*
 * ============================================================================
 * SLOT TABLE
 * ============================================================================
 *
 * The slot table is a flat array of ZramSlot structures.
 * Each slot is 16 bytes.
 *
 * The table is pre-allocated at creation time.
 * We use a simple array (no dynamic allocation of individual slots).
 */

struct ZramDevice {
    ZramPlatformOps* ops;
    uint32_t slot_count;
    ZramSlot* slots;
    ZramCtx** ctxs;
    uint32_t ctx_count;
    uint32_t ctx_capacity;
    uint64_t disk_bytes;
    uint64_t pool_limit;
    uint64_t pool_bytes;
    uint64_t pages_same;
    uint64_t pages_raw;
    uint64_t pages_lz4;
    uint64_t orig_bytes;
    uint64_t stored_bytes;
    uint64_t pool_bytes_actual;
    uint64_t writes;
    uint64_t reads;
    uint64_t frees;
    uint64_t write_nomem;
    uint64_t read_corrupt;
    uint64_t read_empty;
    pthread_mutex_t stripe_locks[ZRAM_STRIPES];
    pthread_mutex_t class_mutexes[ZRAM_CLASS_COUNT];
    pthread_mutex_t reserver_lock;
    pthread_mutex_t ctx_mutex;
    pthread_mutex_t reserve_mutex;
    pthread_mutex_t stats_mutex;
    pthread_mutex_t destroy_mutex;
    pthread_t refill_thread;
    sem_t* refill_sem;
    volatile int refill_thread_done;
    volatile int running;
    bool initialized;
    bool destroyed;
    void* user_data;
};

/*
 * ============================================================================
 * CONTEXT
 * ============================================================================
 *
 * Each device has a pool of contexts. Each context can be used by one
 * thread at a time for compression.
 *
 * A context contains:
 *   - LZ4 state (LZ4_stream_t)
 *   - 4096-byte output buffer
 *   - 4096-byte input buffer
 */

struct ZramCtx {
    LZ4_stream_t state;
    uint8_t out_buf[4096];
    uint8_t in_buf[4096];
};

/*
 * ============================================================================
 * PUBLIC API
 * ============================================================================
 */

int zram_create(const ZramConfig* cfg, const ZramPlatformOps* ops, ZramDevice** out)
{
    if (cfg == NULL || out == NULL) {
        return -1;
    }

    if (cfg->disk_bytes == 0 || cfg->pool_limit == 0) {
        return -1;
    }

    uint32_t slot_count = (uint32_t)(cfg->disk_bytes / ZRAM_PAGE_SIZE);
    if (slot_count == 0) {
        return -1;
    }

    /* Check that pool_limit is a multiple of chunk size. */
    if (cfg->pool_limit % ZRAM_CHUNK_BYTES != 0) {
        return -1;
    }

    /* Allocate the device. */
    ZramDevice* dev = (ZramDevice*)ops->mem_alloc(sizeof(ZramDevice));
    if (dev == NULL) {
        return -2;
    }

    /* Allocate the slot table. */
    dev->slots = (ZramSlot*)ops->mem_alloc(slot_count * sizeof(ZramSlot));
    if (dev->slots == NULL) {
        ops->mem_free(dev);
        return -2;
    }

    /* Allocate the context array. */
    uint32_t ctx_count = ops->cpu_count() > 8 ? 8 : ops->cpu_count();
    dev->ctxs = (ZramCtx**)ops->mem_alloc(ctx_count * sizeof(ZramCtx*));
    if (dev->ctxs == NULL) {
        ops->mem_free(dev->slots);
        ops->mem_free(dev);
        return -2;
    }

    /* Initialize the device. */
    dev->ops = ops;
    dev->slot_count = slot_count;
    dev->disk_bytes = cfg->disk_bytes;
    dev->pool_limit = cfg->pool_limit;
    dev->pool_bytes = 0;
    dev->pages_same = 0;
    dev->pages_raw = 0;
    dev->pages_lz4 = 0;
    dev->orig_bytes = 0;
    dev->stored_bytes = 0;
    dev->pool_bytes_actual = 0;
    dev->writes = 0;
    dev->reads = 0;
    dev->frees = 0;
    dev->write_nomem = 0;
    dev->read_corrupt = 0;
    dev->read_empty = 0;
    dev->ctx_count = 0;
    dev->ctx_capacity = ctx_count;
    dev->initialized = false;
    dev->destroyed = false;
    dev->running = 0;
    dev->refill_thread_done = 0;
    dev->user_data = NULL;

    /* Initialize all mutexes. */
    for (uint32_t i = 0; i < ZRAM_STRIPES; i++) {
        dev->stripe_locks[i] = ops->mutex_new();
        if (dev->stripe_locks[i] == NULL) {
            goto fail;
        }
    }

    for (uint32_t i = 0; i < ZRAM_CLASS_COUNT; i++) {
        dev->class_mutexes[i] = ops->mutex_new();
        if (dev->class_mutexes[i] == NULL) {
            goto fail;
        }
    }

    dev->reserver_lock = ops->mutex_new();
    if (dev->reserver_lock == NULL) {
        goto fail;
    }

    dev->ctx_mutex = ops->mutex_new();
    if (dev->ctx_mutex == NULL) {
        goto fail;
    }

    dev->reserve_mutex = ops->mutex_new();
    if (dev->reserve_mutex == NULL) {
        goto fail;
    }

    dev->stats_mutex = ops->mutex_new();
    if (dev->stats_mutex == NULL) {
        goto fail;
    }

    dev->destroy_mutex = ops->mutex_new();
    if (dev->destroy_mutex == NULL) {
        goto fail;
    }

    /* Initialize all slots to EMPTY. */
    for (uint32_t i = 0; i < slot_count; i++) {
        dev->slots[i].data = NULL;
        dev->slots[i].size = 0;
        dev->slots[i].state = ZRAM_EMPTY;
        dev->slots[i].cls = 0;
        dev->slots[i].reserved = 0;
    }

    /* Initialize class allocators. */
    zram_allocator_init();

    /* Initialize the reserver. */
    uint32_t num_spans = (uint32_t)(dev->pool_limit / ZRAM_SPAN_BYTES);
    for (uint32_t i = 0; i < num_spans; i++) {
        void* span = ops->mem_alloc(ZRAM_SPAN_BYTES);
        if (span == NULL) {
            goto fail;
        }
        g_allocators[0].reserver.spans[i].span = span;
        g_allocators[0].reserver.spans[i].next = i + 1;
        g_allocators[0].reserver.count++;
    }

    /* Initialize the refill semaphore. */
    dev->refill_sem = ops->sem_new(0);
    if (dev->refill_sem == NULL) {
        goto fail;
    }

    /* Initialize contexts. */
    for (uint32_t i = 0; i < ctx_count; i++) {
        dev->ctxs[i] = (ZramCtx*)ops->mem_alloc(sizeof(ZramCtx));
        if (dev->ctxs[i] == NULL) {
            goto fail;
        }
        dev->ctxs[i] = NULL;  /* not available yet */
    }

    /* Start the refill thread. */
    dev->running = 1;
    dev->refill_thread_done = 0;

    /* Create the refill thread. */
    int rc = pthread_create(&dev->refill_thread, NULL,
        (void*(*)(void*))zram_refill_thread, dev);
    if (rc != 0) {
        goto fail;
    }

    /* Initialize the device as ready. */
    dev->initialized = true;

    *out = dev;
    return 0;

fail:
    zram_destroy(dev);
    return -2;
}

static void* zram_refill_thread(void* arg)
{
    ZramDevice* dev = (ZramDevice*)arg;

    while (dev->running && !dev->refill_thread_done) {
        /* Wait for a refill request. */
        sem_wait(dev->refill_sem);

        /* Check if we should refill. */
        uint64_t pool_bytes = 0;
        {
            pthread_mutex_lock(&dev->reserve_mutex);
            pool_bytes = dev->pool_bytes;
            pthread_mutex_unlock(&dev->reserve_mutex);
        }

        if (pool_bytes + ZRAM_CHUNK_BYTES <= dev->pool_limit) {
            /* Request a new chunk. */
            dev->ops->request_refill(dev->user_data);
        }
    }

    return NULL;
}

static void zram_refill_thread_done(ZramDevice* dev)
{
    dev->refill_thread_done = 1;
    sem_post(dev->refill_sem);

    /* Wait for the thread to exit. */
    pthread_join(dev->refill_thread, NULL);
}

void zram_refill(ZramDevice* dev)
{
    /* Wake up the refill thread. */
    dev->ops->request_refill(dev->user_data);
}

static void* zram_refill_worker(void* arg)
{
    ZramDevice* dev = (ZramDevice*)arg;

    while (dev->running) {
        /* Wait for a refill request. */
        sem_wait(dev->refill_sem);

        /* Check if we should refill. */
        uint64_t pool_bytes = 0;
        {
            pthread_mutex_lock(&dev->reserve_mutex);
            pool_bytes = dev->pool_bytes;
            pthread_mutex_unlock(&dev->reserve_mutex);
        }

        if (pool_bytes + ZRAM_CHUNK_BYTES <= dev->pool_limit) {
            /* Request a new chunk. */
            dev->ops->request_refill(dev->user_data);
        } else {
            /* No more room. Signal that we're done. */
            dev->refill_thread_done = 1;
            sem_post(dev->refill_sem);
            break;
        }
    }

    return NULL;
}

void zram_refill_worker_loop(ZramDevice* dev)
{
    pthread_t thread;
    pthread_create(&thread, NULL, zram_refill_worker, dev);

    /* Wait for the thread to finish. */
    pthread_join(thread, NULL);
}

static void zram_refill_inner(ZramDevice* dev)
{
    /* Acquire the reserver lock. */
    pthread_mutex_lock(&dev->reserver_lock);

    /* Check if we should refill. */
    uint64_t pool_bytes = dev->pool_bytes;
    uint64_t pool_limit = dev->pool_limit;

    if (pool_bytes + ZRAM_CHUNK_BYTES <= pool_limit) {
        /* Request a new chunk. */
        dev->ops->request_refill(dev->user_data);
    }

    pthread_mutex_unlock(&dev->reserver_lock);
}

/*
 * Create a new zram device.
 *
 * This is the public API. It allocates the device, initializes all
 * internal structures, and starts the refill thread.
 *
 * The caller is responsible for calling zram_destroy() when done.
 */
int zram_create(const ZramConfig* cfg, const ZramPlatformOps* ops, ZramDevice** out)
{
    if (cfg == NULL || out == NULL) {
        return -1;
    }

    if (cfg->disk_bytes == 0 || cfg->pool_limit == 0) {
        return -1;
    }

    uint32_t slot_count = (uint32_t)(cfg->disk_bytes / ZRAM_PAGE_SIZE);
    if (slot_count == 0) {
        return -1;
    }

    /* Check that pool_limit is a multiple of chunk size. */
    if (cfg->pool_limit % ZRAM_CHUNK_BYTES != 0) {
        return -1;
    }

    /* Allocate the device. */
    ZramDevice* dev = (ZramDevice*)ops->mem_alloc(sizeof(ZramDevice));
    if (dev == NULL) {
        return -2;
    }

    /* Allocate the slot table. */
    dev->slots = (ZramSlot*)ops->mem_alloc(slot_count * sizeof(ZramSlot));
    if (dev->slots == NULL) {
        ops->mem_free(dev);
        return -2;
    }

    /* Allocate the context array. */
    uint32_t ctx_count = ops->cpu_count() > 8 ? 8 : ops->cpu_count();
    dev->ctxs = (ZramCtx**)ops->mem_alloc(ctx_count * sizeof(ZramCtx*));
    if (dev->ctxs == NULL) {
        ops->mem_free(dev->slots);
        ops->mem_free(dev);
        return -2;
    }

    /* Initialize the device. */
    dev->ops = ops;
    dev->slot_count = slot_count;
    dev->disk_bytes = cfg->disk_bytes;
    dev->pool_limit = cfg->pool_limit;
    dev->pool_bytes = 0;
    dev->pages_same = 0;
    dev->pages_raw = 0;
    dev->pages_lz4 = 0;
    dev->orig_bytes = 0;
    dev->stored_bytes = 0;
    dev->pool_bytes_actual = 0;
    dev->writes = 0;
    dev->reads = 0;
    dev->frees = 0;
    dev->write_nomem = 0;
    dev->read_corrupt = 0;
    dev->read_empty = 0;
    dev->ctx_count = 0;
    dev->ctx_capacity = ctx_count;
    dev->initialized = false;
    dev->destroyed = false;
    dev->running = 0;
    dev->refill_thread_done = 0;
    dev->user_data = NULL;

    /* Initialize all mutexes. */
    for (uint32_t i = 0; i < ZRAM_STRIPES; i++) {
        dev->stripe_locks[i] = ops->mutex_new();
        if (dev->stripe_locks[i] == NULL) {
            goto fail;
        }
    }

    for (uint32_t i = 0; i < ZRAM_CLASS_COUNT; i++) {
        dev->class_mutexes[i] = ops->mutex_new();
        if (dev->class_mutexes[i] == NULL) {
            goto fail;
        }
    }

    dev->reserver_lock = ops->mutex_new();
    if (dev->reserver_lock == NULL) {
        goto fail;
    }

    dev->ctx_mutex = ops->mutex_new();
    if (dev->ctx_mutex == NULL) {
        goto fail;
    }

    dev->reserve_mutex = ops->mutex_new();
    if (dev->reserve_mutex == NULL) {
        goto fail;
    }

    dev->stats_mutex = ops->mutex_new();
    if (dev->stats_mutex == NULL) {
        goto fail;
    }

    dev->destroy_mutex = ops->mutex_new();
    if (dev->destroy_mutex == NULL) {
        goto fail;
    }

    /* Initialize all slots to EMPTY. */
    for (uint32_t i = 0; i < slot_count; i++) {
        dev->slots[i].data = NULL;
        dev->slots[i].size = 0;
        dev->slots[i].state = ZRAM_EMPTY;
        dev->slots[i].cls = 0;
        dev->slots[i].reserved = 0;
    }

    /* Initialize class allocators. */
    zram_allocator_init();

    /* Initialize the reserver. */
    uint32_t num_spans = (uint32_t)(dev->pool_limit / ZRAM_SPAN_BYTES);
    for (uint32_t i = 0; i < num_spans; i++) {
        void* span = ops->mem_alloc(ZRAM_SPAN_BYTES);
        if (span == NULL) {
            goto fail;
        }
        g_allocators[0].reserver.spans[i].span = span;
        g_allocators[0].reserver.spans[i].next = i + 1;
        g_allocators[0].reserver.count++;
    }

    /* Initialize the refill semaphore. */
    dev->refill_sem = ops->sem_new(0);
    if (dev->refill_sem == NULL) {
        goto fail;
    }

    /* Initialize contexts. */
    for (uint32_t i = 0; i < ctx_count; i++) {
        dev->ctxs[i] = (ZramCtx*)ops->mem_alloc(sizeof(ZramCtx));
        if (dev->ctxs[i] == NULL) {
            goto fail;
        }
        dev->ctxs[i] = NULL;  /* not available yet */
    }

    /* Start the refill thread. */
    dev->running = 1;
    dev->refill_thread_done = 0;

    /* Create the refill thread. */
    int rc = pthread_create(&dev->refill_thread, NULL,
        (void*(*)(void*))zram_refill_worker, dev);
    if (rc != 0) {
        goto fail;
    }

    /* Initialize the device as ready. */
    dev->initialized = true;

    *out = dev;
    return 0;

fail:
    zram_destroy(dev);
    return -2;
}

void zram_destroy(ZramDevice* dev)
{
    if (dev == NULL || dev->destroyed) {
        return;
    }

    /* Stop the refill thread. */
    dev->running = 0;
    dev->refill_thread_done = 1;
    sem_post(dev->refill_sem);
    pthread_join(dev->refill_thread, NULL);

    /* Destroy all contexts. */
    for (uint32_t i = 0; i < dev->ctx_count; i++) {
        if (dev->ctxs[i] != NULL) {
            ops->mem_free(dev->ctxs[i]);
            dev->ctxs[i] = NULL;
        }
    }

    /* Destroy the slot table. */
    ops->mem_free(dev->slots);

    /* Destroy the context array. */
    ops->mem_free(dev->ctxs);

    /* Destroy all mutexes. */
    for (uint32_t i = 0; i < ZRAM_STRIPES; i++) {
        ops->mutex_delete(dev->stripe_locks[i]);
    }

    for (uint32_t i = 0; i < ZRAM_CLASS_COUNT; i++) {
        ops->mutex_delete(dev->class_mutexes[i]);
    }

    ops->mutex_delete(dev->reserver_lock);
    ops->mutex_delete(dev->ctx_mutex);
    ops->mutex_delete(dev->reserve_mutex);
    ops->mutex_delete(dev->stats_mutex);
    ops->mutex_delete(dev->destroy_mutex);

    ops->mem_free(dev);
    dev->destroyed = true;
}

ZramCtx* zram_ctx_get(ZramDevice* dev)
{
    if (dev == NULL) {
        return NULL;
    }

    pthread_mutex_lock(&dev->ctx_mutex);

    /* Find an available context. */
    for (uint32_t i = 0; i < dev->ctx_count; i++) {
        if (dev->ctxs[i] == NULL) {
            dev->ctxs[i] = (ZramCtx*)ops->mem_alloc(sizeof(ZramCtx));
            if (dev->ctxs[i] == NULL) {
                /* Out of memory. */
                pthread_mutex_unlock(&dev->ctx_mutex);
                return NULL;
            }
            break;
        }
    }

    pthread_mutex_unlock(&dev->ctx_mutex);

    return dev->ctxs[0];  /* return the first available context */
}

void zram_ctx_put(ZramDevice* dev, ZramCtx* ctx)
{
    if (dev == NULL || ctx == NULL) {
        return;
    }

    pthread_mutex_lock(&dev->ctx_mutex);

    /* Find a slot in the context array. */
    for (uint32_t i = 0; i < dev->ctx_count; i++) {
        if (dev->ctxs[i] == ctx) {
            dev->ctxs[i] = NULL;
            break;
        }
    }

    pthread_mutex_unlock(&dev->ctx_mutex);
}

int zram_write_page(ZramDevice* dev, ZramCtx* ctx, uint32_t slot, const void* page)
{
    if (dev == NULL || ctx == NULL || slot >= dev->slot_count || page == NULL) {
        return -1;
    }

    /* Check if the slot is already empty. */
    if (dev->slots[slot].state == ZRAM_EMPTY) {
        dev->pages_same++;
        return 0;
    }

    /* Check for same-page detection. */
    uint64_t* words = (uint64_t*)page;
    uint64_t first_word = words[0];
    bool all_same = true;
    for (uint32_t i = 1; i < 512; i++) {
        if (words[i] != first_word) {
            all_same = false;
            break;
        }
    }

    if (all_same) {
        dev->slots[slot].pattern = first_word;
        dev->slots[slot].state = ZRAM_SAME;
        dev->slots[slot].size = 0;
        dev->slots[slot].cls = 0;
        dev->pages_same++;
        return 0;
    }

    /* Try to compress with LZ4. */
    LZ4_stream_t* state = &ctx->state;
    uint8_t* out_buf = ctx->out_buf;
    const uint8_t* in_buf = (const uint8_t*)page;

    int compressed_size = LZ4_compress_fast_extState(
        state, in_buf, out_buf, ZRAM_PAGE_SIZE, ZRAM_MAX_COMPRESSED, 1);

    if (compressed_size > 0) {
        /* Compressed successfully. */
        uint32_t cls = 0;
        for (uint32_t c = 0; c < ZRAM_CLASS_COUNT; c++) {
            if (kZramClassSize[c] >= compressed_size) {
                cls = c;
                break;
            }
        }

        /* Allocate an object of the appropriate class. */
        void* obj = zram_class_alloc(cls);
        if (obj == NULL) {
            dev->write_nomem++;
            return -2;
        }

        /* Copy the compressed data into the object. */
        memcpy(obj, out_buf, compressed_size);

        dev->slots[slot].data = obj;
        dev->slots[slot].size = compressed_size;
        dev->slots[slot].state = ZRAM_LZ4;
        dev->slots[slot].cls = cls;
        dev->pages_lz4++;
        dev->orig_bytes += ZRAM_PAGE_SIZE;
        dev->stored_bytes += compressed_size;
        dev->writes++;

        /* Update pool bytes. */
        {
            pthread_mutex_lock(&dev->reserve_mutex);
            dev->pool_bytes += compressed_size;
            pthread_mutex_unlock(&dev->reserve_mutex);
        }

        return 0;
    } else {
        /* Compression failed. Store as RAW. */
        void* obj = zram_class_alloc(ZRAM_CLASS_RAW);
        if (obj == NULL) {
            dev->write_nomem++;
            return -2;
        }

        memcpy(obj, page, ZRAM_PAGE_SIZE);

        dev->slots[slot].data = obj;
        dev->slots[slot].size = ZRAM_PAGE_SIZE;
        dev->slots[slot].state = ZRAM_RAW;
        dev->slots[slot].cls = ZRAM_CLASS_RAW;
        dev->pages_raw++;
        dev->orig_bytes += ZRAM_PAGE_SIZE;
        dev->stored_bytes += ZRAM_PAGE_SIZE;
        dev->writes++;

        /* Update pool bytes. */
        {
            pthread_mutex_lock(&dev->reserve_mutex);
            dev->pool_bytes += ZRAM_PAGE_SIZE;
            pthread_mutex_unlock(&dev->reserve_mutex);
        }

        return 0;
    }
}

int zram_read_page(ZramDevice* dev, uint32_t slot, void* dst)
{
    if (dev == NULL || slot >= dev->slot_count || dst == NULL) {
        return -1;
    }

    /* Acquire the stripe lock. */
    uint32_t stripe = slot % ZRAM_STRIPES;
    pthread_mutex_lock(&dev->stripe_locks[stripe]);

    ZramSlot* slot_entry = &dev->slots[slot];

    if (slot_entry->state == ZRAM_EMPTY) {
        dev->read_empty++;
        memset(dst, 0, ZRAM_PAGE_SIZE);
        pthread_mutex_unlock(&dev->stripe_locks[stripe]);
        return 0;
    }

    if (slot_entry->state == ZRAM_SAME) {
        uint64_t* words = (uint64_t*)dst;
        for (uint32_t i = 0; i < 512; i++) {
            words[i] = slot_entry->pattern;
        }
        pthread_mutex_unlock(&dev->stripe_locks[stripe]);
        return 0;
    }

    if (slot_entry->state == ZRAM_RAW) {
        memcpy(dst, slot_entry->data, ZRAM_PAGE_SIZE);
        pthread_mutex_unlock(&dev->stripe_locks[stripe]);
        dev->reads++;
        return 0;
    }

    if (slot_entry->state == ZRAM_LZ4) {
        uint8_t* obj = slot_entry->data;
        uint32_t size = slot_entry->size;

        int decompressed_size = LZ4_decompress_safe(
            obj, dst, size, ZRAM_PAGE_SIZE);

        if (decompressed_size != ZRAM_PAGE_SIZE) {
            dev->read_corrupt++;
            pthread_mutex_unlock(&dev->stripe_locks[stripe]);
            return -3;
        }

        pthread_mutex_unlock(&dev->stripe_locks[stripe]);
        dev->reads++;
        return 0;
    }

    pthread_mutex_unlock(&dev->stripe_locks[stripe]);
    return -1;
}

void zram_free_slots(ZramDevice* dev, uint32_t first, uint32_t count)
{
    if (dev == NULL || first >= dev->slot_count || count == 0) {
        return;
    }

    uint32_t last = first + count;
    if (last > dev->slot_count) {
        last = dev->slot_count;
    }

    for (uint32_t i = first; i < last; i++) {
        /* Acquire the stripe lock. */
        uint32_t stripe = i % ZRAM_STRIPES;
        pthread_mutex_lock(&dev->stripe_locks[stripe]);

        ZramSlot* slot_entry = &dev->slots[i];

        /* If the slot is already empty, skip it. */
        if (slot_entry->state == ZRAM_EMPTY) {
            pthread_mutex_unlock(&dev->stripe_locks[stripe]);
            continue;
        }

        /* Release the object if it was not empty. */
        if (slot_entry->state != ZRAM_EMPTY) {
            if (slot_entry->state == ZRAM_RAW || slot_entry->state == ZRAM_LZ4) {
                zram_class_free(slot_entry->cls, slot_entry->data);
            }
        }

        /* Mark the slot as empty. */
        slot_entry->state = ZRAM_EMPTY;
        slot_entry->size = 0;
        slot_entry->cls = 0;

        dev->frees++;

        pthread_mutex_unlock(&dev->stripe_locks[stripe]);
    }
}

bool zram_can_accept(ZramDevice* dev)
{
    if (dev == NULL) {
        return false;
    }

    uint64_t pool_bytes = 0;
    {
        pthread_mutex_lock(&dev->reserve_mutex);
        pool_bytes = dev->pool_bytes;
        pthread_mutex_unlock(&dev->reserve_mutex);
    }

    if (pool_bytes < dev->pool_limit) {
        return true;
    }

    /* Check if there are free spans in the reserver. */
    pthread_mutex_lock(&dev->reserver_lock);
    bool can = g_allocators[0].reserver.count > 0;
    pthread_mutex_unlock(&dev->reserver_lock);

    return can;
}

void zram_get_stats(ZramDevice* dev, ZramStats* out)
{
    if (dev == NULL || out == NULL) {
        return;
    }

    pthread_mutex_lock(&dev->stats_mutex);

    out->pages_same = dev->pages_same;
    out->pages_raw = dev->pages_raw;
    out->pages_lz4 = dev->pages_lz4;
    out->orig_bytes = dev->orig_bytes;
    out->stored_bytes = dev->stored_bytes;
    out->pool_bytes = dev->pool_bytes;
    out->writes = dev->writes;
    out->reads = dev->reads;
    out->frees = dev->frees;
    out->write_nomem = dev->write_nomem;
    out->read_corrupt = dev->read_corrupt;
    out->read_empty = dev->read_empty;

    pthread_mutex_unlock(&dev->stats_mutex);
}

bool zram_check_invariants(ZramDevice* dev)
{
    if (dev == NULL) {
        return false;
    }

    /* Check slot count. */
    if (dev->slot_count != (uint32_t)(dev->disk_bytes / ZRAM_PAGE_SIZE)) {
        return false;
    }

    /* Check that all slots are accounted for. */
    uint32_t accounted = 0;
    for (uint32_t i = 0; i < dev->slot_count; i++) {
        if (dev->slots[i].state != ZRAM_EMPTY) {
            accounted++;
        }
    }

    /* Check that the sum of pages_* matches accounted. */
    uint64_t accounted_pages = dev->pages_same + dev->pages_raw + dev->pages_lz4;
    if (accounted_pages != accounted) {
        return false;
    }

    /* Check that stored_bytes matches the sum of class sizes. */
    uint64_t stored = 0;
    for (uint32_t i = 0; i < ZRAM_CLASS_COUNT; i++) {
        stored += (uint64_t)g_allocators[i].class.count * kZramClassSize[i];
    }

    if (stored != dev->stored_bytes) {
        return false;
    }

    /* Check for overlapping objects. */
    for (uint32_t c = 0; c < ZRAM_CLASS_COUNT; c++) {
        struct ZramClass* obj = g_allocators[c].class.next;
        while (obj != NULL) {
            struct ZramClass* next = ((struct ZramClass*)obj)->next;
            /* Check that this object is not in the free list. */
            if (obj == g_allocators[c].class.next) {
                /* It's the head of the free list, which is fine. */
            } else {
                /* Check against all other objects in the class. */
                struct ZramClass* other = g_allocators[c].class.next;
                while (other != NULL && other != obj) {
                    uint8_t* p1 = (uint8_t*)obj;
                    uint8_t* p2 = (uint8_t*)other;
                    uint32_t size = kZramClassSize[c];
                    if (p1 + size > p2 && p2 + size > p1) {
                        return false;  /* overlap detected */
                    }
                    other = ((struct ZramClass*)other)->next;
                }
            }
            obj = next;
        }
    }

    return true;
}