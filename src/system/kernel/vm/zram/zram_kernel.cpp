/*
 * zram_kernel.cpp - zram kernel module implementation for Haiku
 *
 * This module integrates zram into the Haiku swap system.
 *
 * Architecture:
 *   - zram_kernel_create: called by swap_zram_add() to create a device
 *   - zram_kernel_read: implements swap_file_read
 *   - zram_kernel_write: implements swap_file_write
 *   - zram_kernel_free: implements swap_slot_dealloc
 *   - zram_kernel_can_accept: implements swap_file_can_accept
 *
 * The module is loaded as a kernel module and registered as a swap file
 * type. When swap_file_add() is called with a zram device, the kernel
 * swap code will call the appropriate zram_kernel_* functions.
 */

#include "zram_kernel.h"
#include "zram_core.h"

#include <kernel/kernel.h>
#include <kernel/vm/vm.h>
#include <kernel/vm/vm_page.h>
#include <kernel/swap/swap.h>
#include <kernel/swap/swap_file.h>
#include <kernel/swap/swap_slot.h>
#include <kernel/io/io_request.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/lock.h>
#include <kernel/OS.h>
#include <kernel/OS/sem.h>
#include <kernel/OS/mutex.h>
#include <kernel/util/DoublyLinkedList.h>
#include <kernel/posix/posix.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/*
 * ============================================================================
 * GLOBAL STATE
 * ============================================================================
 */

static struct ZramDevice* g_zram_devices = NULL;
static uint32_t g_zram_device_count = 0;
static uint64_t g_total_zram_size = 0;
static bool g_zram_enabled = false;

/*
 * ============================================================================
 * INTERNAL STRUCTURES
 * ============================================================================
 */

struct ZramDevice {
    ZramCoreDevice* core;      /* the core zram device */
    swap_file_t* swap_file;    /* the swap_file_t wrapper */
    swap_file_t* next;         /* linked list next pointer */
    uint32_t first_slot;       /* first slot index in the device */
    uint32_t last_slot;        /* last slot index in the device */
    uint64_t disk_bytes;       /* nominal size */
    uint64_t pool_limit;       /* pool memory limit */
    uint64_t pool_bytes;       /* current pool usage */
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
    void* user_data;           /* user pointer passed to zram_create */
    bool initialized;
    bool destroyed;
    pthread_t refill_thread;
    sem_t* refill_sem;
    volatile int refill_thread_done;
    volatile int running;
};

/*
 * ============================================================================
 * PLATFORM OPS
 * ============================================================================
 */

static void* zram_mem_alloc(size_t size)
{
    /* Use the kernel's memory allocator.
     * This is safe to call from create/destroy paths.
     */
    return malloc(size);
}

static void zram_mem_free(void* ptr)
{
    free(ptr);
}

static void* zram_chunk_alloc(size_t bytes)
{
    /* Allocate a chunk of physical memory.
     * We use vm_create_area to get physical memory.
     *
     * The chunk must be aligned to a page boundary.
     *
     * Returns NULL if no memory is available.
     * Does NOT block.
     */
    void* result = NULL;

    /* Try to allocate a page-sized chunk. */
    area_t* area = vm_create_area(B_PAGE_SIZE, B_FULL_LOCK, B_READWRITE, B_ANY_DEVICE);
    if (area != NULL) {
        result = area->data;
        vm_delete_area(area);
    }

    return result;
}

static void zram_chunk_free(void* ptr, size_t size)
{
    /* Free a chunk of physical memory.
     * We need to find the area that owns this memory.
     *
     * This is tricky because we don't have a direct mapping from
     * pointer to area. We'll use a simple approach:
     *   1. Find the page that contains the pointer.
     *   2. Find the area that owns that page.
     *   3. Delete the area.
     */
    (void)size;  /* unused */

    /* Find the page containing the pointer. */
    page_addr_t page_addr = (page_addr_t)(uintptr_t)ptr;
    page_addr = (page_addr & ~((page_addr_t)~0)) >> 12;  /* round down to page */

    /* Find the area that owns this page. */
    area_t* area = NULL;
    for (area_t* a = g_kern_area_list; a != NULL; a = a->next) {
        if (a->page_addr <= page_addr && page_addr < a->page_addr + a->page_count) {
            area = a;
            break;
        }
    }

    if (area != NULL) {
        vm_delete_area(area);
    }
}

static void* zram_mutex_new(void)
{
    return mutex_new();
}

static void zram_mutex_lock(void* lock)
{
    mutex_lock((mutex_t*)lock);
}

static void zram_mutex_unlock(void* lock)
{
    mutex_unlock((mutex_t*)lock);
}

static void zram_mutex_delete(void* lock)
{
    mutex_delete((mutex_t*)lock);
}

static void* zram_sem_new(uint32_t count)
{
    return sem_new(count);
}

static void zram_sem_acquire(void* sem)
{
    sem_acquire((sem_t*)sem);
}

static void zram_sem_release(void* sem)
{
    sem_release((sem_t*)sem);
}

static void zram_sem_delete(void* sem)
{
    sem_delete((sem_t*)sem);
}

static uint32_t zram_cpu_count(void)
{
    return smp_get_num_cpus();
}

static void zram_request_refill(void* user)
{
    ((ZramDevice*)user)->refill_thread = user;
    /* Wake up the refill thread. */
    sem_post(((ZramDevice*)user)->refill_sem);
}

static const ZramPlatformOps g_platform_ops = {
    .mem_alloc = zram_mem_alloc,
    .mem_free = zram_mem_free,
    .chunk_alloc = zram_chunk_alloc,
    .chunk_free = zram_chunk_free,
    .mutex_new = zram_mutex_new,
    .mutex_lock = zram_mutex_lock,
    .mutex_unlock = zram_mutex_unlock,
    .mutex_delete = zram_mutex_delete,
    .sem_new = zram_sem_new,
    .sem_acquire = zram_sem_acquire,
    .sem_release = zram_sem_release,
    .sem_delete = zram_sem_delete,
    .cpu_count = zram_cpu_count,
    .request_refill = zram_request_refill,
    .user = NULL,
};

/*
 * ============================================================================
 * KERNEL MODULE FUNCTIONS
 * ============================================================================
 */

status_t zram_kernel_create(uint64_t diskBytes, uint64_t poolLimit, ZramDevice** out)
{
    if (out == NULL) {
        return B_BAD_VALUE;
    }

    if (diskBytes == 0 || poolLimit == 0) {
        return B_BAD_VALUE;
    }

    if (diskBytes % ZRAM_PAGE_SIZE != 0) {
        return B_BAD_VALUE;
    }

    if (poolLimit % ZRAM_CHUNK_BYTES != 0) {
        return B_BAD_VALUE;
    }

    if (poolLimit < ZRAM_CHUNK_BYTES) {
        return B_BAD_VALUE;
    }

    /* Create the core zram device. */
    ZramConfig cfg;
    cfg.disk_bytes = diskBytes;
    cfg.pool_limit = poolLimit;

    ZramDevice* core_dev = NULL;
    int rc = zram_create(&cfg, &g_platform_ops, &core_dev);
    if (rc != 0) {
        return B_NO_MEMORY;
    }

    /* Wrap it in a kernel device. */
    ZramDevice* dev = (ZramDevice*)malloc(sizeof(ZramDevice));
    if (dev == NULL) {
        zram_destroy(core_dev);
        return B_NO_MEMORY;
    }

    dev->core = core_dev;
    dev->swap_file = NULL;  /* will be set by the caller */
    dev->next = NULL;
    dev->first_slot = 0;
    dev->last_slot = 0;
    dev->disk_bytes = diskBytes;
    dev->pool_limit = poolLimit;
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
    dev->user_data = NULL;
    dev->initialized = true;
    dev->destroyed = false;

    /* Set up the linked list. */
    dev->next = g_zram_devices;
    g_zram_devices = dev;
    g_zram_device_count++;
    g_total_zram_size += diskBytes;

    *out = dev;
    return B_OK;
}

static void zram_kernel_destroy(ZramDevice* dev)
{
    if (dev == NULL) {
        return;
    }

    /* Stop the refill thread. */
    dev->running = 0;
    dev->refill_thread_done = 1;
    sem_post(dev->refill_sem);
    pthread_join(dev->refill_thread, NULL);

    /* Destroy the core device. */
    zram_destroy(dev->core);

    /* Free the device. */
    free(dev);

    /* Update the global list. */
    if (dev == g_zram_devices) {
        g_zram_devices = g_zram_devices->next;
    } else {
        ZramDevice* prev = g_zram_devices;
        while (prev != NULL && prev->next != dev) {
            prev = prev->next;
        }
        if (prev != NULL) {
            prev->next = dev->next;
        }
    }
    g_zram_device_count--;
    g_total_zram_size -= dev->disk_bytes;
}

void zram_kernel_free(ZramDevice* dev, swap_addr_t firstSlot, uint32_t count)
{
    if (dev == NULL || dev->destroyed) {
        return;
    }

    /* Convert slot indices to core slot indices. */
    uint32_t first = firstSlot - dev->first_slot;
    uint32_t last = firstSlot + count - dev->first_slot;

    if (first < 0 || last > (uint32_t)dev->core->slot_count) {
        return;
    }

    if (last <= first) {
        count = 0;
    }

    /* Free the slots. */
    zram_free_slots(dev->core, first, count - first);

    /* Update the device's slot range. */
    dev->last_slot = firstSlot + count;

    /* Update stats. */
    dev->frees += count;
}

bool zram_kernel_can_accept(ZramDevice* dev)
{
    if (dev == NULL || dev->destroyed) {
        return false;
    }

    return zram_can_accept(dev->core);
}

status_t zram_kernel_read(ZramDevice* dev, off_t pos,
                         const generic_io_vec* vecs, size_t count,
                         uint32_t flags, generic_size_t* numBytes)
{
    if (dev == NULL || dev->destroyed) {
        return B_ERROR;
    }

    if (count == 0) {
        *numBytes = 0;
        return B_OK;
    }

    /* Validate the request. */
    if (pos < 0 || pos >= dev->last_slot * ZRAM_PAGE_SIZE) {
        return B_BAD_VALUE;
    }

    /* Calculate the number of pages to read. */
    uint64_t bytes_to_read = (uint64_t)count;
    uint64_t remaining = dev->last_slot * ZRAM_PAGE_SIZE - pos;
    if (bytes_to_read > remaining) {
        bytes_to_read = remaining;
    }

    /* Round up to page boundaries. */
    uint64_t pages_to_read = (bytes_to_read + ZRAM_PAGE_SIZE - 1) / ZRAM_PAGE_SIZE;

    /* Allocate a buffer for the pages. */
    void* buffer = malloc(pages_to_read * ZRAM_PAGE_SIZE);
    if (buffer == NULL) {
        return B_NO_MEMORY;
    }

    /* Read the pages from the core device. */
    uint32_t slot = (uint32_t)(pos / ZRAM_PAGE_SIZE);
    bool done = false;
    size_t total = 0;

    while (!done && total < bytes_to_read) {
        uint64_t this_page = total / ZRAM_PAGE_SIZE;
        uint64_t this_bytes = (this_page + 1) * ZRAM_PAGE_SIZE - total;

        if (this_bytes > ZRAM_PAGE_SIZE) {
            this_bytes = ZRAM_PAGE_SIZE;
        }

        if (slot >= (uint32_t)dev->core->slot_count) {
            /* End of device. */
            break;
        }

        /* Read the page. */
        int rc = zram_read_page(dev->core, slot, (uint8_t*)buffer + total);
        if (rc != 0) {
            free(buffer);
            *numBytes = total;
            return rc < 0 ? B_ERROR : B_OK;
        }

        total += this_bytes;
        slot++;
        done = (total >= bytes_to_read);
    }

    /* Copy the data to the output vectors. */
    size_t offset = 0;
    for (size_t i = 0; i < count && offset < bytes_to_read; i++) {
        size_t vec_len = vecs[i].length;
        if (offset + vec_len > bytes_to_read) {
            vec_len = bytes_to_read - offset;
        }
        memcpy(vecs[i].data, (uint8_t*)buffer + offset, vec_len);
        offset += vec_len;
    }

    free(buffer);
    *numBytes = offset;
    return B_OK;
}

status_t zram_kernel_write(ZramDevice* dev, off_t pos,
                          const generic_io_vec* vecs, size_t count,
                          uint32_t flags, generic_size_t* numBytes)
{
    if (dev == NULL || dev->destroyed) {
        return B_ERROR;
    }

    if (count == 0) {
        *numBytes = 0;
        return B_OK;
    }

    /* Validate the request. */
    if (pos < 0 || pos >= dev->last_slot * ZRAM_PAGE_SIZE) {
        return B_BAD_VALUE;
    }

    /* Calculate the number of pages to write. */
    uint64_t bytes_to_write = (uint64_t)count;
    uint64_t remaining = dev->last_slot * ZRAM_PAGE_SIZE - pos;
    if (bytes_to_write > remaining) {
        bytes_to_write = remaining;
    }

    /* Round up to page boundaries. */
    uint64_t pages_to_write = (bytes_to_write + ZRAM_PAGE_SIZE - 1) / ZRAM_PAGE_SIZE;

    /* Allocate a buffer for the pages. */
    void* buffer = malloc(pages_to_write * ZRAM_PAGE_SIZE);
    if (buffer == NULL) {
        return B_NO_MEMORY;
    }

    /* Copy the data from the input vectors. */
    size_t offset = 0;
    for (size_t i = 0; i < count && offset < bytes_to_write; i++) {
        size_t vec_len = vecs[i].length;
        if (offset + vec_len > bytes_to_write) {
            vec_len = bytes_to_write - offset;
        }
        memcpy((uint8_t*)buffer + offset, vecs[i].data, vec_len);
        offset += vec_len;
    }

    /* Write the pages to the core device. */
    uint32_t slot = (uint32_t)(pos / ZRAM_PAGE_SIZE);
    bool done = false;
    size_t total = 0;

    while (!done && total < bytes_to_write) {
        uint64_t this_page = total / ZRAM_PAGE_SIZE;
        uint64_t this_bytes = (this_page + 1) * ZRAM_PAGE_SIZE - total;

        if (this_bytes > ZRAM_PAGE_SIZE) {
            this_bytes = ZRAM_PAGE_SIZE;
        }

        if (slot >= (uint32_t)dev->core->slot_count) {
            /* End of device. */
            break;
        }

        /* Write the page. */
        int rc = zram_write_page(dev->core, NULL, slot, (uint8_t*)buffer + total);
        if (rc != 0) {
            free(buffer);
            *numBytes = total;
            return rc < 0 ? B_ERROR : B_OK;
        }

        total += this_bytes;
        slot++;
        done = (total >= bytes_to_write);
    }

    free(buffer);
    *numBytes = total;
    return B_OK;
}

void zram_kernel_get_stats(ZramDevice* dev, ZramStats* out)
{
    if (dev == NULL || out == NULL) {
        return;
    }

    zram_get_stats(dev->core, out);
}

bool zram_kernel_check_invariants(ZramDevice* dev)
{
    if (dev == NULL) {
        return false;
    }

    return zram_check_invariants(dev->core);
}

/*
 * ============================================================================
 * MODULE INITIALIZATION
 * ============================================================================
 */

static status_t zram_init(void)
{
    /* Register the module. */
    return B_OK;
}

static void zram_fini(void)
{
    /* Clean up all zram devices. */
    while (g_zram_devices != NULL) {
        ZramDevice* dev = g_zram_devices;
        g_zram_devices = g_zram_devices->next;
        zram_kernel_destroy(dev);
    }
    g_zram_device_count = 0;
    g_total_zram_size = 0;
    g_zram_enabled = false;
}

MODULE_INIT(zram_init)
MODULE_FINI(zram_fini)

/*
 * ============================================================================
 * DEBUGGER COMMANDS
 * ============================================================================
 */

static status_t zram_debugger_command_show(void* user, char* buffer, size_t size)
{
    (void)user;
    (void)buffer;
    (void)size;

    /* Show zram statistics. */
    char buf[4096];
    char* p = buf;

    p += sprintf(p, "zram:\n");
    p += sprintf(p, "  devices: %u\n", g_zram_device_count);
    p += sprintf(p, "  total size: %lu MB\n", g_total_zram_size / (1024 * 1024));

    for (ZramDevice* dev = g_zram_devices; dev != NULL; dev = dev->next) {
        char name[64];
        char dev_name[64];
        if (dev->swap_file != NULL) {
            dev_name[0] = '\0';
            if (dev->swap_file->vnode != NULL) {
                dev_name[0] = dev->swap_file->vnode->name[0];
                dev_name[1] = dev->swap_file->vnode->name[1];
                dev_name[2] = dev->swap_file->vnode->name[2];
                dev_name[3] = dev->swap_file->vnode->name[3];
                dev_name[4] = dev->swap_file->vnode->name[4];
                dev_name[5] = dev->swap_file->vnode->name[5];
                dev_name[6] = dev->swap_file->vnode->name[6];
                dev_name[7] = dev->swap_file->vnode->name[7];
                dev_name[8] = '\0';
            }
        }

        p += sprintf(p, "  %s:\n", dev_name);
        p += sprintf(p, "    first_slot: %u\n", dev->first_slot);
        p += sprintf(p, "    last_slot:  %u\n", dev->last_slot);
        p += sprintf(p, "    disk_bytes: %lu MB\n", dev->disk_bytes / (1024 * 1024));
        p += sprintf(p, "    pool_limit: %lu MB\n", dev->pool_limit / (1024 * 1024));
        p += sprintf(p, "    pool_bytes: %lu MB\n", dev->pool_bytes / (1024 * 1024));
        p += sprintf(p, "    pages_same: %lu\n", dev->pages_same);
        p += sprintf(p, "    pages_raw:  %lu\n", dev->pages_raw);
        p += sprintf(p, "    pages_lz4:  %lu\n", dev->pages_lz4);
        p += sprintf(p, "    writes:     %lu\n", dev->writes);
        p += sprintf(p, "    reads:      %lu\n", dev->reads);
        p += sprintf(p, "    frees:      %lu\n", dev->frees);
        p += sprintf(p, "    write_nomem:%lu\n", dev->write_nomem);
        p += sprintf(p, "    read_corrupt:%lu\n", dev->read_corrupt);
        p += sprintf(p, "    read_empty: %lu\n", dev->read_empty);
    }

    size_t len = p - buf;
    if (len >= size) {
        len = size - 1;
    }
    memcpy(buffer, buf, len);
    buffer[len] = '\0';

    return B_OK;
}

static const struct debugger_command zram_commands[] = {
    { "show", zram_debugger_command_show, NULL, NULL, NULL },
    { NULL, NULL, NULL, NULL, NULL },
};

static const struct debugger_command_table zram_debugger_table = {
    .name = "zram",
    .commands = zram_commands,
};

static status_t zram_debugger_init(void)
{
    return debugger_register_table(&zram_debugger_table);
}

static void zram_debugger_fini(void)
{
    debugger_unregister_table(&zram_debugger_table);
}

MODULE_DEBUGGER_INIT(zram_debugger_init)
MODULE_DEBUGGER_FINI(zram_debugger_fini)