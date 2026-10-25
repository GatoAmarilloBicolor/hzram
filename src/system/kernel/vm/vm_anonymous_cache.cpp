/*
 * VMAnonymousCache - Virtual Memory Anonymous Cache
 * 
 * Implements a swap-based memory management system with zram support.
 * 
 * Dependencies:
 * - zram/zram_kernel.h - API for zram kernel module
 * - radix_bitmap.h - Bitmap allocation management
 * - mutex.h - Mutex synchronization
 */

#include "zram/zram_kernel.h"
#include "radix_bitmap.h"
#include "mutex.h"

/**
 * swap_file - Structure for swap file management
 * Handles both filesystem and zram-backed swap operations
 */
struct swap_file {
    int             fd;                 // File descriptor
    struct vnode*   vnode;             // Vnode pointer
    void*           cookie;             // Cookie for file identification
    swap_addr_t     first_slot;        // First swap slot index
    swap_addr_t     last_slot;         // Last swap slot index
    radix_bitmap*   bmp;               // Bitmap for slot tracking
    ZramKernelDevice* zram;            // Optional zram device reference
};

/**
 * Initialize swap file with zram device
 * Sets up the zram connection if available
 */
static void swap_file_init(ZramKernelDevice* zram) {
    swap_file* swap = new(std::nothrow) swap_file;
    if (!swap) return;
    
    swap->zram = zram;
    swap->bmp = radix_bitmap_create((size_t)zram ? zram->disk_bytes : 0, B_PAGE_SIZE);
    if (!swap->bmp) {
        delete swap;
        return;
    }
    
    swap->first_slot = 0;
    swap->last_slot = 0;
}

/**
 * Allocate a swap slot, prioritizing zram-backed files
 * Returns the slot index or NULL on failure
 */
static swap_addr_t swap_file_alloc(ZramKernelDevice* zram) {
    if (!zram) return NULL;
    
    int count = swap_file_get_count(zram);
    swap_addr_t slot = radix_bitmap_alloc(zram->bmp, count);
    if (slot == NULL) return NULL;
    
    swap_file* swap = new(std::nothrow) swap_file;
    swap->zram = zram;
    swap->bmp = radix_bitmap_create((size_t)zram->disk_bytes / B_PAGE_SIZE);
    if (!swap->bmp) {
        delete swap;
        return NULL;
    }
    
    swap->first_slot = slot;
    swap->last_slot = slot;
    swap_file_set_count(zram, 1);
    
    return slot;
}

/**
 * Remove a swap slot from the file
 * Frees both the swap file and any zram resources
 */
static void swap_file_delete(ZramKernelDevice* zram) {
    if (!zram) return;
    
    swap_file* swap = swap_file_get(zram);
    if (swap && swap->zram) {
        swap->zram = NULL;
        swap->bmp = NULL;
        swap_file_set_count(zram, 0);
    }
}

/**
 * Get maximum slot count for a zram device
 */
static int swap_file_get_max_slots(ZramKernelDevice* zram) {
    return swap_file_get_count(zram);
}

/**
 * Read a page from swap file, using zram if available
 * Returns status code on success or failure
 */
static status_t zram_swap_read(ZramKernelDevice* zram, uint32_t pos, 
                                const void* vecs, uint32_t j,
                                uint32_t flags, size_t numBytes) {
    status_t status;
    swap_file* swap = swap_file_get(zram);
    
    if (!swap) {
        return status;
    }
    
    if (swap->zram) {
        status = zram_swap_read(swap->zram, pos, vecs + j, j, numBytes, flags);
    } else {
        status = vfs_read_pages(swap->vnode, swap->cookie, pos, vecs + j, j, numBytes, flags);
    }
    
    return status;
}

/**
 * Write a page to swap file, using zram if available
 * Returns status code on success or failure
 */
static status_t zram_swap_write(ZramKernelDevice* zram, uint32_t pos,
                                const void* vecs, uint32_t j,
                                uint32_t numBytes, uint32_t flags, void* callback) {
    status_t status;
    swap_file* swap = swap_file_get(zram);
    
    if (!swap) {
        return status;
    }
    
    status = zram_swap_write(swap->zram, pos, vecs, j, numBytes, flags, callback);
    return status;
}

/**
 * Check if a swap file can accept a request
 * Returns true if zram is available and can handle the request
 */
static bool zram_kernel_can_accept(ZramKernelDevice* zram) {
    swap_file* swap = swap_file_get(zram);
    return swap && swap->zram;
}

/**
 * Initialize post-module settings for zram
 * Loads configuration and creates zram swap space if needed
 */
static void swap_init_post_modules() {
    const char* size = load_driver_parameter("virtual_memory", "zram_size", NULL, NULL);
    const char* pool = load_driver_parameter("virtual_memory", "zram_pool_limit", NULL, NULL);
    
    if (size && pool) {
        uint64_t zramSize = atoll(size);
        uint64_t zramPool = atoll(pool);
        
        if (zramSize >= (16ull * 1024 * 1024) && zramPool >= ZRAM_CHUNK_BYTES && zramPool % ZRAM_CHUNK_BYTES == 0) {
            status_t status = swap_zram_add(zramSize, zramPool);
            if (status != B_OK) {
                dprintf("swap_init_post_modules: swap_zram_add: %s\n", strerror(status));
            }
        }
    }
    unload_driver_settings("virtual_memory");
}

/**
 * Dump swap information for debugging
 * Prints statistics about zram swap space
 */
static void dump_swap_info() {
    const ZramKernelDevice* dev = g_kernel_devices;
    if (dev) {
        uint32_t total = dev->slots.count;
        uint32_t free = 0;
        for (size_t i = 0; i < total; i++) {
            if (dev->slots[i].state != ZRAM_EMPTY) free++;
        }
        kprintf("  zram: pages: total: %zu, free: %zu\n", total, free);
    }
}

/**
 * Delete supported swap files
 * Removes zram-backed swap files from the system
 */
static status_t swap_file_delete(ZramKernelDevice* zram) {
    if (!zram) return B_NOT_SUPPORTED;
    swap_file* swap = swap_file_get(zram);
    return swap && swap->zram ? B_NOT_SUPPORTED : B_OK;
}

/**
 * Add new swap file to kernel versions
 * Creates zram swap space and initializes the file
 */
static status_t swap_file_add(ZramKernelDevice* zram, uint64_t diskBytes, uint64_t poolLimit) {
    ZramKernelDevice* dev = NULL;
    status_t status = zram_kernel_create(diskBytes, poolLimit, &dev);
    if (status != B_OK) return B_NO_MEMORY;
    
    uint32_t pageCount = (uint32_t)(diskBytes / B_PAGE_SIZE);
    swap_file* swap = new(std::nothrow) swap_file;
    if (!swap) return B_NO_MEMORY;
    
    swap->fd = -1;
    swap->vnode = NULL;
    swap->cookie = NULL;
    swap->zram = dev;
    swap->bmp = radix_bitmap_create((size_t)zram->disk_bytes / B_PAGE_SIZE);
    if (!swap->bmp) {
        delete swap;
        return B_NO_MEMORY;
    }
    
    swap->bmp->free_slots = pageCount;
    swap->first_slot = 0;
    swap->last_slot = pageCount;
    swap_file_set_count(zram, 1);
    return B_OK;
}

/**
 * New function swap_zram_add() - Creates swap file with zram
 * Wrapper for swap_file_add with zram initialization
 */
static status_t swap_zram_add(uint64_t diskBytes, uint64_t poolLimit) {
    ZramKernelDevice* dev = NULL;
    status_t status = zram_kernel_create(diskBytes, poolLimit, &dev);
    if (status != B_OK) return status;
    
    uint32_t pageCount = (uint32_t)(diskBytes / B_PAGE_SIZE);
    swap_file* swap = new(std::nothrow) swap_file;
    if (!swap) {
        zram_kernel_destroy(dev);
        return B_NO_MEMORY;
    }
    
    swap->fd = -1;
    swap->vnode = NULL;
    swap->cookie = NULL;
    swap->zram = dev;
    swap->bmp = radix_bitmap_create(pageCount);
    if (swap->bmp == NULL) {
        delete swap;
        zram_kernel_destroy(dev);
        return B_NO_MEMORY;
    }
    
    swap->first_slot = 0;
    swap->last_slot = pageCount;
    
    mutex_lock(&sSwapFileListLock);
    if (sSwapFileList.IsEmpty()) {
        swap->first_slot = 0;
        swap->last_slot = pageCount;
    } else {
        swap->first_slot = sSwapFileList.Last()->last_slot + 1;
        swap->last_slot = swap->first_slot + pageCount;
    }
    sSwapFileList.Add(swap);
    sSwapFileCount++;
    mutex_unlock(&sSwapFileListLock);
    
    mutex_lock(&sAvailableSwapSpaceLock);
    sAvailableSwapSpace += (off_t)pageCount * B_PAGE_SIZE;
    mutex_unlock(&sAvailableSwapSpaceLock);
    
    return B_OK;
}

/**
 * Initialize zram kernel after modules load
 * Enables debug commands and zram functionality
 */
static void swap_init_post_modules_debug() {
    swap_init_post_modules();
    zram_kernel_debug_init();
}

/**
 * Dump swap information for debugging
 */
static void dump_swap_info_debug() {
    dump_swap_info();
}

/**
 * Initialize post-modules with zram support
 * Called after kernel modules initialize
 */
static void swap_init_post_modules_full() {
    swap_init_post_modules();
    swap_init_post_modules_debug();
}

#endif
