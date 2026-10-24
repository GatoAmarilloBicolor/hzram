/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		hzram developers
 */

#include "zram_core.h"

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <pthread.h>

/*
 * ============================================================================
 * SEGUIMIENTO DE MEMORIA
 * ============================================================================
 */

static std::atomic<int64_t> g_alloc_count{0};
static std::atomic<int64_t> g_alloc_bytes{0};

bool g_fail_chunk_alloc = false;


int64_t
host_alloc_count(void)
{
	return g_alloc_count.load();
}


int64_t
host_alloc_bytes(void)
{
	return g_alloc_bytes.load();
}

/*
 * ============================================================================
 * mem_alloc / mem_free
 * ============================================================================
 */

static void*
host_mem_alloc(size_t size)
{
	void* p = malloc(size);
	if (p != nullptr) {
		g_alloc_count++;
		g_alloc_bytes += (int64_t)size;
	}
	return p;
}


static void
host_mem_free(void* ptr)
{
	if (ptr != nullptr) {
		g_alloc_count--;
		free(ptr);
	}
}

/*
 * ============================================================================
 * chunk_alloc / chunk_free (alineado a 64 KiB)
 * ============================================================================
 */

static void*
host_chunk_alloc(size_t bytes)
{
	if (g_fail_chunk_alloc)
		return nullptr;

	void* p = nullptr;
#if defined(__APPLE__)
	if (posix_memalign(&p, 65536, bytes) != 0)
		return nullptr;
#else
	p = aligned_alloc(65536, bytes);
#endif
	if (p != nullptr) {
		g_alloc_count++;
		g_alloc_bytes += (int64_t)bytes;
		memset(p, 0, bytes);
	}
	return p;
}


static void
host_chunk_free(void* ptr, size_t bytes)
{
	if (ptr != nullptr) {
		g_alloc_count--;
		g_alloc_bytes -= (int64_t)bytes;
		free(ptr);
	}
}

/*
 * ============================================================================
 * Mutex (pthread)
 * ============================================================================
 */

static void*
host_mutex_new(void)
{
	pthread_mutex_t* m = (pthread_mutex_t*)malloc(sizeof(pthread_mutex_t));
	if (m == nullptr)
		return nullptr;
	pthread_mutex_init(m, nullptr);
	return m;
}


static void
host_mutex_lock(void* lock)
{
	pthread_mutex_lock((pthread_mutex_t*)lock);
}


static void
host_mutex_unlock(void* lock)
{
	pthread_mutex_unlock((pthread_mutex_t*)lock);
}


static void
host_mutex_delete(void* lock)
{
	if (lock == nullptr)
		return;
	pthread_mutex_destroy((pthread_mutex_t*)lock);
	free(lock);
}

/*
 * ============================================================================
 * Semáforo (pthread cond + mutex)
 * ============================================================================
 */

struct HostSem {
	pthread_mutex_t	mtx;
	pthread_cond_t	cond;
	int32_t			count;
};


static void*
host_sem_new(uint32_t count)
{
	HostSem* s = (HostSem*)malloc(sizeof(HostSem));
	if (s == nullptr)
		return nullptr;
	pthread_mutex_init(&s->mtx, nullptr);
	pthread_cond_init(&s->cond, nullptr);
	s->count = (int32_t)count;
	return s;
}


static void
host_sem_acquire(void* sem)
{
	HostSem* s = (HostSem*)sem;
	pthread_mutex_lock(&s->mtx);
	while (s->count <= 0)
		pthread_cond_wait(&s->cond, &s->mtx);
	s->count--;
	pthread_mutex_unlock(&s->mtx);
}


static void
host_sem_release(void* sem)
{
	HostSem* s = (HostSem*)sem;
	pthread_mutex_lock(&s->mtx);
	s->count++;
	pthread_cond_signal(&s->cond);
	pthread_mutex_unlock(&s->mtx);
}


static void
host_sem_delete(void* sem)
{
	if (sem == nullptr)
		return;
	HostSem* s = (HostSem*)sem;
	pthread_mutex_destroy(&s->mtx);
	pthread_cond_destroy(&s->cond);
	free(s);
}

/*
 * ============================================================================
 * Operaciones misceláneas
 * ============================================================================
 */

static uint32_t
host_cpu_count(void)
{
	return 2;
}


static ZramDevice* g_current_host_dev = nullptr;


void
host_set_active_device(ZramDevice* dev)
{
	g_current_host_dev = dev;
}


static void
host_request_refill(void* user)
{
	ZramDevice* dev = (ZramDevice*)user;
	if (dev == nullptr)
		dev = g_current_host_dev;
	if (dev != nullptr)
		zram_refill(dev);
}

/*
 * ============================================================================
 * Tabla de operaciones del host
 * ============================================================================
 */

extern "C" const ZramPlatformOps g_host_ops = {
	host_mem_alloc,
	host_mem_free,
	host_chunk_alloc,
	host_chunk_free,
	host_mutex_new,
	host_mutex_lock,
	host_mutex_unlock,
	host_mutex_delete,
	host_sem_new,
	host_sem_acquire,
	host_sem_release,
	host_sem_delete,
	host_cpu_count,
	host_request_refill,
	nullptr,
};
