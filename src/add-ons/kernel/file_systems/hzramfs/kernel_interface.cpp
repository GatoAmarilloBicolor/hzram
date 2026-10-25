/*
 * Copyright 2026, hzram developers.
 * Distributed under the terms of the MIT License.
 *
 * hzramfs - a virtual file system whose only payload is a compressed
 * swap file.  It is mounted without a backing device:
 *
 *     mkdir /hzram
 *     mount -t hzramfs -p "size=512M,pool=128M" /hzram
 *     echo up > /hzram/.control
 *     cat /hzram/.stats
 *     echo down > /hzram/.control
 *
 * The kernel has no userland swapon()/swapoff(), so this add-on calls the
 * exported kernel swap_file_add()/swap_file_delete() from a kernel thread.
 * No kernel patch and no Haiku repository change is required.
 */

#include "hzramfs.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "zram_core.h"
#ifdef __cplusplus
}
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>

#include <KernelExport.h>
#include <NodeMonitor.h>
#include <fs_info.h>
#include <fs_volume.h>
#include <lock.h>
#include <module.h>
#include <smp.h>
#include <thread.h>
#include <vfs.h>
#include <vm/vm_page.h>


/* Kernel-internal functions, exported through kernel_versions. */
extern "C" status_t swap_file_add(const char* path);
extern "C" status_t swap_file_delete(const char* path);

extern fs_vnode_ops gHzramVnodeOps;
extern fs_volume_ops gHzramVolumeOps;


#define HZRAM_SLOT_SIZE		((uint64)ZRAM_PAGE_SIZE)
#define HZRAM_POOL_MIN		(1ULL * 1024 * 1024)
#define HZRAM_TRACE(x)		/* dprintf x */


static struct hzram_volume* sVolume = NULL;
static mutex sVolumeLock = MUTEX_INITIALIZER("hzramfs volumes");


/* --------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------ */

static uint64
parse_number(const char* string, uint64 defaultValue)
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
		case '%':
		{
			uint64 memory = (uint64)vm_page_num_pages() * B_PAGE_SIZE;
			value = memory / 100 * value;
			break;
		}
		default:
			break;
	}

	return value;
}


static uint64
physical_memory(void)
{
	return (uint64)vm_page_num_pages() * B_PAGE_SIZE;
}


static uint64
round_up(uint64 value, uint64 alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}


static uint32
now_seconds(void)
{
	return (uint32)real_time_clock();
}


/* --------------------------------------------------------------------------
 * zram platform glue (single volume; the kernel swap is global anyway)
 * ------------------------------------------------------------------------ */

static void*
hzram_mem_alloc(size_t size)
{
	return malloc(size);
}


static void
hzram_mem_free(void* pointer)
{
	free(pointer);
}


static void*
hzram_chunk_alloc(size_t bytes)
{
	struct hzram_volume* volume = sVolume;
	if (volume == NULL)
		return NULL;

	if (volume->poolBump + bytes > volume->poolEnd)
		return NULL;

	void* chunk = (void*)volume->poolBump;
	volume->poolBump += bytes;
	return chunk;
}


static void
hzram_chunk_free(void* pointer, size_t bytes)
{
	(void)pointer;
	(void)bytes;
}


static void*
hzram_mutex_new(void)
{
	mutex* lock = (mutex*)malloc(sizeof(mutex));
	if (lock == NULL)
		return NULL;

	mutex_init(lock, "hzramfs zram");
	return lock;
}


static void
hzram_mutex_lock(void* lock)
{
	if (lock == NULL)
		return;
	mutex_lock((mutex*)lock);
}


static void
hzram_mutex_unlock(void* lock)
{
	if (lock == NULL)
		return;
	mutex_unlock((mutex*)lock);
}


static void
hzram_mutex_delete(void* lock)
{
	if (lock == NULL)
		return;
	mutex_destroy((mutex*)lock);
	free(lock);
}


static void*
hzram_sem_new(uint32 count)
{
	sem_id sem = create_sem(count, "hzramfs zram");
	return sem >= 0 ? (void*)(intptr_t)sem : NULL;
}


static void
hzram_sem_acquire(void* sem)
{
	if (sem == NULL)
		return;
	acquire_sem((sem_id)(intptr_t)sem);
}


static void
hzram_sem_release(void* sem)
{
	if (sem == NULL)
		return;
	release_sem((sem_id)(intptr_t)sem);
}


static void
hzram_sem_delete(void* sem)
{
	if (sem == NULL)
		return;
	delete_sem((sem_id)(intptr_t)sem);
}


static uint32
hzram_cpu_count(void)
{
	int32 count = smp_get_num_cpus();
	return count > 0 ? (uint32)count : 1;
}


static void
hzram_request_refill(void* user)
{
	struct hzram_volume* volume = (struct hzram_volume*)user;
	if (volume != NULL && volume->refillSem >= 0)
		release_sem(volume->refillSem);
}


static const ZramPlatformOps sPlatformOps = {
	hzram_mem_alloc,
	hzram_mem_free,
	hzram_chunk_alloc,
	hzram_chunk_free,
	hzram_mutex_new,
	hzram_mutex_lock,
	hzram_mutex_unlock,
	hzram_mutex_delete,
	hzram_sem_new,
	hzram_sem_acquire,
	hzram_sem_release,
	hzram_sem_delete,
	hzram_cpu_count,
	hzram_request_refill,
	NULL
};


static status_t
hzram_refill_thread(void* data)
{
	struct hzram_volume* volume = (struct hzram_volume*)data;

	while (!volume->refillDone) {
		acquire_sem(volume->refillSem);
		if (volume->refillDone)
			break;
		zram_refill(volume->device);
	}

	return B_OK;
}


/* --------------------------------------------------------------------------
 * Volume
 * ------------------------------------------------------------------------ */

static void
hzram_node_init(struct hzram_node* node, ino_t id, enum hzram_node_kind kind,
	mode_t mode, uint64 size)
{
	memset(node, 0, sizeof(*node));
	node->id = id;
	node->kind = kind;
	node->mode = mode;
	node->nlink = kind == HZRAM_NODE_DIRECTORY ? 2 : 1;
	node->uid = 0;
	node->gid = 0;
	node->size = size;
	node->crtime = now_seconds();
	node->mtime = node->crtime;
}


struct hzram_volume*
hzram_volume_create(fs_volume* fsVolume, const char* args, status_t* _error)
{
	status_t error = B_OK;
	uint64 swapSize = HZRAM_DEFAULT_SWAP;
	uint64 poolLimit = 0;
	char name[B_FILE_NAME_LENGTH] = "hzram";
	bool sizeGiven = false;
	bool poolGiven = false;

	if (args != NULL) {
		char buffer[256];
		strlcpy(buffer, args, sizeof(buffer));

		char* save = NULL;
		for (char* token = strtok_r(buffer, ", ", &save); token != NULL;
				token = strtok_r(NULL, ", ", &save)) {
			char* value = strchr(token, '=');
			if (value == NULL)
				continue;
			*value++ = '\0';

			if (strcmp(token, "size") == 0) {
				swapSize = parse_number(value, HZRAM_DEFAULT_SWAP);
				sizeGiven = swapSize != 0;
			} else if (strcmp(token, "pool") == 0) {
				poolLimit = parse_number(value, 0);
				poolGiven = poolLimit != 0;
			} else if (strcmp(token, "name") == 0) {
				strlcpy(name, value, sizeof(name));
			}
		}
	}

	if (!sizeGiven)
		swapSize = HZRAM_DEFAULT_SWAP;
	if (swapSize < HZRAM_MIN_SWAP)
		swapSize = HZRAM_MIN_SWAP;
	if (swapSize > HZRAM_MAX_SWAP)
		swapSize = HZRAM_MAX_SWAP;
	swapSize = round_up(swapSize, B_PAGE_SIZE);

	if (swapSize / ZRAM_PAGE_SIZE > UINT32_MAX) {
		*_error = B_BAD_VALUE;
		return NULL;
	}

	if (!poolGiven)
		poolLimit = swapSize / 4;
	if (poolLimit < HZRAM_MIN_POOL)
		poolLimit = HZRAM_MIN_POOL;
	poolLimit = round_up(poolLimit, HZRAM_POOL_ALIGN);

	uint64 memory = physical_memory();
	if (memory > 0 && poolLimit > memory / 2)
		poolLimit = round_up(memory / 2, HZRAM_POOL_ALIGN);
	if (sizeof(void*) == 4 && poolLimit > 1024ULL * 1024 * 1024)
		poolLimit = 1024ULL * 1024 * 1024;
	if (poolLimit < HZRAM_POOL_ALIGN)
		poolLimit = HZRAM_POOL_ALIGN;

	void* pool = NULL;
	area_id area = -1;
	struct hzram_volume* volume
		= (struct hzram_volume*)calloc(1, sizeof(struct hzram_volume));
	if (volume == NULL) {
		*_error = B_NO_MEMORY;
		return NULL;
	}

	volume->fsVolume = fsVolume;
	volume->poolArea = -1;
	volume->refillSem = -1;
	volume->refillThread = -1;
	volume->diskBytes = swapSize;
	volume->poolLimit = poolLimit;
	strlcpy(volume->name, name, sizeof(volume->name));
	mutex_init(&volume->lock, "hzramfs volume");

	mutex_lock(&sVolumeLock);
	if (sVolume != NULL) {
		mutex_unlock(&sVolumeLock);
		error = B_BUSY;
		goto fail;
	}
	sVolume = volume;
	mutex_unlock(&sVolumeLock);

	/* Reserve the compressed pool.  B_FULL_LOCK keeps the swap write path
	 * free of blocking allocations.  Fall back to a smaller pool if the
	 * requested one cannot be locked. */
	for (;;) {
		area = create_area("hzramfs pool", &pool, B_ANY_KERNEL_ADDRESS,
			(size_t)poolLimit, B_FULL_LOCK, B_READ_AREA | B_WRITE_AREA);
		if (area >= 0)
			break;
		if (poolLimit <= HZRAM_POOL_MIN)
			break;
		poolLimit = round_up(poolLimit / 2, HZRAM_POOL_ALIGN);
		if (poolLimit < HZRAM_POOL_MIN)
			poolLimit = HZRAM_POOL_MIN;
	}

	if (area < 0) {
		error = B_NO_MEMORY;
		goto fail;
	}

	volume->poolArea = area;
	volume->pool = pool;
	volume->poolLimit = poolLimit;
	volume->poolBase = (addr_t)pool;
	volume->poolBump = (addr_t)pool;
	volume->poolEnd = (addr_t)pool + (addr_t)poolLimit;

	volume->refillSem = create_sem(0, "hzramfs refill");
	if (volume->refillSem < 0) {
		error = B_NO_MEMORY;
		goto fail;
	}

	ZramConfig config;
	config.disk_bytes = volume->diskBytes;
	config.pool_limit = volume->poolLimit;

	if (zram_create(&config, &sPlatformOps, &volume->device) != ZRAM_OK) {
		error = B_NO_MEMORY;
		goto fail;
	}

	volume->refillThread = spawn_kernel_thread(hzram_refill_thread,
		"hzramfs refill", B_NORMAL_PRIORITY, volume);
	if (volume->refillThread < 0) {
		error = B_NO_MEMORY;
		goto fail;
	}
	resume_thread(volume->refillThread);

	hzram_node_init(&volume->root, HZRAM_ROOT_ID, HZRAM_NODE_DIRECTORY,
		S_IFDIR | 0755, 0);
	hzram_node_init(&volume->swap, HZRAM_SWAP_ID, HZRAM_NODE_SWAP,
		S_IFREG | 0600, volume->diskBytes);
	hzram_node_init(&volume->control, HZRAM_CONTROL_ID, HZRAM_NODE_CONTROL,
		S_IFREG | 0600, 0);
	hzram_node_init(&volume->stats, HZRAM_STATS_ID, HZRAM_NODE_STATS,
		S_IFREG | 0444, 0);

	*_error = B_OK;
	return volume;

fail:
	hzram_volume_destroy(volume);
	*_error = error;
	return NULL;
}


void
hzram_volume_destroy(struct hzram_volume* volume)
{
	if (volume == NULL)
		return;

	if (volume->refillThread >= 0) {
		volume->refillDone = true;
		release_sem(volume->refillSem);
		status_t status;
		wait_for_thread(volume->refillThread, &status);
	}

	if (volume->refillSem >= 0)
		delete_sem(volume->refillSem);

	if (volume->device != NULL)
		zram_destroy(volume->device);

	if (volume->poolArea >= 0)
		delete_area(volume->poolArea);

	mutex_lock(&sVolumeLock);
	if (sVolume == volume)
		sVolume = NULL;
	mutex_unlock(&sVolumeLock);

	mutex_destroy(&volume->lock);
	free(volume);
}


static struct hzram_node*
hzram_node_for_id(struct hzram_volume* volume, ino_t id)
{
	switch (id) {
		case HZRAM_ROOT_ID:
			return &volume->root;
		case HZRAM_SWAP_ID:
			return &volume->swap;
		case HZRAM_CONTROL_ID:
			return &volume->control;
		case HZRAM_STATS_ID:
			return &volume->stats;
		default:
			return NULL;
	}
}


static int
access_for_open_mode(int openMode)
{
	switch (openMode & O_RWMASK) {
		case O_WRONLY:
			return W_OK;
		case O_RDWR:
			return R_OK | W_OK;
		default:
			return R_OK;
	}
}


/* --------------------------------------------------------------------------
 * Page I/O helpers
 * ------------------------------------------------------------------------ */

static status_t
hzram_swap_write_pages(struct hzram_volume* volume, off_t pos,
	const iovec* vecs, size_t count, size_t* _numBytes)
{
	if (pos < 0)
		return B_BAD_VALUE;

	uint64 fileSize = volume->swap.size;
	if ((uint64)pos >= fileSize)
		return B_BAD_VALUE;

	ZramCtx* context = zram_ctx_get(volume->device);
	if (context == NULL)
		return B_NO_MEMORY;

	uint8* scratch = (uint8*)zram_ctx_page(context);
	uint64 offset = (uint64)pos;
	uint64 remaining = fileSize - offset;
	size_t total = 0;

	for (size_t i = 0; i < count && remaining > 0; i++) {
		const uint8* source = (const uint8*)vecs[i].iov_base;
		size_t length = vecs[i].iov_len;
		if (length > remaining)
			length = (size_t)remaining;

		while (length > 0) {
			uint32 slot = (uint32)(offset / ZRAM_PAGE_SIZE);
			uint32 pageOffset = (uint32)(offset % ZRAM_PAGE_SIZE);
			size_t chunk = ZRAM_PAGE_SIZE - pageOffset;
			if (chunk > length)
				chunk = length;

			int result;
			if (pageOffset == 0 && chunk == ZRAM_PAGE_SIZE) {
				result = zram_write_page(volume->device, context, slot,
					source);
			} else {
				result = zram_read_page(volume->device, slot, scratch);
				if (result == ZRAM_OK) {
					memcpy(scratch + pageOffset, source, chunk);
					result = zram_write_page(volume->device, context, slot,
						scratch);
				}
			}

			if (result == ZRAM_E_NOMEM) {
				zram_ctx_put(volume->device, context);
				return B_NO_MEMORY;
			}
			if (result != ZRAM_OK) {
				zram_ctx_put(volume->device, context);
				return B_ERROR;
			}

			offset += chunk;
			source += chunk;
			length -= chunk;
			remaining -= chunk;
			total += chunk;
		}
	}

	zram_ctx_put(volume->device, context);
	volume->swap.mtime = now_seconds();

	if (_numBytes != NULL)
		*_numBytes = total;
	return B_OK;
}


static status_t
hzram_swap_read_pages(struct hzram_volume* volume, off_t pos,
	const iovec* vecs, size_t count, size_t* _numBytes)
{
	if (pos < 0)
		return B_BAD_VALUE;

	uint64 fileSize = volume->swap.size;
	if ((uint64)pos >= fileSize)
		return B_BAD_VALUE;

	ZramCtx* context = zram_ctx_get(volume->device);
	if (context == NULL)
		return B_NO_MEMORY;

	uint8* scratch = (uint8*)zram_ctx_page(context);
	uint64 offset = (uint64)pos;
	uint64 remaining = fileSize - offset;
	size_t total = 0;

	for (size_t i = 0; i < count && remaining > 0; i++) {
		uint8* destination = (uint8*)vecs[i].iov_base;
		size_t length = vecs[i].iov_len;
		if (length > remaining)
			length = (size_t)remaining;

		while (length > 0) {
			uint32 slot = (uint32)(offset / ZRAM_PAGE_SIZE);
			uint32 pageOffset = (uint32)(offset % ZRAM_PAGE_SIZE);
			size_t chunk = ZRAM_PAGE_SIZE - pageOffset;
			if (chunk > length)
				chunk = length;

			int result = zram_read_page(volume->device, slot, scratch);
			if (result != ZRAM_OK) {
				zram_ctx_put(volume->device, context);
				return B_ERROR;
			}

			memcpy(destination, scratch + pageOffset, chunk);

			offset += chunk;
			destination += chunk;
			length -= chunk;
			remaining -= chunk;
			total += chunk;
		}
	}

	zram_ctx_put(volume->device, context);

	if (_numBytes != NULL)
		*_numBytes = total;
	return B_OK;
}


/* --------------------------------------------------------------------------
 * Swap activation
 * ------------------------------------------------------------------------ */

enum hzram_swap_action {
	HZRAM_SWAP_ADD,
	HZRAM_SWAP_DELETE,
	HZRAM_SWAP_RESET
};


struct hzram_swap_command {
	struct hzram_volume*	volume;
	enum hzram_swap_action	action;
	sem_id					done;
	status_t				status;
};


static void
hzram_reset_swap_locked(struct hzram_volume* volume)
{
	uint32 slotCount = (uint32)(volume->diskBytes / ZRAM_PAGE_SIZE);
	zram_free_slots(volume->device, 0, slotCount);
	volume->swap.size = volume->diskBytes;
	volume->swap.mtime = now_seconds();
}


static status_t
hzram_swap_command_thread(void* data)
{
	struct hzram_swap_command* command
		= (struct hzram_swap_command*)data;
	struct hzram_volume* volume = command->volume;
	status_t status = B_OK;

	if (command->action == HZRAM_SWAP_RESET) {
		mutex_lock(&volume->lock);
		if (volume->swapActive)
			status = B_BUSY;
		else
			hzram_reset_swap_locked(volume);
		mutex_unlock(&volume->lock);

		command->status = status;
		release_sem(command->done);
		return B_OK;
	}

	char path[1024];
	status = vfs_entry_ref_to_path(volume->fsVolume->id, HZRAM_ROOT_ID,
		HZRAM_SWAP_NAME, true, path, sizeof(path));

	if (status == B_OK && command->action == HZRAM_SWAP_ADD) {
		mutex_lock(&volume->lock);
		if (volume->swapActive)
			status = B_BUSY;
		else if (volume->swap.size != volume->diskBytes)
			hzram_reset_swap_locked(volume);
		mutex_unlock(&volume->lock);

		if (status == B_OK)
			status = swap_file_add(path);

		mutex_lock(&volume->lock);
		volume->swapActive = status == B_OK;
		mutex_unlock(&volume->lock);
	} else if (status == B_OK) {
		/* shrink to zero is allowed even while active: swap_file_delete()
		 * only truncates after it verified that all slots are free. */
		status = swap_file_delete(path);

		mutex_lock(&volume->lock);
		if (status == B_OK)
			volume->swapActive = false;
		mutex_unlock(&volume->lock);
	}

	command->status = status;
	release_sem(command->done);
	return B_OK;
}


static status_t
hzram_run_swap_command(struct hzram_volume* volume,
	enum hzram_swap_action action)
{
	struct hzram_swap_command command;
	command.volume = volume;
	command.action = action;
	command.status = B_ERROR;

	command.done = create_sem(0, "hzramfs swap command");
	if (command.done < 0)
		return B_NO_MEMORY;

	thread_id thread = spawn_kernel_thread(hzram_swap_command_thread,
		"hzramfs swap", B_NORMAL_PRIORITY, &command);
	if (thread < 0) {
		delete_sem(command.done);
		return B_NO_MEMORY;
	}
	resume_thread(thread);

	status_t result = acquire_sem(command.done);
	if (result == B_OK)
		result = command.status;

	status_t threadStatus;
	wait_for_thread(thread, &threadStatus);
	delete_sem(command.done);
	return result;
}


/* --------------------------------------------------------------------------
 * Statistics
 * ------------------------------------------------------------------------ */

void
hzram_volume_get_stats_string(struct hzram_volume* volume, char* buffer,
	size_t size)
{
	ZramStats stats;
	memset(&stats, 0, sizeof(stats));
	zram_get_stats(volume->device, &stats);

	uint64 active = stats.pages_same + stats.pages_lz4 + stats.pages_raw;
	uint64 ratio = stats.stored_bytes > 0
		? stats.orig_bytes * 100 / stats.stored_bytes : 0;
	uint64 savings = stats.orig_bytes > 0
		? (stats.orig_bytes - stats.stored_bytes) * 100 / stats.orig_bytes : 0;

	snprintf(buffer, size,
		"volume: %s\n"
		"swap size: %llu bytes\n"
		"swap active: %s\n"
		"disk slots: %llu\n"
		"pool limit: %llu bytes\n"
		"pool used: %llu bytes\n"
		"chunks: %lu / %lu\n"
		"pages active: %llu (same %llu, lz4 %llu, raw %llu)\n"
		"pages empty: %llu\n"
		"orig bytes: %llu\n"
		"stored bytes: %llu\n"
		"compression: %llu.%02llu:1 (%llu%% saved)\n"
		"writes: %llu reads: %llu frees: %llu\n"
		"errors: write_nomem %llu read_corrupt %llu read_empty %llu\n",
		volume->name,
		(unsigned long long)volume->swap.size,
		volume->swapActive ? "yes" : "no",
		(unsigned long long)(volume->diskBytes / ZRAM_PAGE_SIZE),
		(unsigned long long)volume->poolLimit,
		(unsigned long long)stats.pool_bytes,
		(unsigned long)stats.chunk_count,
		(unsigned long)stats.chunk_capacity,
		(unsigned long long)active,
		(unsigned long long)stats.pages_same,
		(unsigned long long)stats.pages_lz4,
		(unsigned long long)stats.pages_raw,
		(unsigned long long)stats.pages_empty,
		(unsigned long long)stats.orig_bytes,
		(unsigned long long)stats.stored_bytes,
		(unsigned long long)(ratio / 100),
		(unsigned long long)(ratio % 100),
		(unsigned long long)savings,
		(unsigned long long)stats.writes,
		(unsigned long long)stats.reads,
		(unsigned long long)stats.frees,
		(unsigned long long)stats.write_nomem,
		(unsigned long long)stats.read_corrupt,
		(unsigned long long)stats.read_empty);
}


/* --------------------------------------------------------------------------
 * Vnode operations
 * ------------------------------------------------------------------------ */

static status_t
hzram_lookup(fs_volume* _volume, fs_vnode* _directory, const char* name,
	ino_t* _id)
{
	struct hzram_volume* volume
		= (struct hzram_volume*)_volume->private_volume;
	struct hzram_node* node
		= (struct hzram_node*)_directory->private_node;

	if (node == NULL || node->kind != HZRAM_NODE_DIRECTORY)
		return B_NOT_A_DIRECTORY;

	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		*_id = HZRAM_ROOT_ID;
		return B_OK;
	}

	if (strcmp(name, HZRAM_SWAP_NAME) == 0)
		*_id = HZRAM_SWAP_ID;
	else if (strcmp(name, HZRAM_CONTROL_NAME) == 0)
		*_id = HZRAM_CONTROL_ID;
	else if (strcmp(name, HZRAM_STATS_NAME) == 0)
		*_id = HZRAM_STATS_ID;
	else
		return B_ENTRY_NOT_FOUND;

	return B_OK;
}


static status_t
hzram_get_vnode(fs_volume* _volume, ino_t id, fs_vnode* vnode, int* _type,
	uint32* _flags, bool reenter)
{
	struct hzram_volume* volume
		= (struct hzram_volume*)_volume->private_volume;
	struct hzram_node* node = hzram_node_for_id(volume, id);
	if (node == NULL)
		return B_ENTRY_NOT_FOUND;

	vnode->private_node = node;
	vnode->ops = &gHzramVnodeOps;
	*_type = node->mode;
	*_flags = 0;
	return B_OK;
}


static status_t
hzram_put_vnode(fs_volume* volume, fs_vnode* vnode, bool reenter)
{
	return B_OK;
}


static status_t
hzram_remove_vnode(fs_volume* volume, fs_vnode* vnode, bool reenter)
{
	return B_NOT_ALLOWED;
}


static status_t
hzram_read_fs_info(fs_volume* _volume, struct fs_info* info)
{
	struct hzram_volume* volume
		= (struct hzram_volume*)_volume->private_volume;
	ZramStats stats;
	memset(&stats, 0, sizeof(stats));
	zram_get_stats(volume->device, &stats);

	info->dev = volume->fsVolume->id;
	info->root = HZRAM_ROOT_ID;
	info->flags = 0;
	info->block_size = ZRAM_PAGE_SIZE;
	info->io_size = ZRAM_PAGE_SIZE;
	info->total_blocks = volume->diskBytes / ZRAM_PAGE_SIZE;
	info->free_blocks = stats.pages_empty;
	info->total_nodes = 4;
	info->free_nodes = 0;
	strlcpy(info->device_name, "hzram", sizeof(info->device_name));
	strlcpy(info->volume_name, volume->name, sizeof(info->volume_name));
	strlcpy(info->fsh_name, HZRAM_FS_NAME, sizeof(info->fsh_name));
	return B_OK;
}


static status_t
hzram_write_fs_info(fs_volume* _volume, const struct fs_info* info, uint32 mask)
{
	struct hzram_volume* volume
		= (struct hzram_volume*)_volume->private_volume;

	if ((mask & FS_WRITE_FSINFO_NAME) != 0) {
		mutex_lock(&volume->lock);
		strlcpy(volume->name, info->volume_name, sizeof(volume->name));
		mutex_unlock(&volume->lock);
	}

	return B_OK;
}


static status_t
hzram_sync(fs_volume* volume)
{
	return B_OK;
}


static status_t
hzram_read_stat(fs_volume* _volume, fs_vnode* _node, struct stat* stat)
{
	struct hzram_volume* volume
		= (struct hzram_volume*)_volume->private_volume;
	struct hzram_node* node = (struct hzram_node*)_node->private_node;

	memset(stat, 0, sizeof(*stat));
	stat->st_dev = volume->fsVolume->id;
	stat->st_ino = node->id;
	stat->st_mode = node->mode;
	stat->st_nlink = node->nlink;
	stat->st_uid = node->uid;
	stat->st_gid = node->gid;
	stat->st_size = node->size;
	stat->st_blksize = ZRAM_PAGE_SIZE;
	stat->st_blocks = (node->size + 511) / 512;
	stat->st_atim.tv_sec = node->mtime;
	stat->st_mtim.tv_sec = node->mtime;
	stat->st_ctim.tv_sec = node->mtime;
	stat->st_crtim.tv_sec = node->crtime;
	return B_OK;
}


static status_t
hzram_write_stat(fs_volume* _volume, fs_vnode* _node,
	const struct stat* stat, uint32 mask)
{
	struct hzram_volume* volume
		= (struct hzram_volume*)_volume->private_volume;
	struct hzram_node* node = (struct hzram_node*)_node->private_node;

	if ((mask & B_STAT_SIZE) != 0) {
		if (node->kind != HZRAM_NODE_SWAP)
			return node->kind == HZRAM_NODE_DIRECTORY
				? B_IS_A_DIRECTORY : B_BAD_VALUE;

		uint64 newSize = (uint64)stat->st_size;
		if (newSize > volume->diskBytes)
			newSize = volume->diskBytes;

		mutex_lock(&volume->lock);
		if (volume->swapActive && newSize != 0) {
			mutex_unlock(&volume->lock);
			return B_BUSY;
		}
		if (newSize == 0)
			zram_free_slots(volume->device, 0,
				(uint32)(volume->diskBytes / ZRAM_PAGE_SIZE));
		node->size = newSize;
		node->mtime = now_seconds();
		mutex_unlock(&volume->lock);
	}

	if ((mask & B_STAT_MODE) != 0 && node->kind != HZRAM_NODE_DIRECTORY)
		node->mode = (node->mode & S_IFMT) | (stat->st_mode & 07777);

	return B_OK;
}


static status_t
hzram_access(fs_volume* _volume, fs_vnode* _node, int mode)
{
	struct hzram_node* node = (struct hzram_node*)_node->private_node;
	return check_access_permissions(mode, node->mode, node->gid, node->uid);
}


static status_t
hzram_open(fs_volume* _volume, fs_vnode* _node, int openMode, void** _cookie)
{
	struct hzram_node* node = (struct hzram_node*)_node->private_node;
	status_t status;

	if (node->kind == HZRAM_NODE_DIRECTORY) {
		if ((openMode & O_RWMASK) != O_RDONLY)
			return B_IS_A_DIRECTORY;
	}

	status = check_access_permissions(access_for_open_mode(openMode),
		node->mode, node->gid, node->uid);
	if (status != B_OK)
		return status;

	if ((openMode & O_TRUNC) != 0 && node->kind == HZRAM_NODE_SWAP)
		return B_NOT_ALLOWED;

	*_cookie = NULL;
	return B_OK;
}


static status_t
hzram_close(fs_volume* volume, fs_vnode* vnode, void* cookie)
{
	return B_OK;
}


static status_t
hzram_free_cookie(fs_volume* volume, fs_vnode* vnode, void* cookie)
{
	return B_OK;
}


static status_t
hzram_read(fs_volume* _volume, fs_vnode* _node, void* cookie, off_t pos,
	void* buffer, size_t* _length)
{
	struct hzram_volume* volume
		= (struct hzram_volume*)_volume->private_volume;
	struct hzram_node* node = (struct hzram_node*)_node->private_node;
	size_t length = *_length;

	if (pos < 0)
		return B_BAD_VALUE;

	if (node->kind == HZRAM_NODE_SWAP) {
		if ((uint64)pos >= volume->swap.size) {
			*_length = 0;
			return B_OK;
		}
		iovec vec;
		vec.iov_base = buffer;
		vec.iov_len = length;
		return hzram_swap_read_pages(volume, pos, &vec, 1, _length);
	}

	if (node->kind == HZRAM_NODE_CONTROL) {
		static const char help[] =
			"up\n"
			"down\n"
			"reset\n";
		size_t helpLength = sizeof(help) - 1;
		if ((uint64)pos >= helpLength) {
			*_length = 0;
			return B_OK;
		}
		if (length > helpLength - (size_t)pos)
			length = helpLength - (size_t)pos;
		memcpy(buffer, help + pos, length);
		*_length = length;
		return B_OK;
	}

	if (node->kind == HZRAM_NODE_STATS) {
		char text[2048];
		hzram_volume_get_stats_string(volume, text, sizeof(text));
		size_t textLength = strlen(text);
		if ((uint64)pos >= textLength) {
			*_length = 0;
			return B_OK;
		}
		if (length > textLength - (size_t)pos)
			length = textLength - (size_t)pos;
		memcpy(buffer, text + pos, length);
		*_length = length;
		return B_OK;
	}

	return B_IS_A_DIRECTORY;
}


static status_t
hzram_write(fs_volume* _volume, fs_vnode* _node, void* cookie, off_t pos,
	const void* buffer, size_t* _length)
{
	struct hzram_volume* volume
		= (struct hzram_volume*)_volume->private_volume;
	struct hzram_node* node = (struct hzram_node*)_node->private_node;
	size_t length = *_length;

	if (pos < 0)
		return B_BAD_VALUE;

	if (node->kind == HZRAM_NODE_SWAP) {
		iovec vec;
		vec.iov_base = const_cast<void*>(buffer);
		vec.iov_len = length;
		return hzram_swap_write_pages(volume, pos, &vec, 1, _length);
	}

	if (node->kind != HZRAM_NODE_CONTROL)
		return B_NOT_ALLOWED;

	if (pos != 0)
		return B_BAD_VALUE;

	char command[64];
	if (length > sizeof(command) - 1)
		length = sizeof(command) - 1;
	memcpy(command, buffer, length);
	command[length] = '\0';

	char* end = command + length;
	while (end > command && (end[-1] == '\n' || end[-1] == '\r'
			|| end[-1] == ' ' || end[-1] == '\t'))
		end--;
	*end = '\0';

	char* start = command;
	while (*start == ' ' || *start == '\t')
		start++;

	status_t status;
	if (strcmp(start, "up") == 0)
		status = hzram_run_swap_command(volume, HZRAM_SWAP_ADD);
	else if (strcmp(start, "down") == 0)
		status = hzram_run_swap_command(volume, HZRAM_SWAP_DELETE);
	else if (strcmp(start, "reset") == 0)
		status = hzram_run_swap_command(volume, HZRAM_SWAP_RESET);
	else
		return B_BAD_VALUE;

	if (status == B_OK)
		*_length = length;
	return status;
}


static status_t
hzram_read_pages(fs_volume* _volume, fs_vnode* _node, void* cookie, off_t pos,
	const iovec* vecs, size_t count, size_t* _numBytes)
{
	struct hzram_volume* volume
		= (struct hzram_volume*)_volume->private_volume;
	struct hzram_node* node = (struct hzram_node*)_node->private_node;

	if (node->kind != HZRAM_NODE_SWAP)
		return B_UNSUPPORTED;

	if ((uint64)pos >= volume->swap.size) {
		*_numBytes = 0;
		return B_OK;
	}

	return hzram_swap_read_pages(volume, pos, vecs, count, _numBytes);
}


static status_t
hzram_write_pages(fs_volume* _volume, fs_vnode* _node, void* cookie,
	off_t pos, const iovec* vecs, size_t count, size_t* _numBytes)
{
	struct hzram_volume* volume
		= (struct hzram_volume*)_volume->private_volume;
	struct hzram_node* node = (struct hzram_node*)_node->private_node;

	if (node->kind != HZRAM_NODE_SWAP)
		return B_UNSUPPORTED;

	return hzram_swap_write_pages(volume, pos, vecs, count, _numBytes);
}


static status_t
hzram_fsync(fs_volume* volume, fs_vnode* vnode, bool dataOnly)
{
	return B_OK;
}


/* Directory operations. */
struct hzram_dir_cookie {
	int32	index;
};


static const struct {
	const char*	name;
	ino_t		id;
} sDirectoryEntries[] = {
	{ ".", HZRAM_ROOT_ID },
	{ "..", HZRAM_ROOT_ID },
	{ HZRAM_SWAP_NAME, HZRAM_SWAP_ID },
	{ HZRAM_CONTROL_NAME, HZRAM_CONTROL_ID },
	{ HZRAM_STATS_NAME, HZRAM_STATS_ID },
};

static const int32 kDirectoryEntryCount
	= sizeof(sDirectoryEntries) / sizeof(sDirectoryEntries[0]);


static status_t
hzram_open_dir(fs_volume* _volume, fs_vnode* _node, void** _cookie)
{
	struct hzram_node* node = (struct hzram_node*)_node->private_node;
	if (node->kind != HZRAM_NODE_DIRECTORY)
		return B_NOT_A_DIRECTORY;

	struct hzram_dir_cookie* cookie
		= (struct hzram_dir_cookie*)malloc(sizeof(*cookie));
	if (cookie == NULL)
		return B_NO_MEMORY;

	cookie->index = 0;
	*_cookie = cookie;
	return B_OK;
}


static status_t
hzram_close_dir(fs_volume* volume, fs_vnode* vnode, void* cookie)
{
	return B_OK;
}


static status_t
hzram_free_dir_cookie(fs_volume* volume, fs_vnode* vnode, void* cookie)
{
	free(cookie);
	return B_OK;
}


static status_t
hzram_read_dir(fs_volume* _volume, fs_vnode* _node, void* _cookie,
	struct dirent* entry, size_t size, uint32* _num)
{
	struct hzram_volume* volume
		= (struct hzram_volume*)_volume->private_volume;
	struct hzram_dir_cookie* cookie = (struct hzram_dir_cookie*)_cookie;
	uint32 maxCount = *_num;
	uint32 count = 0;

	while (count < maxCount && cookie->index < kDirectoryEntryCount) {
		const char* name = sDirectoryEntries[cookie->index].name;
		size_t nameLength = strlen(name);
		size_t recordLength = offsetof(struct dirent, d_name) + nameLength + 1;

		if (recordLength > size) {
			if (count == 0)
				return B_BUFFER_OVERFLOW;
			break;
		}

		memset(entry, 0, recordLength);
		entry->d_dev = volume->fsVolume->id;
		entry->d_ino = sDirectoryEntries[cookie->index].id;
		entry->d_reclen = recordLength;
		memcpy(entry->d_name, name, nameLength + 1);

		entry = (struct dirent*)((uint8*)entry + recordLength);
		size -= recordLength;
		cookie->index++;
		count++;
	}

	*_num = count;
	return B_OK;
}


static status_t
hzram_rewind_dir(fs_volume* volume, fs_vnode* vnode, void* cookie)
{
	((struct hzram_dir_cookie*)cookie)->index = 0;
	return B_OK;
}


/* --------------------------------------------------------------------------
 * Ops tables
 * ------------------------------------------------------------------------ */

fs_vnode_ops gHzramVnodeOps = {
	/* vnode operations */
	&hzram_lookup,		// lookup
	NULL,				// get_vnode_name
	&hzram_put_vnode,	// put_vnode
	&hzram_remove_vnode,// remove_vnode

	/* VM file access */
	NULL,				// can_page
	&hzram_read_pages,	// read_pages
	&hzram_write_pages,	// write_pages

	/* asynchronous I/O */
	NULL,				// io
	NULL,				// cancel_io

	/* cache file access */
	NULL,				// get_file_map

	/* common operations */
	NULL,				// ioctl
	NULL,				// set_flags
	NULL,				// select
	NULL,				// deselect
	&hzram_fsync,		// fsync

	/* symbolic links */
	NULL,				// read_symlink
	NULL,				// create_symlink

	/* hard links */
	NULL,				// link
	NULL,				// unlink
	NULL,				// rename

	&hzram_access,		// access
	&hzram_read_stat,	// read_stat
	&hzram_write_stat,	// write_stat
	NULL,				// preallocate

	/* file operations */
	NULL,				// create
	&hzram_open,		// open
	&hzram_close,		// close
	&hzram_free_cookie,	// free_cookie
	&hzram_read,		// read
	&hzram_write,		// write

	/* directory operations */
	NULL,				// create_dir
	NULL,				// remove_dir
	&hzram_open_dir,	// open_dir
	&hzram_close_dir,	// close_dir
	&hzram_free_dir_cookie,	// free_dir_cookie
	&hzram_read_dir,	// read_dir
	&hzram_rewind_dir,	// rewind_dir

	/* attribute directory operations */
	NULL, NULL, NULL, NULL, NULL,

	/* attribute operations */
	NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL,

	/* special nodes */
	NULL,				// create_special_node
	NULL				// get_super_vnode
};


fs_volume_ops gHzramVolumeOps = {
	&hzram_unmount,
	&hzram_read_fs_info,
	&hzram_write_fs_info,
	&hzram_sync,
	&hzram_get_vnode,

	/* index directory & index operations */
	NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL,

	/* query operations */
	NULL, NULL, NULL, NULL, NULL,

	/* support for FS layers */
	NULL, NULL, NULL
};


/* --------------------------------------------------------------------------
 * Mount / unmount
 * ------------------------------------------------------------------------ */

status_t
hzram_mount(fs_volume* volume, const char* device, uint32 flags,
	const char* args, ino_t* _rootID)
{
	if ((flags & B_MOUNT_READ_ONLY) != 0)
		return B_BAD_VALUE;

	status_t error;
	struct hzram_volume* hzram = hzram_volume_create(volume, args, &error);
	if (hzram == NULL)
		return error;

	volume->private_volume = hzram;
	volume->ops = &gHzramVolumeOps;
	*_rootID = HZRAM_ROOT_ID;
	return B_OK;
}


status_t
hzram_unmount(fs_volume* _volume)
{
	struct hzram_volume* volume
		= (struct hzram_volume*)_volume->private_volume;

	if (volume == NULL)
		return B_BAD_VALUE;

	mutex_lock(&volume->lock);
	bool active = volume->swapActive;
	mutex_unlock(&volume->lock);

	if (active)
		return B_BUSY;

	hzram_volume_destroy(volume);
	_volume->private_volume = NULL;
	return B_OK;
}


/* --------------------------------------------------------------------------
 * Module interface
 * ------------------------------------------------------------------------ */

static status_t
hzram_std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
			return B_OK;
		case B_MODULE_UNINIT:
			return B_OK;
		default:
			return B_ERROR;
	}
}


static file_system_module_info sHzramModuleInfo = {
	{
		HZRAM_FS_MODULE_NAME,
		0,
		hzram_std_ops
	},

	HZRAM_FS_NAME,
	HZRAM_FS_PRETTY_NAME,
	0,

	/* scanning */
	NULL,	// identify_partition
	NULL,	// scan_partition
	NULL,	// free_identify_partition_cookie
	NULL,	// free_partition_content_cookie

	/* general operations */
	&hzram_mount,

	/* capability querying */
	NULL,	// get_supported_operations

	NULL,	// validate_resize
	NULL,	// validate_move
	NULL,	// validate_set_content_name
	NULL,	// validate_set_content_parameters
	NULL,	// validate_initialize

	/* shadow partition modification */
	NULL,	// shadow_changed

	/* writing */
	NULL,	// defragment
	NULL,	// repair
	NULL,	// resize
	NULL,	// move
	NULL,	// set_content_name
	NULL,	// set_content_parameters
	NULL,	// initialize
	NULL	// uninitialize
};


module_info* modules[] = {
	(module_info*)&sHzramModuleInfo,
	NULL
};
