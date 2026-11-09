/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		hzram developers
 */

#include "zram_kernel.h"
#include "zram_core.h"

#include <OS.h>
#include <debug.h>
#include <device_manager/IORequest.h>	/* B_PHYSICAL_IO_REQUEST */
#include <lock.h>
#include <smp.h>
#include <stdatomic.h>
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
	ZramDevice*			core;
	void*				pool_base;
	void*				pool_end;
	void*				pool_bump;
	void*				pool_lock;
	area_id				pool_area;
	uint64_t			disk_bytes;
	uint64_t			pool_limit;
	thread_id			refill_thread;
	sem_id				refill_sem;
	volatile bool		refill_done;
	ZramPlatformOps		ops;
	ZramKernelDevice*	next;
	_Atomic int32_t		ref_count;
};

static ZramKernelDevice* g_kernel_devices = NULL;
static void* g_devices_lock = NULL;
static _Atomic int g_devices_lock_init = 0;

/*
 * ============================================================================
 * OPERACIONES DE PLATAFORMA
 * ============================================================================
 */

static void*
kern_mem_alloc(size_t size)
{
	return malloc(size);
}


static void
kern_mem_free(void* ptr)
{
	free(ptr);
}


static void*
kern_chunk_alloc(void* user, size_t bytes)
{
	ZramKernelDevice* dev = (ZramKernelDevice*)user;
	if (dev == NULL)
		return NULL;

	mutex_lock((mutex*)dev->pool_lock);
	if ((uint8_t*)dev->pool_bump + bytes > (uint8_t*)dev->pool_end) {
		mutex_unlock((mutex*)dev->pool_lock);
		return NULL;
	}

	void* chunk = dev->pool_bump;
	dev->pool_bump = (uint8_t*)chunk + bytes;
	mutex_unlock((mutex*)dev->pool_lock);
	return chunk;
}


static void
kern_chunk_free(void* user, void* ptr, size_t bytes)
{
	(void)user;
	(void)ptr;
	(void)bytes;
	/* Bump allocator from wired area (create_area) - no per-chunk free.
	 * The entire pool area is freed on device destruction. */
}


static void*
kern_mutex_new(void)
{
	mutex* m = (mutex*)malloc(sizeof(mutex));
	if (m == NULL)
		return NULL;
	mutex_init(m, "zram");
	return m;
}


static void
kern_mutex_lock(void* lock)
{
	if (lock == NULL)
		return;
	mutex_lock((mutex*)lock);
}


static void
kern_mutex_unlock(void* lock)
{
	if (lock == NULL)
		return;
	mutex_unlock((mutex*)lock);
}


static void
kern_mutex_delete(void* lock)
{
	if (lock == NULL)
		return;
	mutex_destroy((mutex*)lock);
	free(lock);
}


static void*
kern_sem_new(uint32_t count)
{
	sem_id id = create_sem(count, "zram");
	return id >= 0 ? (void*)(intptr_t)id : NULL;
}


static void
kern_sem_acquire(void* sem)
{
	if (sem == NULL)
		return;
	acquire_sem((sem_id)(intptr_t)sem);
}


static void
kern_sem_release(void* sem)
{
	if (sem == NULL)
		return;
	release_sem((sem_id)(intptr_t)sem);
}


static void
kern_sem_delete(void* sem)
{
	if (sem == NULL)
		return;
	delete_sem((sem_id)(intptr_t)sem);
}


static uint32_t
kern_cpu_count(void)
{
	int32 n = smp_get_num_cpus();
	return n > 0 ? (uint32_t)n : 1;
}


static void
kern_request_refill(void* user)
{
	ZramKernelDevice* dev = (ZramKernelDevice*)user;
	if (dev != NULL && dev->refill_sem >= 0)
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
	NULL,
};

/*
 * ============================================================================
 * HILO DE REPOSICIÓN
 * ============================================================================
 */

static status_t
zram_refill_thread(void* data)
{
	ZramKernelDevice* dev = (ZramKernelDevice*)data;
	zram_kernel_dev_get(dev);

	while (!dev->refill_done) {
		acquire_sem(dev->refill_sem);
		if (dev->refill_done)
			break;
		zram_refill(dev->core);
	}

	zram_kernel_dev_put(dev);
	return B_OK;
}

/*
 * ============================================================================
 * CREACIÓN / DESTRUCCIÓN
 * ============================================================================
 */

status_t
zram_kernel_create(uint64_t diskBytes, uint64_t poolLimit,
	ZramKernelDevice** _out)
{
	if (_out == NULL)
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
	dev->ref_count = 1;

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

	dev->pool_lock = malloc(sizeof(mutex));
	if (dev->pool_lock == NULL) {
		delete_area(area);
		free(dev);
		return B_NO_MEMORY;
	}
	mutex_init((mutex*)dev->pool_lock, "zram pool");

	dev->refill_sem = create_sem(0, "zram_refill");
	if (dev->refill_sem < 0) {
		mutex_destroy((mutex*)dev->pool_lock);
		free(dev->pool_lock);
		delete_area(area);
		free(dev);
		return B_NO_MEMORY;
	}
	dev->refill_done = false;

	dev->ops = g_platform_ops;
	dev->ops.user = dev;

	ZramConfig cfg = { diskBytes, poolLimit, ZRAM_ENGINE_AUTO };
	int rc = zram_create(&cfg, &dev->ops, &dev->core);
	if (rc != 0) {
		delete_sem(dev->refill_sem);
		mutex_destroy((mutex*)dev->pool_lock);
		free(dev->pool_lock);
		delete_area(area);
		free(dev);
		return B_NO_MEMORY;
	}

	dev->refill_thread = spawn_kernel_thread(zram_refill_thread, "zram_refill",
		B_NORMAL_PRIORITY, dev);
	if (dev->refill_thread < 0) {
		delete_sem(dev->refill_sem);
		zram_destroy(dev->core);
		mutex_destroy((mutex*)dev->pool_lock);
		free(dev->pool_lock);
		delete_area(area);
		free(dev);
		return B_NO_MEMORY;
	}
	resume_thread(dev->refill_thread);

	/* Add to global list for KDL debugging - thread-safe lazy init */
	if (atomic_load_explicit(&g_devices_lock_init, memory_order_acquire) == 0) {
		void* lock = malloc(sizeof(mutex));
		if (lock) {
			mutex_init((mutex*)lock, "zram devices");
			if (atomic_compare_exchange_strong_explicit(&g_devices_lock_init, &(int){0}, 1,
				memory_order_acq_rel, memory_order_acquire)) {
				g_devices_lock = lock;
			} else {
				/* Another thread won - destroy our lock */
				mutex_destroy((mutex*)lock);
				free(lock);
			}
		}
	}
	if (g_devices_lock) {
		mutex_lock((mutex*)g_devices_lock);
		dev->next = g_kernel_devices;
		g_kernel_devices = dev;
		mutex_unlock((mutex*)g_devices_lock);
	}

	*_out = dev;
	return B_OK;
}


static inline void
zram_kernel_dev_get(ZramKernelDevice* dev)
{
	atomic_fetch_add_explicit(&dev->ref_count, 1, memory_order_acquire);
}

static void
zram_kernel_dev_put(ZramKernelDevice* dev)
{
	if (dev == NULL)
		return;
	
	if (atomic_fetch_sub_explicit(&dev->ref_count, 1, memory_order_release) > 1)
		return;
	
	/* Last reference - safe to destroy */
	atomic_thread_fence(memory_order_acquire);
	dev->refill_done = true;
	if (dev->refill_sem >= 0)
		release_sem(dev->refill_sem);

	if (dev->refill_thread >= 0) {
		status_t status;
		wait_for_thread(dev->refill_thread, &status);
	}

	if (dev->refill_sem >= 0)
		delete_sem(dev->refill_sem);

	if (dev->core != NULL)
		zram_destroy(dev->core);

	if (dev->pool_area >= 0)
		delete_area(dev->pool_area);

	if (dev->pool_lock != NULL) {
		mutex_destroy((mutex*)dev->pool_lock);
		free(dev->pool_lock);
	}

	if (g_devices_lock) {
		mutex_lock((mutex*)g_devices_lock);
		if (g_kernel_devices == dev) {
			g_kernel_devices = dev->next;
		} else {
			for (ZramKernelDevice* d = g_kernel_devices; d != NULL; d = d->next) {
				if (d->next == dev) {
					d->next = dev->next;
					break;
				}
			}
		}
		mutex_unlock((mutex*)g_devices_lock);
	}

	free(dev);
}

void
zram_kernel_destroy(ZramKernelDevice* dev)
{
	if (dev == NULL)
		return;

	/* C-002: Wait for all references to drop before destroying.
	 * Use semaphore instead of busy-wait to avoid CPU spinning. */
	while (atomic_load_explicit(&dev->ref_count, memory_order_acquire) > 0) {
		if (dev->refill_sem >= 0) {
			acquire_sem(dev->refill_sem);
		} else {
			snooze(1000); /* fallback */
		}
	}

	zram_kernel_dev_put(dev);
}

/*
 * ============================================================================
 * LECTURA / ESCRITURA POR VECTORES
 * ============================================================================
 */

static bool
zram_is_physical(uint32_t flags)
{
	return (flags & B_PHYSICAL_IO_REQUEST) != 0;
}


status_t
zram_swap_write(ZramKernelDevice* dev, off_t pos,
	const generic_io_vec* vecs, size_t count, uint32_t flags,
	generic_size_t* _numBytes)
{
	if (dev == NULL)
		return B_BAD_VALUE;
	if (_numBytes != NULL)
		*_numBytes = 0;
	if (count == 0)
		return B_OK;
	if (vecs == NULL || pos < 0 || pos % B_PAGE_SIZE != 0)
		return B_BAD_VALUE;

	zram_kernel_dev_get(dev);
	
	ZramCtx* ctx = zram_ctx_get(dev->core);
	if (ctx == NULL) {
		zram_kernel_dev_put(dev);
		return B_NO_MEMORY;
	}

	bool physical = zram_is_physical(flags);
	uint32_t slot = (uint32_t)(pos / B_PAGE_SIZE);
	generic_size_t total = 0;

	for (uint32_t i = 0; i < count; i++) {
		if (vecs[i].length % B_PAGE_SIZE != 0) {
			zram_ctx_put(dev->core, ctx);
			zram_kernel_dev_put(dev);
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
			if (rc != ZRAM_OK) {
				zram_ctx_put(dev->core, ctx);
				zram_kernel_dev_put(dev);
				return rc == ZRAM_E_NOMEM ? B_NO_MEMORY : B_ERROR;
			}

			slot++;
			total += B_PAGE_SIZE;
		}
	}

	zram_ctx_put(dev->core, ctx);
	if (_numBytes != NULL)
		*_numBytes = total;
	
	zram_kernel_dev_put(dev);
	return B_OK;
}


status_t
zram_swap_read(ZramKernelDevice* dev, off_t pos,
	const generic_io_vec* vecs, size_t count, uint32_t flags,
	generic_size_t* _numBytes)
{
	if (dev == NULL)
		return B_BAD_VALUE;
	if (_numBytes != NULL)
		*_numBytes = 0;
	if (count == 0)
		return B_OK;
	if (vecs == NULL || pos < 0 || pos % B_PAGE_SIZE != 0)
		return B_BAD_VALUE;

	zram_kernel_dev_get(dev);
	
	ZramCtx* ctx = zram_ctx_get(dev->core);
	if (ctx == NULL) {
		zram_kernel_dev_put(dev);
		return B_NO_MEMORY;
	}

	bool physical = zram_is_physical(flags);
	uint32_t slot = (uint32_t)(pos / B_PAGE_SIZE);
	generic_size_t total = 0;

	for (uint32_t i = 0; i < count; i++) {
		if (vecs[i].length % B_PAGE_SIZE != 0) {
			zram_ctx_put(dev->core, ctx);
			zram_kernel_dev_put(dev);
			return B_BAD_VALUE;
		}

		for (generic_size_t off = 0; off < vecs[i].length; off += B_PAGE_SIZE) {
			void* dst = (void*)(vecs[i].base + off);
			void* read_dst = dst;

			if (physical)
				read_dst = zram_ctx_page(ctx);

			int rc = zram_read_page(dev->core, ctx, slot, read_dst);
			if (rc != ZRAM_OK) {
				zram_ctx_put(dev->core, ctx);
				zram_kernel_dev_put(dev);
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
	if (_numBytes != NULL)
		*_numBytes = total;
	
	zram_kernel_dev_put(dev);
	return B_OK;
}

/*
 * ============================================================================
 * CONSULTAS
 * ============================================================================
 */

void
zram_kernel_free(ZramKernelDevice* dev, swap_addr_t firstSlot, uint32_t count)
{
	if (dev == NULL)
		return;
	zram_free_slots(dev->core, firstSlot, count);
}


bool
zram_kernel_can_accept(ZramKernelDevice* dev)
{
	if (dev == NULL)
		return false;
	return zram_can_accept(dev->core);
}


void
zram_kernel_get_stats(ZramKernelDevice* dev, ZramStats* out)
{
	if (dev == NULL)
		return;
	zram_get_stats(dev->core, out);
}


bool
zram_kernel_check_invariants(ZramKernelDevice* dev)
{
	if (dev == NULL)
		return false;
	return zram_check_invariants(dev->core);
}


/*
 * ============================================================================
 * ESTADÍSTICAS FORMATO POSIX (kernel-safe: usa kprintf)
 * ============================================================================
 */

static void
zram_kernel_fmt_bytes(char* buf, size_t size, uint64_t bytes)
{
	const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
	int unit = 0;
	double val = (double)bytes;
	while (val >= 1024.0 && unit < 4) {
		val /= 1024.0;
		unit++;
	}
	snprintf(buf, size, "%.2f %s", val, units[unit]);
}

static uint32_t
zram_kernel_calc_ratio(uint64_t orig, uint64_t stored)
{
	if (stored == 0)
		return orig > 0 ? 10000 : 100;
	return (uint32_t)((orig * 100) / stored);
}

int
zram_kernel_stats_snprintf(ZramKernelDevice* dev, char* buf, size_t size)
{
	if (dev == NULL || buf == NULL || size == 0)
		return -1;

	ZramStats st;
	zram_get_stats(dev->core, &st);

	ZramConfig cfg = {
		.disk_bytes = dev->disk_bytes,
		.pool_limit = dev->pool_limit,
	};

	/* Format config values first */
	char disk_size_buf[32];
	char pool_limit_buf[32];
	zram_kernel_fmt_bytes(disk_size_buf, sizeof(disk_size_buf), cfg.disk_bytes);
	zram_kernel_fmt_bytes(pool_limit_buf, sizeof(pool_limit_buf), cfg.pool_limit);

	/* Format aggregate values */
	char orig_buf[32], stored_buf[32], pool_buf[32];
	zram_kernel_fmt_bytes(orig_buf, sizeof(orig_buf), st.orig_bytes);
	zram_kernel_fmt_bytes(stored_buf, sizeof(stored_buf), st.stored_bytes);
	zram_kernel_fmt_bytes(pool_buf, sizeof(pool_buf), st.pool_bytes);

	uint64_t total_pages = st.pages_same + st.pages_raw + st.pages_lz4 + st.pages_empty;
	uint64_t active_pages = st.pages_same + st.pages_raw + st.pages_lz4;
	uint32_t ratio = zram_kernel_calc_ratio(st.orig_bytes, st.stored_bytes);

	double same_pct = active_pages > 0 ? (double)st.pages_same * 100.0 / active_pages : 0.0;
	double lz4_pct = active_pages > 0 ? (double)st.pages_lz4 * 100.0 / active_pages : 0.0;
	double raw_pct = active_pages > 0 ? (double)st.pages_raw * 100.0 / active_pages : 0.0;
	double active_pct = total_pages > 0 ? (double)active_pages * 100.0 / total_pages : 0.0;

	/* Format dynamic values with per-layer ratios */
	char same_saved_buf[32];
	zram_kernel_fmt_bytes(same_saved_buf, sizeof(same_saved_buf), st.pages_same * B_PAGE_SIZE);
	uint32_t same_ratio = st.pages_same > 0 ? 10000 : 100;  /* 100:1 for SAME (0 stored bytes) */

	char lz4_stored_buf[32] = "0 B";
	uint32_t lz4_ratio = 100;
	if (st.pages_lz4 > 0) {
		uint64_t lz4_stored = st.stored_bytes - st.pages_raw * B_PAGE_SIZE;
		zram_kernel_fmt_bytes(lz4_stored_buf, sizeof(lz4_stored_buf), lz4_stored);
		lz4_ratio = zram_kernel_calc_ratio(st.pages_lz4 * B_PAGE_SIZE, lz4_stored);
	}

	char raw_stored_buf[32];
	zram_kernel_fmt_bytes(raw_stored_buf, sizeof(raw_stored_buf), st.pages_raw * B_PAGE_SIZE);

	int len = snprintf(buf, size,
		"zram_stats:\n"
		"  config:\n"
		"    disk_size:        %s\n"
		"    pool_limit:       %s\n"
		"  pages:\n"
		"    total_slots:      %" PRIu64 "\n"
		"    active_pages:     %" PRIu64 " (%.2f%%)\n"
		"    empty_pages:      %" PRIu64 "\n"
		"  compression_layers:\n"
		"    SAME (identical): %" PRIu64 " pages (%5.2f%%)  ratio: %u%%  saved: %s\n"
		"    LZ4  (compressed): %" PRIu64 " pages (%5.2f%%)  ratio: %u%%  stored: %s\n"
		"    RAW  (incompress): %" PRIu64 " pages (%5.2f%%)  ratio: 100%%  stored: %s\n"
		"  aggregate:\n"
		"    orig_bytes:       %s\n"
		"    stored_bytes:     %s\n"
		"    compression_ratio: %u%%\n"
		"    pool_allocated:   %s\n"
		"  class_distribution:\n",
		disk_size_buf, pool_limit_buf,
		total_pages,
		active_pages, active_pct,
		st.pages_empty,
		st.pages_same, same_pct, same_ratio, same_saved_buf,
		st.pages_lz4, lz4_pct, lz4_ratio, lz4_stored_buf,
		st.pages_raw, raw_pct, raw_stored_buf,
		orig_buf, stored_buf, ratio, pool_buf);

	/* Append class distribution */
	for (uint32_t c = 0; c < ZRAM_CLASS_COUNT && len < (int)size; c++) {
		if (st.class_live[c] > 0 || st.class_free[c] > 0) {
			len += snprintf(buf + len, size - len,
				"    class %2u (%4u B): live=%5u free=%5u\n",
				c, kZramClassSize[c], st.class_live[c], st.class_free[c]);
		}
	}

	if (len < (int)size) {
		snprintf(buf + len, size - len,
			"  reserver: spans=%u/%u\n",
			st.reserver_count, st.reserver_capacity);
	}

	return len;
}

/* Global console lock for stats output */
static mutex g_stats_console_lock = MUTEX_INITIALIZER("zram stats console");

int
zram_kernel_stats_kprintf(ZramKernelDevice* dev)
{
	if (dev == NULL)
		return -1;

	char buf[2048];
	int len = zram_kernel_stats_snprintf(dev, buf, sizeof(buf));
	if (len < 0)
		return -1;

	mutex_lock(&g_stats_console_lock);
	kprintf("%s", buf);
	mutex_unlock(&g_stats_console_lock);
	return len;
}


/*
 * ============================================================================
 * COMANDO DE DEPURACIÓN (KDL)
 * ============================================================================
 */

static int
zram_debug_command(int argc, char** argv)
{
	(void)argc;
	(void)argv;

	if (g_devices_lock == NULL)
		return 0;

	mutex_lock((mutex*)g_devices_lock);
	for (ZramKernelDevice* dev = g_kernel_devices; dev != NULL;
			dev = dev->next) {
		ZramStats st;
		zram_get_stats(dev->core, &st);

		uint64_t totalPages = dev->disk_bytes / B_PAGE_SIZE;
		uint64_t activePages = st.pages_same + st.pages_raw + st.pages_lz4;

		kprintf("zram device %p:\n", dev);
	kprintf("  Configuration:\n");
	kprintf("    disk_bytes:         %" B_PRIu64 " B (%" B_PRIu64 " MB, %" B_PRIu64 " slots)\n",
		dev->disk_bytes, dev->disk_bytes / (1024 * 1024), totalPages);
	kprintf("    pool_limit:         %" B_PRIu64 " B (%" B_PRIu64 " MB, %" B_PRIu32 " chunks)\n",
			dev->pool_limit, dev->pool_limit / (1024 * 1024), st.chunk_capacity);
		kprintf("    pool_allocated:     %" B_PRIu64 " B (%" B_PRIu64 " MB, %" B_PRIu32 " chunks)\n",
			st.pool_bytes, st.pool_bytes / (1024 * 1024), st.chunk_count);
		kprintf("    reserver_spans:     %" B_PRIu32 " / %" B_PRIu32 "\n",
			st.reserver_count, st.reserver_capacity);

		kprintf("  Page States Distribution:\n");
		if (activePages > 0) {
			kprintf("    SAME (identical):   %" B_PRIu64 " (%3" B_PRIu64 "%%) -> cost: 0 B (saved: %" B_PRIu64 " B)\n",
				st.pages_same, (st.pages_same * 100) / activePages,
				st.pages_same * B_PAGE_SIZE);
			kprintf("    LZ4  (compressed):  %" B_PRIu64 " (%3" B_PRIu64 "%%) -> stored: %" B_PRIu64 " B\n",
				st.pages_lz4, (st.pages_lz4 * 100) / activePages,
				st.stored_bytes - (st.pages_raw * B_PAGE_SIZE));
			kprintf("    RAW  (uncompress):  %" B_PRIu64 " (%3" B_PRIu64 "%%) -> stored: %" B_PRIu64 " B\n",
				st.pages_raw, (st.pages_raw * 100) / activePages,
				st.pages_raw * B_PAGE_SIZE);
		} else {
			kprintf("    No active pages stored.\n");
		}
		kprintf("    EMPTY (free slots): %" B_PRIu64 " (%" B_PRIu64 "%% of device)\n",
			st.pages_empty, totalPages > 0 ? (st.pages_empty * 100) / totalPages : 0);

		kprintf("  Memory & Efficiency:\n");
		kprintf("    orig_uncompressed:  %" B_PRIu64 " B\n", st.orig_bytes);
		kprintf("    stored_in_ram:      %" B_PRIu64 " B\n", st.stored_bytes);
		if (st.stored_bytes > 0 && st.orig_bytes > 0) {
			uint64_t ratioInt = st.orig_bytes / st.stored_bytes;
			uint64_t ratioDec = ((st.orig_bytes * 100) / st.stored_bytes) % 100;
			uint64_t savingsPct = ((st.orig_bytes - st.stored_bytes) * 100) / st.orig_bytes;
			kprintf("    net_memory_saved:   %" B_PRIu64 " B (%" B_PRIu64 "%% savings)\n",
				st.orig_bytes - st.stored_bytes, savingsPct);
			kprintf("    compression_ratio:  %" B_PRIu64 ".%02" B_PRIu64 ":1\n",
				ratioInt, ratioDec);
		}

		kprintf("  Size Classes (live / free objects):\n");
		for (uint32_t c = 0; c < ZRAM_CLASS_COUNT; c++) {
			kprintf("    class %2" B_PRIu32 " (%4" B_PRIu32 " B): %6" B_PRIu32 " live, %6" B_PRIu32 " free\n",
				c, kZramClassSize[c], st.class_live[c], st.class_free[c]);
		}

		kprintf("  Operations & Errors:\n");
		kprintf("    writes:             %" B_PRIu64 "\n", st.writes);
		kprintf("    reads:              %" B_PRIu64 "\n", st.reads);
		kprintf("    frees:              %" B_PRIu64 "\n", st.frees);
		kprintf("    write_nomem:        %" B_PRIu64 "\n", st.write_nomem);
		kprintf("    read_corrupt:       %" B_PRIu64 "\n", st.read_corrupt);
		kprintf("    read_empty:         %" B_PRIu64 "\n", st.read_empty);
	}
	mutex_unlock((mutex*)g_devices_lock);

	return 0;
}


void
zram_kernel_debug_init(void)
{
	add_debugger_command_etc("zram", zram_debug_command,
		"Show zram state distribution, memory and statistics", "zram\n", 0);
}
