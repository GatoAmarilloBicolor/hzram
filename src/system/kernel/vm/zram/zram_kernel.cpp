/*
 * zram_kernel.cpp - Pegamento del kernel de zram para Haiku
 *
 * Capa 2 del diseño (h_zram_plan_implementacion.md, §6 T6). Implementa
 * ZramPlatformOps con las primitivas reales de Haiku/BeOS:
 *
 *   - mutex_init/lock/unlock/destroy        (headers/private/kernel/lock.h)
 *   - create_sem/acquire_sem/release_sem    (headers/os/kernel/OS.h)
 *   - spawn_thread/wait_for_thread          (headers/os/kernel/OS.h)
 *   - smp_get_num_cpus()                    (headers/private/kernel/smp.h)
 *   - vm_memcpy_from_physical/to_physical   (headers/private/kernel/vm/vm.h)
 *   - create_area/delete_area               (headers/os/kernel/OS.h)
 *   - add_debugger_command_etc              (headers/private/kernel/debug.h)
 *
 * La memoria cableada del pool se reserva completa en zram_kernel_create()
 * (create_area con B_FULL_LOCK) y chunk_alloc() la reparte de forma no
 * bloqueante. En v1 el pool no se encoge, así que chunk_free() es no-op.
 */

#include "zram_kernel.h"
#include "zram_core.h"

#include <OS.h>
#include <debug.h>
#include <device_manager/IORequest.h>   /* B_PHYSICAL_IO_REQUEST */
#include <lock.h>
#include <smp.h>
#include <stdlib.h>
#include <string.h>
#include <thread.h>
#include <vfs.h>
#include <vm/vm.h>

/*
 * ============================================================================
 * DISPOSITIVO DEL KERNEL
 * ============================================================================
 */

struct ZramKernelDevice {
    ZramDevice* core;
    void*       pool_base;
    void*       pool_end;
    void*       pool_bump;
    area_id     pool_area;
    uint64_t    disk_bytes;
    uint64_t    pool_limit;
    thread_id   refill_thread;
    sem_id      refill_sem;
    volatile bool refill_done;
    ZramKernelDevice* next;
};

/* v1: un único dispositivo zram (una entrada zram_size/zram_pool_limit).
 * chunk_alloc() no recibe puntero de usuario en ZramPlatformOps, así que se
 * usa este puntero global. Para soportar varios dispositivos habría que
 * ampliar la firma de chunk_alloc (T11). */
static ZramKernelDevice* g_kernel_device = NULL;
static ZramKernelDevice* g_kernel_devices = NULL;

/*
 * ============================================================================
 * OPERACIONES DE PLATAFORMA
 * ============================================================================
 */

static void* kern_mem_alloc(size_t size)
{
    return malloc(size);
}

static void kern_mem_free(void* ptr)
{
    free(ptr);
}

static void* kern_chunk_alloc(size_t bytes)
{
    ZramKernelDevice* dev = g_kernel_device;
    if (dev == NULL)
        return NULL;

    if ((uint8_t*)dev->pool_bump + bytes > (uint8_t*)dev->pool_end)
        return NULL;

    void* chunk = dev->pool_bump;
    dev->pool_bump = (uint8_t*)chunk + bytes;
    return chunk;
}

static void kern_chunk_free(void* ptr, size_t bytes)
{
    /* v1: el pool no se encoge; se libera en zram_kernel_destroy(). */
    (void)ptr;
    (void)bytes;
}

static void* kern_mutex_new(void)
{
    mutex* m = (mutex*)malloc(sizeof(mutex));
    if (m == NULL)
        return NULL;
    mutex_init(m, "zram");
    return m;
}

static void kern_mutex_lock(void* lock)
{
    mutex_lock((mutex*)lock);
}

static void kern_mutex_unlock(void* lock)
{
    mutex_unlock((mutex*)lock);
}

static void kern_mutex_delete(void* lock)
{
    if (lock == NULL)
        return;
    mutex_destroy((mutex*)lock);
    free(lock);
}

static void* kern_sem_new(uint32_t count)
{
    sem_id id = create_sem(count, "zram");
    return id >= 0 ? (void*)(intptr_t)id : NULL;
}

static void kern_sem_acquire(void* sem)
{
    acquire_sem((sem_id)(intptr_t)sem);
}

static void kern_sem_release(void* sem)
{
    release_sem((sem_id)(intptr_t)sem);
}

static void kern_sem_delete(void* sem)
{
    delete_sem((sem_id)(intptr_t)sem);
}

static uint32_t kern_cpu_count(void)
{
    int32 n = smp_get_num_cpus();
    return n > 0 ? (uint32_t)n : 1;
}

static void kern_request_refill(void* user)
{
    ZramKernelDevice* dev = (ZramKernelDevice*)user;
    if (dev != NULL)
        release_sem(dev->refill_sem);
}

static const ZramPlatformOps g_platform_ops = {
    kern_mem_alloc,
    kern_mem_free,
    kern_chunk_alloc,
    kern_chunk_free,
    kern_mutex_new,
    kern_mutex_lock,
    kern_mutex_unlock,
    kern_mutex_delete,
    kern_sem_new,
    kern_sem_acquire,
    kern_sem_release,
    kern_sem_delete,
    kern_cpu_count,
    kern_request_refill,
    NULL,   /* user se fija por dispositivo al crear */
};

/*
 * ============================================================================
 * HILO DE REPOSICIÓN
 * ============================================================================
 */

static status_t zram_refill_thread(void* data)
{
    ZramKernelDevice* dev = (ZramKernelDevice*)data;

    while (!dev->refill_done) {
        acquire_sem(dev->refill_sem);
        if (dev->refill_done)
            break;
        zram_refill(dev->core);
    }

    return B_OK;
}

/*
 * ============================================================================
 * CREACIÓN / DESTRUCCIÓN
 * ============================================================================
 */

status_t zram_kernel_create(uint64_t diskBytes, uint64_t poolLimit,
    ZramKernelDevice** out)
{
    if (out == NULL)
        return B_BAD_VALUE;
    if (diskBytes == 0 || poolLimit == 0)
        return B_BAD_VALUE;
    if (diskBytes % B_PAGE_SIZE != 0)
        return B_BAD_VALUE;
    if (poolLimit % ZRAM_CHUNK_BYTES != 0)
        return B_BAD_VALUE;

    ZramKernelDevice* dev = (ZramKernelDevice*)malloc(sizeof(*dev));
    if (dev == NULL)
        return B_NO_MEMORY;
    memset(dev, 0, sizeof(*dev));
    dev->pool_area = -1;

    /* Reserva completa del pool: memoria cableada y no bloqueante. */
    void* pool = NULL;
    area_id area = create_area("zram_pool", &pool, B_ANY_KERNEL_ADDRESS,
        poolLimit, B_FULL_LOCK, B_READ_AREA | B_WRITE_AREA);
    if (area < 0) {
        free(dev);
        return B_NO_MEMORY;
    }

    dev->pool_area = area;
    dev->pool_base = pool;
    dev->pool_end = (uint8_t*)pool + poolLimit;
    dev->pool_bump = pool;
    dev->disk_bytes = diskBytes;
    dev->pool_limit = poolLimit;
    g_kernel_device = dev;

    ZramConfig cfg = { diskBytes, poolLimit };
    int rc = zram_create(&cfg, &g_platform_ops, &dev->core);
    if (rc != 0) {
        delete_area(area);
        free(dev);
        return B_NO_MEMORY;
    }

    dev->refill_sem = create_sem(0, "zram_refill");
    if (dev->refill_sem < 0) {
        zram_destroy(dev->core);
        delete_area(area);
        free(dev);
        return B_NO_MEMORY;
    }
    dev->refill_done = false;

    dev->refill_thread = spawn_thread(zram_refill_thread, "zram_refill",
        B_NORMAL_PRIORITY, dev);
    if (dev->refill_thread < 0) {
        delete_sem(dev->refill_sem);
        zram_destroy(dev->core);
        delete_area(area);
        free(dev);
        return B_NO_MEMORY;
    }
    resume_thread(dev->refill_thread);

    dev->next = g_kernel_devices;
    g_kernel_devices = dev;

    *out = dev;
    return B_OK;
}

void zram_kernel_destroy(ZramKernelDevice* dev)
{
    if (dev == NULL)
        return;

    dev->refill_done = true;
    release_sem(dev->refill_sem);

    status_t status;
    wait_for_thread(dev->refill_thread, &status);

    delete_sem(dev->refill_sem);
    zram_destroy(dev->core);
    if (dev->pool_area >= 0)
        delete_area(dev->pool_area);
    free(dev);
}

/*
 * ============================================================================
 * LECTURA / ESCRITURA POR VECTORES
 * ============================================================================
 */

static bool zram_is_physical(uint32_t flags)
{
    return (flags & B_PHYSICAL_IO_REQUEST) != 0;
}

status_t zram_swap_write(ZramKernelDevice* dev, off_t pos,
    const generic_io_vec* vecs, size_t count, uint32_t flags,
    generic_size_t* numBytes)
{
    if (dev == NULL)
        return B_BAD_VALUE;
    if (numBytes != NULL)
        *numBytes = 0;
    if (count == 0)
        return B_OK;
    if (vecs == NULL || pos < 0 || pos % B_PAGE_SIZE != 0)
        return B_BAD_VALUE;

    ZramCtx* ctx = zram_ctx_get(dev->core);
    if (ctx == NULL)
        return B_NO_MEMORY;

    bool physical = zram_is_physical(flags);
    uint32_t slot = (uint32_t)(pos / B_PAGE_SIZE);
    generic_size_t total = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (vecs[i].length % B_PAGE_SIZE != 0) {
            zram_ctx_put(dev->core, ctx);
            return B_BAD_VALUE;
        }

        for (generic_size_t off = 0; off < vecs[i].length; off += B_PAGE_SIZE) {
            const void* src = (const void*)(vecs[i].base + off);

            if (physical) {
                vm_memcpy_from_physical(zram_ctx_page(ctx),
                    (phys_addr_t)(vecs[i].base + off), B_PAGE_SIZE, false);
                src = zram_ctx_page(ctx);
            }

            int rc = zram_write_page(dev->core, ctx, slot, src);
            if (rc != 0) {
                zram_ctx_put(dev->core, ctx);
                return rc == -2 ? B_NO_MEMORY : B_ERROR;
            }

            slot++;
            total += B_PAGE_SIZE;
        }
    }

    zram_ctx_put(dev->core, ctx);
    if (numBytes != NULL)
        *numBytes = total;
    return B_OK;
}

status_t zram_swap_read(ZramKernelDevice* dev, off_t pos,
    const generic_io_vec* vecs, size_t count, uint32_t flags,
    generic_size_t* numBytes)
{
    if (dev == NULL)
        return B_BAD_VALUE;
    if (numBytes != NULL)
        *numBytes = 0;
    if (count == 0)
        return B_OK;
    if (vecs == NULL || pos < 0 || pos % B_PAGE_SIZE != 0)
        return B_BAD_VALUE;

    ZramCtx* ctx = zram_ctx_get(dev->core);
    if (ctx == NULL)
        return B_NO_MEMORY;

    bool physical = zram_is_physical(flags);
    uint32_t slot = (uint32_t)(pos / B_PAGE_SIZE);
    generic_size_t total = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (vecs[i].length % B_PAGE_SIZE != 0) {
            zram_ctx_put(dev->core, ctx);
            return B_BAD_VALUE;
        }

        for (generic_size_t off = 0; off < vecs[i].length; off += B_PAGE_SIZE) {
            void* dst = (void*)(vecs[i].base + off);
            void* read_dst = dst;

            if (physical)
                read_dst = zram_ctx_page(ctx);

            int rc = zram_read_page(dev->core, slot, read_dst);
            if (rc != 0) {
                zram_ctx_put(dev->core, ctx);
                return B_ERROR;
            }

            if (physical) {
                vm_memcpy_to_physical((phys_addr_t)(vecs[i].base + off),
                    read_dst, B_PAGE_SIZE, false);
            }

            slot++;
            total += B_PAGE_SIZE;
        }
    }

    zram_ctx_put(dev->core, ctx);
    if (numBytes != NULL)
        *numBytes = total;
    return B_OK;
}

/*
 * ============================================================================
 * CONSULTAS
 * ============================================================================
 */

void zram_kernel_free(ZramKernelDevice* dev, swap_addr_t firstSlot,
    uint32_t count)
{
    if (dev == NULL)
        return;
    zram_free_slots(dev->core, firstSlot, count);
}

bool zram_kernel_can_accept(ZramKernelDevice* dev)
{
    if (dev == NULL)
        return false;
    return zram_can_accept(dev->core);
}

void zram_kernel_get_stats(ZramKernelDevice* dev, ZramStats* out)
{
    if (dev == NULL)
        return;
    zram_get_stats(dev->core, out);
}

bool zram_kernel_check_invariants(ZramKernelDevice* dev)
{
    if (dev == NULL)
        return false;
    return zram_check_invariants(dev->core);
}

/*
 * ============================================================================
 * COMANDO DE DEPURACIÓN
 * ============================================================================
 */

static int zram_debug_command(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    for (ZramKernelDevice* dev = g_kernel_devices; dev != NULL;
            dev = dev->next) {
        ZramStats st;
        zram_get_stats(dev->core, &st);

        kprintf("zram device %p:\n", dev);
        kprintf("  disk_bytes: %" B_PRIu64 "\n", dev->disk_bytes);
        kprintf("  pool_limit: %" B_PRIu64 "\n", dev->pool_limit);
        kprintf("  pool_bytes: %" B_PRIu64 "\n", st.pool_bytes);
        kprintf("  pages_same: %" B_PRIu64 "\n", st.pages_same);
        kprintf("  pages_raw:  %" B_PRIu64 "\n", st.pages_raw);
        kprintf("  pages_lz4:  %" B_PRIu64 "\n", st.pages_lz4);
        kprintf("  orig_bytes: %" B_PRIu64 "\n", st.orig_bytes);
        kprintf("  stored:     %" B_PRIu64 "\n", st.stored_bytes);
        kprintf("  writes:     %" B_PRIu64 "\n", st.writes);
        kprintf("  reads:      %" B_PRIu64 "\n", st.reads);
        kprintf("  frees:      %" B_PRIu64 "\n", st.frees);
        kprintf("  write_nomem:%" B_PRIu64 "\n", st.write_nomem);
        kprintf("  read_corrupt:%" B_PRIu64 "\n", st.read_corrupt);
        kprintf("  read_empty: %" B_PRIu64 "\n", st.read_empty);
    }

    return 0;
}

void zram_kernel_debug_init(void)
{
    add_debugger_command_etc("zram", zram_debug_command,
        "Show zram usage statistics", "zram\n", 0);
}
