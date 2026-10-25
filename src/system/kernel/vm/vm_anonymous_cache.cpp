/*
 * VMAnonymousCache - Virtual Memory Anonymous Cache
 *
 * Implements a swap-based memory management system with zram support.
 * Provides path-based API for hzramfs: swap_file_add/swap_file_delete.
 */

#include <KernelExport.h>
#include <OS.h>
#include <drivers/Drivers.h>
#include <util/DoublyLinkedList.h>
#include <vm/VMCache.h>
#include <vm/vm_page.h>
#include <string.h>
#include <stdlib.h>

#include "zram/zram_kernel.h"
#include "radix_bitmap.h"
#include "mutex.h"

extern "C" {
#include <vm/VMAnonymousCache.h>
}

/* Forward declarations */
struct vnode;
typedef uint32 swap_addr_t;

struct swap_file : DoublyLinkedListLinkImpl<swap_file> {
	int				fd;
	struct vnode*	vnode;
	void*			cookie;
	swap_addr_t		first_slot;
	swap_addr_t		last_slot;
	radix_bitmap*	bmp;
	ZramKernelDevice* zram;
	char*			path;		/* original path for lookup */
};

/* Global swap management */
typedef DoublyLinkedList<swap_file> SwapFileList;

static SwapFileList sSwapFileList;
static mutex sSwapFileListLock = MUTEX_INITIALIZER("swap file list");
static mutex sAvailableSwapSpaceLock = MUTEX_INITIALIZER("swap space");
static int32 sSwapFileCount = 0;
static off_t sAvailableSwapSpace = 0;

static swap_file* sSwapFileAlloc = NULL;

/* Helper functions */
static swap_file* swap_file_get(ZramKernelDevice* zram)
{
	if (!zram)
		return NULL;
	
	mutex_lock(&sSwapFileListLock);
	for (SwapFileList::Iterator it = sSwapFileList.GetIterator();
			swap_file* swap = it.Next();) {
		if (swap->zram == zram) {
			mutex_unlock(&sSwapFileListLock);
			return swap;
		}
	}
	mutex_unlock(&sSwapFileListLock);
	return NULL;
}

static int swap_file_get_count(ZramKernelDevice* zram)
{
	(void)zram;
	return sSwapFileCount;
}

static void swap_file_set_count(ZramKernelDevice* zram, int count)
{
	(void)zram;
	(void)count;
}

static swap_file* swap_file_find_by_path(const char* path)
{
	if (!path)
		return NULL;
	
	mutex_lock(&sSwapFileListLock);
	for (SwapFileList::Iterator it = sSwapFileList.GetIterator();
			swap_file* swap = it.Next();) {
		if (swap->path && strcmp(swap->path, path) == 0) {
			mutex_unlock(&sSwapFileListLock);
			return swap;
		}
	}
	mutex_unlock(&sSwapFileListLock);
	return NULL;
}

static uint64 parse_number_value(const char* string, uint64 defaultValue)
{
	if (string == NULL || *string == '\0')
		return defaultValue;

	uint64 value = 0;
	bool any = false;
	while (*string >= '0' && *string <= '9') {
		uint64 next = value * 10 + (*string - '0');
		if (next < value)
			return defaultValue;
		value = next;
		any = true;
		string++;
	}

	if (!any)
		return defaultValue;

	switch (*string) {
		case 'k':
		case 'K':
			value *= 1024;
			break;
		case 'm':
		case 'M':
			value *= 1024 * 1024;
			break;
		case 'g':
		case 'G':
			value *= 1024ULL * 1024 * 1024;
			break;
		case 't':
		case 'T':
			value *= 1024ULL * 1024 * 1024 * 1024;
			break;
		default:
			break;
	}

	return value;
}

static void parse_path_params(const char* path, uint64* diskBytes, uint64* poolLimit)
{
	*diskBytes = 512ULL * 1024 * 1024;	/* default 512MB */
	*poolLimit = 128ULL * 1024 * 1024;	/* default 128MB */
	
	if (!path)
		return;
	
	const char* size = strstr(path, "size=");
	if (size) {
		size += 5;
		*diskBytes = parse_number_value(size, *diskBytes);
	}
	
	const char* pool = strstr(path, "pool=");
	if (pool) {
		pool += 5;
		*poolLimit = parse_number_value(pool, *poolLimit);
	}
	
	/* Apply minimums and alignment */
	if (*diskBytes < 16ULL * 1024 * 1024)
		*diskBytes = 16ULL * 1024 * 1024;
	if (*poolLimit < ZRAM_CHUNK_BYTES)
		*poolLimit = ZRAM_CHUNK_BYTES;
	
	/* Align to page size */
	*diskBytes = (*diskBytes + B_PAGE_SIZE - 1) & ~(B_PAGE_SIZE - 1);
	*poolLimit = (*poolLimit + ZRAM_CHUNK_BYTES - 1) & ~(ZRAM_CHUNK_BYTES - 1);
}

/**
 * Create zram swap space
 */
status_t swap_zram_add(uint64_t diskBytes, uint64_t poolLimit)
{
	ZramKernelDevice* dev = NULL;
	status_t status = zram_kernel_create(diskBytes, poolLimit, &dev);
	if (status != B_OK)
		return status;
	
	uint32_t pageCount = (uint32_t)(diskBytes / B_PAGE_SIZE);
	
	swap_file* swap = new(std::nothrow) swap_file;
	if (swap == NULL) {
		zram_kernel_destroy(dev);
		return B_NO_MEMORY;
	}
	
	swap->fd = -1;
	swap->vnode = NULL;
	swap->cookie = NULL;
	swap->zram = dev;
	swap->path = NULL;
	swap->bmp = radix_bitmap_create(pageCount);
	if (swap->bmp == NULL) {
		delete swap;
		zram_kernel_destroy(dev);
		return B_NO_MEMORY;
	}
	
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
 * Path-based API for hzramfs
 */
extern "C" status_t swap_file_add(const char* path)
{
	if (!path)
		return B_BAD_VALUE;
	
	/* Check if already exists */
	if (swap_file_find_by_path(path))
		return B_NAME_IN_USE;
	
	uint64_t diskBytes = 0;
	uint64_t poolLimit = 0;
	parse_path_params(path, &diskBytes, &poolLimit);
	
	ZramKernelDevice* dev = NULL;
	status_t status = zram_kernel_create(diskBytes, poolLimit, &dev);
	if (status != B_OK)
		return status;
	
	uint32_t pageCount = (uint32_t)(diskBytes / B_PAGE_SIZE);
	
	swap_file* swap = new(std::nothrow) swap_file;
	if (swap == NULL) {
		zram_kernel_destroy(dev);
		return B_NO_MEMORY;
	}
	
	swap->fd = -1;
	swap->vnode = NULL;
	swap->cookie = NULL;
	swap->zram = dev;
	swap->path = strdup(path);
	swap->bmp = radix_bitmap_create(pageCount);
	if (swap->bmp == NULL) {
		if (swap->path)
			free(swap->path);
		delete swap;
		zram_kernel_destroy(dev);
		return B_NO_MEMORY;
	}
	
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
	
	dprintf("swap_file_add: added zram swap at %s (%llu MB)\n", path, diskBytes / (1024*1024));
	return B_OK;
}

extern "C" status_t swap_file_delete(const char* path)
{
	if (!path)
		return B_BAD_VALUE;
	
	swap_file* swap = swap_file_find_by_path(path);
	if (!swap)
		return B_ENTRY_NOT_FOUND;
	
	if (!swap->zram)
		return B_BAD_VALUE;
	
	uint32_t pageCount = swap->last_slot - swap->first_slot;
	
	/* Remove from list */
	mutex_lock(&sSwapFileListLock);
	sSwapFileList.Remove(swap);
	sSwapFileCount--;
	mutex_unlock(&sSwapFileListLock);
	
	/* Update available space */
	mutex_lock(&sAvailableSwapSpaceLock);
	sAvailableSwapSpace -= (off_t)pageCount * B_PAGE_SIZE;
	mutex_unlock(&sAvailableSwapSpaceLock);
	
	/* Destroy zram device */
	zram_kernel_destroy(swap->zram);
	
	/* Free resources */
	if (swap->bmp)
		radix_bitmap_delete(swap->bmp);
	if (swap->path)
		free(swap->path);
	delete swap;
	
	dprintf("swap_file_delete: removed zram swap at %s\n", path);
	return B_OK;
}

/* Zram swap read/write wrappers */
static status_t zram_swap_read_wrapper(ZramKernelDevice* zram, uint32_t pos,
										const void* vecs, uint32_t j,
										uint32_t flags, size_t numBytes)
{
	swap_file* swap = swap_file_get(zram);
	if (!swap || !swap->zram)
		return B_BAD_VALUE;
	
	/* Call kernel API */
	generic_size_t numBytesOut = 0;
	status_t status = zram_swap_read(swap->zram, pos, (const generic_io_vec*)vecs, 
									 1, flags, &numBytesOut);
	return status;
}

static status_t zram_swap_write_wrapper(ZramKernelDevice* zram, uint32_t pos,
										const void* vecs, uint32_t j,
										uint32_t numBytes, uint32_t flags, void* callback)
{
	swap_file* swap = swap_file_get(zram);
	if (!swap || !swap->zram)
		return B_BAD_VALUE;
	
	generic_size_t numBytesOut = 0;
	status_t status = zram_swap_write(swap->zram, pos, (const generic_io_vec*)vecs,
									  1, flags, &numBytesOut);
	if (callback)
		/* callback handling would go here */
		(void)callback;
	return status;
}

static bool zram_kernel_can_accept_wrapper(ZramKernelDevice* zram)
{
	swap_file* swap = swap_file_get(zram);
	if (!swap || !swap->zram)
		return false;
	return zram_kernel_can_accept(swap->zram);
}

/**
 * Initialize swap from driver parameters
 */
static void swap_init_post_modules()
{
	/* Try to load settings - in Haiku this would use load_driver_settings */
	/* For now, just init debug */
}

/**
 * Dump swap information
 */
static void dump_swap_info()
{
	mutex_lock(&sSwapFileListLock);
	kprintf("Swap files: %d, total space: %lld MB\n", sSwapFileCount, 
			sAvailableSwapSpace / (1024 * 1024));
	
	for (SwapFileList::Iterator it = sSwapFileList.GetIterator();
			swap_file* swap = it.Next();) {
		if (swap->zram) {
			ZramStats stats;
			zram_kernel_get_stats(swap->zram, &stats);
			kprintf("  %s: zram compressed=%llu original=%llu ratio=%d%%\n",
					swap->path ? swap->path : "(unknown)",
					stats.compr_data_size, stats.orig_data_size,
					stats.orig_data_size ? (int)(stats.compr_data_size * 100 / stats.orig_data_size) : 0);
		}
	}
	mutex_unlock(&sSwapFileListLock);
}

/**
 * Initialize with debug
 */
static void swap_init_post_modules_debug()
{
	swap_init_post_modules();
	zram_kernel_debug_init();
	dprintf("hzram: swap_file_add/delete path-based API initialized\n");
}

static void swap_init_post_modules_full()
{
	swap_init_post_modules_debug();
}

/* For compatibility with existing codebase expectations */
static status_t swap_file_add_device(ZramKernelDevice* zram, uint64_t diskBytes, uint64_t poolLimit)
{
	(void)zram;
	return swap_zram_add(diskBytes, poolLimit);
}

static status_t swap_file_delete_device(ZramKernelDevice* zram)
{
	if (!zram)
		return B_BAD_VALUE;
	swap_file* swap = swap_file_get(zram);
	if (!swap || !swap->path)
		return B_BAD_VALUE;
	return swap_file_delete(swap->path);
}
