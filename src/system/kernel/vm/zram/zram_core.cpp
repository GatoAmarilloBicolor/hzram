/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		hzram developers
 */

#include "zram_core.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#if defined(__aarch64__) || defined(__arm__)
#  if !defined(ZRAM_TESTING) && !defined(__APPLE__)
#    include <arm_neon.h>
#  endif
#endif
#if defined(__x86_64__) || defined(__i386__)
#  include <emmintrin.h>
#endif

#include "lz4/lz4.h"
#include "lz4/lz4hc.h"

/* ============================================================================
 * xxHash64 - fast 64-bit hash for same-page detection
 * Single-file, no dependencies, public domain
 * ============================================================================
 */
static const uint64_t PRIME64_1 = 0x9E3779B185EBCA87ULL;
static const uint64_t PRIME64_2 = 0xC2B2AE3D27D4EB4FULL;
static const uint64_t PRIME64_3 = 0x165667B19E3779F9ULL;
static const uint64_t PRIME64_4 = 0x85EBCA77C2B2AE63ULL;
static const uint64_t PRIME64_5 = 0x27D4EB2F165667C5ULL;

static inline uint64_t XXH_rotl64(uint64_t x, int r) {
	return (x << r) | (x >> (64 - r));
}

static inline uint64_t XXH_read64(const void* ptr) {
	uint64_t val;
	memcpy(&val, ptr, sizeof(val));
	return val;
}

static uint64_t XXH64_round(uint64_t acc, uint64_t input) {
	acc += input * PRIME64_2;
	acc = XXH_rotl64(acc, 31);
	acc *= PRIME64_1;
	return acc;
}

static uint64_t XXH64(const void* input, size_t len, uint64_t seed) {
	const uint8_t* p = (const uint8_t*)input;
	const uint8_t* end = p + len;
	uint64_t hash;

	if (len >= 32) {
		uint64_t v1 = seed + PRIME64_1 + PRIME64_2;
		uint64_t v2 = seed + PRIME64_2;
		uint64_t v3 = seed + PRIME64_3;
		uint64_t v4 = seed + PRIME64_4;

		do {
			v1 = XXH64_round(v1, XXH_read64(p)); p += 8;
			v2 = XXH64_round(v2, XXH_read64(p)); p += 8;
			v3 = XXH64_round(v3, XXH_read64(p)); p += 8;
			v4 = XXH64_round(v4, XXH_read64(p)); p += 8;
		} while (p <= end - 32);

		hash = XXH_rotl64(v1, 1) + XXH_rotl64(v2, 7) + XXH_rotl64(v3, 12) + XXH_rotl64(v4, 18);
		hash = XXH64_round(hash, v1 * PRIME64_2);
		hash = XXH64_round(hash, v2 * PRIME64_3);
		hash = XXH64_round(hash, v3 * PRIME64_4);
		hash = XXH64_round(hash, v4 * PRIME64_5);
	} else {
		hash = seed + PRIME64_5;
	}

	hash += (uint64_t)len;

	while (p <= end - 8) {
		uint64_t k1 = XXH64_round(0, XXH_read64(p));
		hash ^= k1;
		hash = XXH_rotl64(hash, 27) * PRIME64_1 + PRIME64_4;
		p += 8;
	}

	if (p + 4 <= end) {
		hash ^= (uint64_t)(*(const uint32_t*)p) * PRIME64_1;
		hash = XXH_rotl64(hash, 23) * PRIME64_2 + PRIME64_3;
		p += 4;
	}

	while (p < end) {
		hash ^= (*p++) * PRIME64_5;
		hash = XXH_rotl64(hash, 11) * PRIME64_1;
	}

	hash ^= hash >> 33;
	hash *= PRIME64_2;
	hash ^= hash >> 29;
	hash *= PRIME64_3;
	hash ^= hash >> 32;

	return hash;
}

/*
 * ============================================================================
 * ESTRUCTURAS INTERNAS
 * ============================================================================
 */

/* Per-CPU cache for lock-free allocation (H-005) */

struct ZramCpuCache {
	void**		entries;	/* dynamically allocated array */
	uint32_t	count;
	uint32_t	capacity;
};

struct ZramClass {
	uint32_t	size;			/* tamaño de objeto */
	uint32_t	free_count;		/* objetos en la lista libre global */
	uint32_t	live_count;		/* objetos entregados a slots */
	void*		free_list;		/* lista intrusiva: primeros 8 bytes = next */
	struct ZramCpuCache*	cpu_caches;	/* array de per-CPU caches [ctx_count] */
	uint32_t	cpu_cache_count;	/* número de CPUs (ctx_count) */
};

struct ZramReserver {
	uint32_t	count;
	uint32_t	capacity;
	void**		spans;			/* pila de spans sin tallar */
};

struct alignas(64) ZramCtx {
	LZ4_stream_t		state;
	LZ4_streamHC_t		state_hc;
	uint8_t			out_buf[ZRAM_PAGE_SIZE];
	uint8_t			page[ZRAM_PAGE_SIZE];
	uint32_t		cpu_index;	/* índice de CPU/context para per-CPU cache */
};

struct ZramDevice {
	const ZramPlatformOps*	ops;

	uint32_t				slot_count;
	ZramSlot*				slots;

	ZramCtx**				ctxs;
	bool*					ctx_in_use;
	uint32_t				ctx_count;

	uint64_t				disk_bytes;
	uint64_t				pool_limit;

	/* Per-CPU counter arrays (one per context/CPU) - eliminates cache-line ping-pong */
	struct ZramPerCpuCounters*	percpu_counters;

	/* Aggregated counters (updated by zram_aggregate_counters) */
	alignas(64) struct {
		_Atomic uint32_t	reserver_count;
		_Atomic uint64_t	pool_bytes;
		_Atomic uint64_t	pages_same;
		_Atomic uint64_t	pages_raw;
		_Atomic uint64_t	pages_lz4;
		_Atomic uint64_t	orig_bytes;
		_Atomic uint64_t	stored_bytes;
		_Atomic uint64_t	writes;
		_Atomic uint64_t	reads;
		_Atomic uint64_t	frees;
		_Atomic uint64_t	write_nomem;
		_Atomic uint64_t	read_corrupt;
		_Atomic uint64_t	read_empty;
	} counters;

	void**					chunks;
	uint32_t				chunk_count;
	uint32_t				chunk_capacity;

	struct ZramClass		classes[ZRAM_CLASS_COUNT];
	struct ZramReserver		reserver;

	/* Dynamic stripe count (cpu_count * 8, min 64, max 1024) */
	uint32_t				stripe_count;
	void**					stripe_locks;

	void*					class_mutexes[ZRAM_CLASS_COUNT];
	void*					reserver_lock;
	void*					ctx_mutex;
	void*					ctx_sem;

	bool					destroyed;

	/* Engine selection (H-007) */
	ZramEngine				engine;
};

/*
 * ============================================================================
 * ASIGNADOR POR CLASES
 * ============================================================================
 */

static void
zram_classes_init(ZramDevice* dev)
{
	for (uint32_t c = 0; c < ZRAM_CLASS_COUNT; c++) {
		dev->classes[c].size = kZramClassSize[c];
		dev->classes[c].free_count = 0;
		dev->classes[c].live_count = 0;
		dev->classes[c].free_list = NULL;
		dev->classes[c].cpu_caches = NULL;
		dev->classes[c].cpu_cache_count = 0;
	}
}

static int
zram_cpu_caches_init(ZramDevice* dev)
{
	/* Dynamic cache size: min(16, ctx_count) to avoid over-allocation */
	uint32_t cache_size = dev->ctx_count < 16 ? dev->ctx_count : 16;
	
	for (uint32_t c = 0; c < ZRAM_CLASS_COUNT; c++) {
		dev->classes[c].cpu_cache_count = dev->ctx_count;
		dev->classes[c].cpu_caches = (struct ZramCpuCache*)dev->ops->mem_alloc(
			dev->ctx_count * sizeof(struct ZramCpuCache));
		if (dev->classes[c].cpu_caches == NULL)
			return ZRAM_E_NOMEM;
		memset(dev->classes[c].cpu_caches, 0, dev->ctx_count * sizeof(struct ZramCpuCache));
		
		/* Allocate entries arrays for each CPU cache */
		for (uint32_t cpu = 0; cpu < dev->ctx_count; cpu++) {
			dev->classes[c].cpu_caches[cpu].entries = (void**)dev->ops->mem_alloc(
				cache_size * sizeof(void*));
			if (dev->classes[c].cpu_caches[cpu].entries == NULL)
				return ZRAM_E_NOMEM;
			dev->classes[c].cpu_caches[cpu].capacity = cache_size;
		}
	}
	return ZRAM_OK;
}

static void
zram_cpu_caches_fini(ZramDevice* dev)
{
	for (uint32_t c = 0; c < ZRAM_CLASS_COUNT; c++) {
		if (dev->classes[c].cpu_caches != NULL) {
			/* Free entries arrays */
			for (uint32_t cpu = 0; cpu < dev->classes[c].cpu_cache_count; cpu++) {
				if (dev->classes[c].cpu_caches[cpu].entries != NULL) {
					dev->ops->mem_free(dev->classes[c].cpu_caches[cpu].entries);
				}
			}
			dev->ops->mem_free(dev->classes[c].cpu_caches);
			dev->classes[c].cpu_caches = NULL;
			dev->classes[c].cpu_cache_count = 0;
		}
	}
}

/* Flush all per-CPU caches to global free lists. Caller must not hold any class_mutex. */
static void
zram_cpu_caches_flush(ZramDevice* dev)
{
	for (uint32_t c = 0; c < ZRAM_CLASS_COUNT; c++) {
		if (dev->classes[c].cpu_caches == NULL)
			continue;
		
		dev->ops->mutex_lock(dev->class_mutexes[c]);
		for (uint32_t cpu = 0; cpu < dev->classes[c].cpu_cache_count; cpu++) {
			struct ZramCpuCache* cache = &dev->classes[c].cpu_caches[cpu];
			while (cache->count > 0) {
				void* obj = cache->entries[--cache->count];
				*(void**)obj = dev->classes[c].free_list;
				dev->classes[c].free_list = obj;
				dev->classes[c].free_count++;
				/* live_count was already decremented when object was added to per-CPU cache */
			}
		}
		dev->ops->mutex_unlock(dev->class_mutexes[c]);
	}
}


/* Talla un span en objetos de la clase c y los apila en su lista libre.
 * El llamador debe tener class_mutexes[c] tomado. */
static void
zram_carve_span(ZramDevice* dev, void* span, uint32_t c)
{
	uint32_t size = kZramClassSize[c];
	uint32_t count = ZRAM_SPAN_BYTES / size;
	uint8_t* base = (uint8_t*)span;

	for (uint32_t i = 0; i < count; i++) {
		void* obj = base + i * size;
		*(void**)obj = dev->classes[c].free_list;
		dev->classes[c].free_list = obj;
	}
	dev->classes[c].free_count += count;
}


/* Toma un span de la reserva. El llamador debe tener reserver_lock. */
static void*
zram_reserver_pop(ZramDevice* dev)
{
	if (dev->reserver.count == 0)
		return NULL;
	return dev->reserver.spans[--dev->reserver.count];
}


static void
zram_reserver_push(ZramDevice* dev, void* span)
{
	if (dev->reserver.count >= dev->reserver.capacity) {
		dev->ops->chunk_free(dev->ops->user, span, ZRAM_SPAN_BYTES);
		dev->counters.pool_bytes -= ZRAM_SPAN_BYTES;
		return;
	}
	dev->reserver.spans[dev->reserver.count++] = span;
	dev->counters.reserver_count = dev->reserver.count;
}


/* Reserva un objeto de la clase c. Devuelve NULL si no hay memoria.
 * El llamador debe tener class_mutexes[c] tomado.
 * Puede tomar reserver_lock internamente (orden: class_mutex -> reserver_lock). */
static void*
zram_class_alloc(ZramDevice* dev, uint32_t c, uint32_t cpu)
{
	struct ZramCpuCache* cache = &dev->classes[c].cpu_caches[cpu];
	
	/* Try per-CPU cache first (lock-free for own CPU) */
	if (cache->count > 0) {
		void* obj = cache->entries[--cache->count];
		dev->classes[c].live_count++;
		return obj;
	}
	
	/* Fallback to global free list (requires class mutex, already held) */
	if (dev->classes[c].free_list == NULL) {
		/* Try to get a span from reserver */
		dev->ops->mutex_lock(dev->reserver_lock);
		void* span = zram_reserver_pop(dev);
		dev->ops->mutex_unlock(dev->reserver_lock);
		
		if (span != NULL) {
			zram_carve_span(dev, span, c);
		} else {
			/* Reserver empty - trigger async refill */
			dev->ops->request_refill(dev->ops->user);
			return NULL;
		}
	}

	void* obj = dev->classes[c].free_list;
	dev->classes[c].free_list = *(void**)obj;
	dev->classes[c].free_count--;
	dev->classes[c].live_count++;
	return obj;
}


/* Devuelve un objeto a su clase. El llamador debe tener class_mutexes[c]. */
static void
zram_class_free(ZramDevice* dev, uint32_t c, void* obj, uint32_t cpu)
{
	struct ZramCpuCache* cache = &dev->classes[c].cpu_caches[cpu];
	
	/* Try per-CPU cache first */
	if (cache->count < cache->capacity) {
		cache->entries[cache->count++] = obj;
		dev->classes[c].live_count--;
		return;
	}
	
	/* Per-CPU cache full, flush to global free list (requires class mutex) */
	*(void**)obj = dev->classes[c].free_list;
	dev->classes[c].free_list = obj;
	dev->classes[c].free_count++;
	dev->classes[c].live_count--;
	
	/* If class is empty and we have spare spans, return one to reserver */
	if (dev->classes[c].live_count == 0 && dev->classes[c].free_count >= ZRAM_SPAN_BYTES / kZramClassSize[c]) {
		dev->ops->mutex_lock(dev->reserver_lock);
		void* span = dev->classes[c].free_list;
		dev->classes[c].free_list = *(void**)span;
		dev->classes[c].free_count -= ZRAM_SPAN_BYTES / kZramClassSize[c];
		zram_reserver_push(dev, span);
		dev->ops->mutex_unlock(dev->reserver_lock);
	}
}


/* Lookup O(1) sin ramas: mapea tamaño a clase (0-255→0, 256-383→0, 384-511→1, etc) */
static const uint8_t kSizeToClass[17] = {
	0, 0, 0, 1, 2, 3, 3, 4, 4, 5, 6, 7, 7, 8, 9, 9, 10
};

static inline uint32_t
zram_class_for_size(uint32_t n)
{
	uint32_t idx = (n + 255u) >> 8;
	return idx < 17u ? (uint32_t)kSizeToClass[idx] : ZRAM_CLASS_RAW;
}

/*
 * ============================================================================
 * REPOSICIÓN
 * ============================================================================
 */

int
zram_refill(ZramDevice* dev)
{
	if (dev == NULL || dev->destroyed)
		return ZRAM_E_INVAL;

	dev->ops->mutex_lock(dev->reserver_lock);

	while (dev->reserver.count < ZRAM_RESERVE_TARGET
			&& dev->counters.pool_bytes + ZRAM_CHUNK_BYTES <= dev->pool_limit) {
		void* chunk = dev->ops->chunk_alloc(dev->ops->user, ZRAM_CHUNK_BYTES);
		if (chunk == NULL)
			break;

		bool all_pushed = true;
		for (uint32_t i = 0; i < ZRAM_CHUNK_BYTES / ZRAM_SPAN_BYTES; i++) {
			void* span = (uint8_t*)chunk + i * ZRAM_SPAN_BYTES;
			if (dev->reserver.count >= dev->reserver.capacity) {
				/* Reserver full - free remaining spans in this chunk */
				dev->ops->chunk_free(dev->ops->user, span, ZRAM_SPAN_BYTES);
				all_pushed = false;
			} else {
				dev->reserver.spans[dev->reserver.count++] = span;
			}
		}

		if (all_pushed) {
			if (dev->chunk_count < dev->chunk_capacity)
				dev->chunks[dev->chunk_count++] = chunk;
			dev->counters.pool_bytes += ZRAM_CHUNK_BYTES;
			dev->counters.reserver_count = dev->reserver.count;
		} else {
			/* Some spans freed - free the chunk allocation */
			dev->ops->chunk_free(dev->ops->user, chunk, ZRAM_CHUNK_BYTES);
			/* reserver_count already updated in loop */
			dev->counters.reserver_count = dev->reserver.count;
			break; /* No point continuing if reserver full */
		}
	}

	dev->ops->mutex_unlock(dev->reserver_lock);
	return ZRAM_OK;
}


bool
zram_can_accept(ZramDevice* dev)
{
	if (dev == NULL)
		return false;

	if (dev->counters.reserver_count > 0)
		return true;
	return dev->counters.pool_bytes + ZRAM_CHUNK_BYTES <= dev->pool_limit;
}

/*
 * ============================================================================
 * DETECCIÓN DE PÁGINA REPETIDA (SIMD-optimizado)
 * ============================================================================
 */

static inline bool
zram_is_same_page(const void* page, uint64_t* _pattern)
{
	const uint64_t* words = (const uint64_t*)page;
	uint64_t first = words[0];

#if defined(__HAIKU__)
	/* NEON: 128-bit loads, compare 2 uint64_t per iteration */
#  include <arm_neon.h>
	uint64x2_t vfirst = vdupq_n_u64(first);
	for (uint32_t i = 1; i < (ZRAM_PAGE_SIZE / sizeof(uint64_t)); i += 2) {
		uint64x2_t v = vld1q_u64(&words[i]);
		uint64x2_t eq = vceqq_u64(v, vfirst);
		if (vgetq_lane_u64(eq, 0) != ~0ULL || vgetq_lane_u64(eq, 1) != ~0ULL)
			return false;
	}
#elif defined(__x86_64__) || defined(__i386__)
	/* SSE2: 128-bit loads, compare 2 uint64_t per iteration */
	#include <emmintrin.h>
	__m128i vfirst = _mm_set1_epi64x((long long)first);
	for (uint32_t i = 1; i < (ZRAM_PAGE_SIZE / sizeof(uint64_t)); i += 2) {
		__m128i v = _mm_loadu_si128((const __m128i*)&words[i]);
		__m128i eq = _mm_cmpeq_epi64(v, vfirst);
		if (_mm_movemask_epi8(eq) != 0xFFFF)
			return false;
	}
#else
	/* Fallback scalar */
	for (uint32_t i = 1; i < (ZRAM_PAGE_SIZE / sizeof(uint64_t)); i++) {
		if (words[i] != first)
			return false;
	}
#endif

	*_pattern = first;
	return true;
}

/*
 * ============================================================================
 * CONTEXTOS
 * ============================================================================
 */

ZramCtx*
zram_ctx_get(ZramDevice* dev)
{
	if (dev == NULL)
		return NULL;

	dev->ops->sem_acquire(dev->ctx_sem);

	dev->ops->mutex_lock(dev->ctx_mutex);
	ZramCtx* ctx = NULL;
	uint32_t ctx_idx = dev->ctx_count;
	for (uint32_t i = 0; i < dev->ctx_count; i++) {
		if (!dev->ctx_in_use[i]) {
			dev->ctx_in_use[i] = true;
			ctx = dev->ctxs[i];
			ctx_idx = i;
			break;
		}
	}
	dev->ops->mutex_unlock(dev->ctx_mutex);

	/* C-005: If no ctx available, release semaphore to avoid leak */
	if (ctx == NULL) {
		dev->ops->sem_release(dev->ctx_sem);
	} else {
		ctx->cpu_index = ctx_idx;
	}

	return ctx;
}


void
zram_ctx_put(ZramDevice* dev, ZramCtx* ctx)
{
	if (dev == NULL || ctx == NULL)
		return;

	dev->ops->mutex_lock(dev->ctx_mutex);
	for (uint32_t i = 0; i < dev->ctx_count; i++) {
		if (dev->ctxs[i] == ctx) {
			dev->ctx_in_use[i] = false;
			break;
		}
	}
	dev->ops->mutex_unlock(dev->ctx_mutex);

	dev->ops->sem_release(dev->ctx_sem);
}


void*
zram_ctx_page(ZramCtx* ctx)
{
	return ctx != NULL ? ctx->page : NULL;
}

/*
 * ============================================================================
 * ESCRITURA / LECTURA DE PÁGINAS
 * ============================================================================
 */

int
zram_write_page(ZramDevice* dev, ZramCtx* ctx, uint32_t slot, const void* page)
{
	if (dev == NULL || ctx == NULL || page == NULL || slot >= dev->slot_count)
		return ZRAM_E_INVAL;

	ZramSlot nuevo;
	memset(&nuevo, 0, sizeof(nuevo));
	nuevo.state = ZRAM_EMPTY;

	uint64_t pattern = 0;
	if (zram_is_same_page(page, &pattern)) {
		nuevo.state = ZRAM_SAME;
		nuevo.pattern = pattern;
		nuevo.size = 0;
	} else {
		/* H-007: Engine selection with AUTO heuristic */
		ZramEngine engine = dev->engine;
		if (engine == ZRAM_ENGINE_AUTO) {
			/* AUTO heuristic: estimate page entropy to select best engine.
			 * Simple heuristic: sample first 64 bytes, count unique bytes.
			 * High entropy (>50 unique) -> LZ4 fast (low latency)
			 * Low entropy (<=50 unique) -> LZ4 HC (better ratio)
			 * Future: GPU engines for high-throughput batch compression */
			uint32_t unique = 0;
			uint8_t seen[256] = {0};
			const uint8_t* sample = (const uint8_t*)page;
			for (int i = 0; i < 64; i++) {
				if (seen[sample[i]] == 0) {
					seen[sample[i]] = 1;
					unique++;
				}
			}
			if (unique > 50)
				engine = ZRAM_ENGINE_CPU_LZ4;      /* High entropy -> fast path */
			else
				engine = ZRAM_ENGINE_CPU_LZ4_HC;   /* Low entropy -> high compression */
		}

		int n = 0;
		if (engine == ZRAM_ENGINE_CPU_LZ4_HC) {
			/* H-004: Reset LZ4 HC stream state for deterministic compression */
			LZ4_resetStreamHC(&ctx->state_hc, 9);  /* Default compression level 9 */

			/* LZ4 HC mode - higher compression ratio */
			n = LZ4_compress_HC_extStateHC(&ctx->state_hc, (const char*)page,
				(char*)ctx->out_buf, ZRAM_PAGE_SIZE, ZRAM_MAX_COMPRESSED, 9);
		} else {
			/* LZ4 Fast mode (default) */
			LZ4_resetStream(&ctx->state);
			n = LZ4_compress_fast_extState(&ctx->state, (const char*)page,
				(char*)ctx->out_buf, ZRAM_PAGE_SIZE, ZRAM_MAX_COMPRESSED, 1);
		}

		if (n > 0) {
			uint32_t cls = zram_class_for_size((uint32_t)n);
			dev->ops->mutex_lock(dev->class_mutexes[cls]);
			void* obj = zram_class_alloc(dev, cls, ctx->cpu_index);
			dev->ops->mutex_unlock(dev->class_mutexes[cls]);

			if (obj == NULL) {
				dev->percpu_counters[ctx->cpu_index].write_nomem++;
				return ZRAM_E_NOMEM;
			}
			memcpy(obj, ctx->out_buf, n);

			nuevo.data = (uintptr_t)obj;
			nuevo.size = (uint32_t)n;
			nuevo.state = ZRAM_LZ4;
			nuevo.cls = (uint8_t)cls;
		} else {
			dev->ops->mutex_lock(dev->class_mutexes[ZRAM_CLASS_RAW]);
			void* obj = zram_class_alloc(dev, ZRAM_CLASS_RAW, ctx->cpu_index);
			dev->ops->mutex_unlock(dev->class_mutexes[ZRAM_CLASS_RAW]);

			if (obj == NULL) {
				dev->percpu_counters[ctx->cpu_index].write_nomem++;
				return ZRAM_E_NOMEM;
			}
			memcpy(obj, page, ZRAM_PAGE_SIZE);

			nuevo.data = (uintptr_t)obj;
			nuevo.size = ZRAM_PAGE_SIZE;
			nuevo.state = ZRAM_RAW;
			nuevo.cls = (uint8_t)ZRAM_CLASS_RAW;
		}
	}

	uint32_t stripe = slot % dev->stripe_count;
	dev->ops->mutex_lock(dev->stripe_locks[stripe]);

	ZramSlot viejo = dev->slots[slot];
	dev->slots[slot] = nuevo;

	/* Per-CPU counters: use cpu_index to avoid cache-line ping-pong */
	struct ZramPerCpuCounters* pc = &dev->percpu_counters[ctx->cpu_index];

	/* Contadores: restar lo viejo, sumar lo nuevo. */
	switch (viejo.state) {
		case ZRAM_SAME: pc->pages_same--; break;
		case ZRAM_RAW:  pc->pages_raw--;  break;
		case ZRAM_LZ4:  pc->pages_lz4--;  break;
		default: break;
	}
	if (viejo.state != ZRAM_EMPTY) {
		pc->orig_bytes -= ZRAM_PAGE_SIZE;
		if (viejo.state == ZRAM_RAW || viejo.state == ZRAM_LZ4)
			pc->stored_bytes -= kZramClassSize[viejo.cls];
	}

	switch (nuevo.state) {
		case ZRAM_SAME: pc->pages_same++; break;
		case ZRAM_RAW:  pc->pages_raw++;  break;
		case ZRAM_LZ4:  pc->pages_lz4++;  break;
		default: break;
	}
	pc->orig_bytes += ZRAM_PAGE_SIZE;
	if (nuevo.state == ZRAM_RAW || nuevo.state == ZRAM_LZ4)
		pc->stored_bytes += kZramClassSize[nuevo.cls];
	pc->writes++;

	dev->ops->mutex_unlock(dev->stripe_locks[stripe]);

	if (viejo.state == ZRAM_RAW || viejo.state == ZRAM_LZ4) {
		dev->ops->mutex_lock(dev->class_mutexes[viejo.cls]);
		zram_class_free(dev, viejo.cls, (void*)viejo.data, ctx->cpu_index);
		dev->ops->mutex_unlock(dev->class_mutexes[viejo.cls]);
	}

	return ZRAM_OK;
}


int
zram_read_page(ZramDevice* dev, uint32_t slot, void* dst)
{
	if (dev == NULL || dst == NULL || slot >= dev->slot_count)
		return ZRAM_E_INVAL;

	uint32_t stripe = slot % dev->stripe_count;
	dev->ops->mutex_lock(dev->stripe_locks[stripe]);

	const ZramSlot* s = &dev->slots[slot];
	int result = ZRAM_OK;

	/* Per-CPU counters for read path */
	uint32_t cpu = slot % dev->ctx_count;
	struct ZramPerCpuCounters* pc = &dev->percpu_counters[cpu];

	switch (s->state) {
		case ZRAM_EMPTY:
			pc->read_empty++;
			memset(dst, 0, ZRAM_PAGE_SIZE);
			break;

		case ZRAM_SAME:
			if (s->pattern == 0) {
				memset(dst, 0, ZRAM_PAGE_SIZE);
			} else {
				uint64_t* words = (uint64_t*)dst;
				for (uint32_t i = 0; i < ZRAM_PAGE_SIZE / sizeof(uint64_t); i++)
					words[i] = s->pattern;
			}
			pc->reads++;
			break;

		case ZRAM_RAW:
			memcpy(dst, (void*)s->data, ZRAM_PAGE_SIZE);
			pc->reads++;
			break;

		case ZRAM_LZ4: {
			int r = LZ4_decompress_safe((const char*)s->data, (char*)dst,
				s->size, ZRAM_PAGE_SIZE);
			if (r != (int)ZRAM_PAGE_SIZE) {
				pc->read_corrupt++;
				result = ZRAM_E_CORRUPT;
			} else {
				pc->reads++;
			}
			break;
		}

		default:
			result = ZRAM_E_INVAL;
			break;
	}

	dev->ops->mutex_unlock(dev->stripe_locks[stripe]);
	return result;
}

/*
 * ============================================================================
 * LIBERACIÓN DE SLOTS
 * ============================================================================
 */

void
zram_free_slots(ZramDevice* dev, uint32_t first, uint32_t count)
{
	if (dev == NULL || count == 0 || first >= dev->slot_count)
		return;

	uint32_t last = first + count;
	if (last > dev->slot_count)
		last = dev->slot_count;

	for (uint32_t i = first; i < last; i++) {
		uint32_t stripe = i % dev->stripe_count;
		dev->ops->mutex_lock(dev->stripe_locks[stripe]);

		ZramSlot* s = &dev->slots[i];
		if (s->state != ZRAM_EMPTY) {
			ZramSlot viejo = *s;

			/* Per-CPU counters for free path */
			uint32_t cpu = i % dev->ctx_count;
			struct ZramPerCpuCounters* pc = &dev->percpu_counters[cpu];

			switch (viejo.state) {
				case ZRAM_SAME: pc->pages_same--; break;
				case ZRAM_RAW:  pc->pages_raw--;  break;
				case ZRAM_LZ4:  pc->pages_lz4--;  break;
				default: break;
			}
			pc->orig_bytes -= ZRAM_PAGE_SIZE;
			if (viejo.state == ZRAM_RAW || viejo.state == ZRAM_LZ4)
				pc->stored_bytes -= kZramClassSize[viejo.cls];

			s->state = ZRAM_EMPTY;
			s->size = 0;
			s->cls = 0;
			s->data = 0;
			pc->frees++;

			if (viejo.state == ZRAM_RAW || viejo.state == ZRAM_LZ4) {
				void* old_data = (void*)viejo.data;
				uint8_t old_cls = viejo.cls;
				dev->ops->mutex_unlock(dev->stripe_locks[stripe]);
				
				/* Use slot index to distribute frees across CPU caches */
				uint32_t free_cpu = i % dev->ctx_count;
				dev->ops->mutex_lock(dev->class_mutexes[old_cls]);
				zram_class_free(dev, old_cls, old_data, free_cpu);
				dev->ops->mutex_unlock(dev->class_mutexes[old_cls]);
			} else {
				dev->ops->mutex_unlock(dev->stripe_locks[stripe]);
			}
		} else {
			dev->ops->mutex_unlock(dev->stripe_locks[stripe]);
		}
	}

	/* Flush per-CPU caches to global free lists after bulk free */
	zram_cpu_caches_flush(dev);
}

/*
 * ============================================================================
 * CREACIÓN / DESTRUCCIÓN
 * ============================================================================
 */

int
zram_create(const ZramConfig* cfg, const ZramPlatformOps* ops, ZramDevice** _out)
{
	if (cfg == NULL || ops == NULL || _out == NULL)
		return ZRAM_E_INVAL;
	if (cfg->disk_bytes == 0 || cfg->pool_limit == 0)
		return ZRAM_E_INVAL;
	if (cfg->pool_limit % ZRAM_CHUNK_BYTES != 0)
		return ZRAM_E_INVAL;

	uint32_t slot_count = (uint32_t)(cfg->disk_bytes / ZRAM_PAGE_SIZE);
	if (slot_count == 0)
		return ZRAM_E_INVAL;

	uint32_t ctx_count = ops->cpu_count();
	if (ctx_count == 0)
		ctx_count = 1;
	/* H-003: Remove hardcoded limit - allow up to 256 contexts for large systems */
	if (ctx_count > 256)
		ctx_count = 256;

	/* Dynamic stripe count: cpu_count * 8, clamped to [64, 1024] */
	uint32_t stripe_count = ctx_count * ZRAM_STRIPES_PER_CPU;
	if (stripe_count < ZRAM_STRIPES_MIN)
		stripe_count = ZRAM_STRIPES_MIN;
	if (stripe_count > ZRAM_STRIPES_MAX)
		stripe_count = ZRAM_STRIPES_MAX;

	ZramDevice* dev = (ZramDevice*)ops->mem_alloc(sizeof(ZramDevice));
	if (dev == NULL)
		return ZRAM_E_NOMEM;
	memset(dev, 0, sizeof(*dev));

	dev->ops = ops;
	dev->disk_bytes = cfg->disk_bytes;
	dev->pool_limit = cfg->pool_limit;
	dev->slot_count = slot_count;
	dev->ctx_count = ctx_count;
	dev->stripe_count = stripe_count;
	/* H-007: Engine selection - validate known engines */
	if (cfg->engine == ZRAM_ENGINE_AUTO ||
	    cfg->engine == ZRAM_ENGINE_CPU_LZ4 ||
	    cfg->engine == ZRAM_ENGINE_CPU_LZ4_HC ||
	    cfg->engine == ZRAM_ENGINE_GPU_OPENCL ||
	    cfg->engine == ZRAM_ENGINE_GPU_VULKAN) {
		dev->engine = cfg->engine;
	} else {
		dev->engine = ZRAM_ENGINE_AUTO;
	}

	dev->slots = (ZramSlot*)ops->mem_alloc(slot_count * sizeof(ZramSlot));
	if (dev->slots == NULL)
		goto fail;

	dev->ctxs = (ZramCtx**)ops->mem_alloc(ctx_count * sizeof(ZramCtx*));
	dev->ctx_in_use = (bool*)ops->mem_alloc(ctx_count * sizeof(bool));
	if (dev->ctxs == NULL || dev->ctx_in_use == NULL)
		goto fail;

	/* Per-CPU counter arrays */
	dev->percpu_counters = (struct ZramPerCpuCounters*)ops->mem_alloc(
		ctx_count * sizeof(struct ZramPerCpuCounters));
	if (dev->percpu_counters == NULL)
		goto fail;
	memset(dev->percpu_counters, 0, ctx_count * sizeof(struct ZramPerCpuCounters));

	dev->reserver.capacity = (uint32_t)(cfg->pool_limit / ZRAM_SPAN_BYTES);
	dev->reserver.spans = (void**)ops->mem_alloc(
		dev->reserver.capacity * sizeof(void*));
	if (dev->reserver.spans == NULL)
		goto fail;

	dev->chunk_capacity = (uint32_t)(cfg->pool_limit / ZRAM_CHUNK_BYTES);
	dev->chunks = (void**)ops->mem_alloc(dev->chunk_capacity * sizeof(void*));
	if (dev->chunks == NULL)
		goto fail;

	/* Dynamic stripe locks */
	dev->stripe_locks = (void**)ops->mem_alloc(stripe_count * sizeof(void*));
	if (dev->stripe_locks == NULL)
		goto fail;
	for (uint32_t i = 0; i < stripe_count; i++) {
		dev->stripe_locks[i] = ops->mutex_new();
		if (dev->stripe_locks[i] == NULL)
			goto fail;
	}
	for (uint32_t i = 0; i < ZRAM_CLASS_COUNT; i++) {
		dev->class_mutexes[i] = ops->mutex_new();
		if (dev->class_mutexes[i] == NULL)
			goto fail;
	}

	dev->reserver_lock = ops->mutex_new();
	dev->ctx_mutex = ops->mutex_new();
	dev->ctx_sem = ops->sem_new(ctx_count);
	if (dev->reserver_lock == NULL || dev->ctx_mutex == NULL
			|| dev->ctx_sem == NULL)
		goto fail;

	for (uint32_t i = 0; i < slot_count; i++) {
		dev->slots[i].data = 0;
		dev->slots[i].size = 0;
		dev->slots[i].state = ZRAM_EMPTY;
		dev->slots[i].cls = 0;
	}

	zram_classes_init(dev);

	if (zram_cpu_caches_init(dev) != ZRAM_OK)
		goto fail;

	for (uint32_t i = 0; i < ctx_count; i++) {
		dev->ctxs[i] = (ZramCtx*)ops->mem_alloc(sizeof(ZramCtx));
		if (dev->ctxs[i] == NULL)
			goto fail;
		memset(dev->ctxs[i], 0, sizeof(ZramCtx));
		dev->ctx_in_use[i] = false;
	}

	/* Primera reposición síncrona: la reserva nace llena. */
	zram_refill(dev);
	if (dev->reserver.count == 0) {
		zram_destroy(dev);
		return ZRAM_E_NOMEM;
	}

	dev->destroyed = false;

	*_out = dev;
	return ZRAM_OK;

fail:
	zram_destroy(dev);
	return ZRAM_E_NOMEM;
}


void
zram_destroy(ZramDevice* dev)
{
	if (dev == NULL || dev->destroyed)
		return;
	dev->destroyed = true;

	/* C-008: Free in reverse order - locks last */
	/* 1. Free contexts first (they may hold locks) */
	if (dev->ctxs != NULL) {
		for (uint32_t i = 0; i < dev->ctx_count; i++) {
			if (dev->ctxs[i] != NULL)
				dev->ops->mem_free(dev->ctxs[i]);
		}
		dev->ops->mem_free(dev->ctxs);
	}
	if (dev->ctx_in_use != NULL)
		dev->ops->mem_free(dev->ctx_in_use);

	/* 2. Free slots array */
	if (dev->slots != NULL)
		dev->ops->mem_free(dev->slots);

	/* 3. Free reserver spans array */
	if (dev->reserver.spans != NULL)
		dev->ops->mem_free(dev->reserver.spans);

	/* 4. Free chunks - these were allocated via chunk_alloc */
	for (uint32_t i = 0; i < dev->chunk_count; i++)
		dev->ops->chunk_free(dev->ops->user, dev->chunks[i], ZRAM_CHUNK_BYTES);
	if (dev->chunks != NULL)
		dev->ops->mem_free(dev->chunks);

	/* 4.5. Flush and free per-CPU caches */
	zram_cpu_caches_fini(dev);

	/* 5. Delete synchronization primitives in reverse lock hierarchy order:
	 * stripe_locks (lowest) -> class_mutexes -> reserver_lock (highest) */
	if (dev->ctx_sem != NULL)
		dev->ops->sem_delete(dev->ctx_sem);
	if (dev->ctx_mutex != NULL)
		dev->ops->mutex_delete(dev->ctx_mutex);

	/* C-008: stripe_locks first (lowest in hierarchy) */
	if (dev->stripe_locks != NULL) {
		for (uint32_t i = 0; i < dev->stripe_count; i++) {
			if (dev->stripe_locks[i] != NULL)
				dev->ops->mutex_delete(dev->stripe_locks[i]);
		}
		dev->ops->mem_free(dev->stripe_locks);
	}

	/* Then class_mutexes */
	for (uint32_t i = 0; i < ZRAM_CLASS_COUNT; i++) {
		if (dev->class_mutexes[i] != NULL)
			dev->ops->mutex_delete(dev->class_mutexes[i]);
	}

	/* Finally reserver_lock (highest in hierarchy) */
	if (dev->reserver_lock != NULL)
		dev->ops->mutex_delete(dev->reserver_lock);

	/* Free per-CPU counters */
	if (dev->percpu_counters != NULL)
		dev->ops->mem_free(dev->percpu_counters);

	dev->ops->mem_free(dev);
}

/*
 * Counter aggregation - merges per-CPU counters into global atomics
 * Call periodically from a timer or before reading stats
 */
void
zram_aggregate_counters(ZramDevice* dev)
{
	if (dev == NULL || dev->percpu_counters == NULL)
		return;

	uint64_t pages_same = 0;
	uint64_t pages_raw = 0;
	uint64_t pages_lz4 = 0;
	uint64_t orig_bytes = 0;
	uint64_t stored_bytes = 0;
	uint64_t writes = 0;
	uint64_t reads = 0;
	uint64_t frees = 0;
	uint64_t write_nomem = 0;
	uint64_t read_corrupt = 0;
	uint64_t read_empty = 0;

	for (uint32_t i = 0; i < dev->ctx_count; i++) {
		struct ZramPerCpuCounters* pc = &dev->percpu_counters[i];
		pages_same += pc->pages_same;
		pages_raw += pc->pages_raw;
		pages_lz4 += pc->pages_lz4;
		orig_bytes += pc->orig_bytes;
		stored_bytes += pc->stored_bytes;
		writes += pc->writes;
		reads += pc->reads;
		frees += pc->frees;
		write_nomem += pc->write_nomem;
		read_corrupt += pc->read_corrupt;
		read_empty += pc->read_empty;
	}

	dev->counters.pages_same = pages_same;
	dev->counters.pages_raw = pages_raw;
	dev->counters.pages_lz4 = pages_lz4;
	dev->counters.orig_bytes = orig_bytes;
	dev->counters.stored_bytes = stored_bytes;
	dev->counters.writes = writes;
	dev->counters.reads = reads;
	dev->counters.frees = frees;
	dev->counters.write_nomem = write_nomem;
	dev->counters.read_corrupt = read_corrupt;
	dev->counters.read_empty = read_empty;
}

/*
 * ============================================================================
 * ESTADÍSTICAS E INVARIANTES
 * ============================================================================
 */

void
zram_get_stats(ZramDevice* dev, ZramStats* out)
{
	if (dev == NULL || out == NULL)
		return;

	/* Aggregate per-CPU counters before reading stats */
	zram_aggregate_counters(dev);

	memset(out, 0, sizeof(*out));

	out->pages_same = dev->counters.pages_same;
	out->pages_raw = dev->counters.pages_raw;
	out->pages_lz4 = dev->counters.pages_lz4;
	out->pages_empty = dev->slot_count
		- (dev->counters.pages_same + dev->counters.pages_raw + dev->counters.pages_lz4);
	out->orig_bytes = dev->counters.orig_bytes;
	out->stored_bytes = dev->counters.stored_bytes;
	out->pool_bytes = dev->counters.pool_bytes;
	out->writes = dev->counters.writes;
	out->reads = dev->counters.reads;
	out->frees = dev->counters.frees;
	out->write_nomem = dev->counters.write_nomem;
	out->read_corrupt = dev->counters.read_corrupt;
	out->read_empty = dev->counters.read_empty;

	for (uint32_t c = 0; c < ZRAM_CLASS_COUNT; c++) {
		out->class_live[c] = dev->classes[c].live_count;
		out->class_free[c] = dev->classes[c].free_count;
	}

	out->reserver_count = dev->reserver.count;
	out->reserver_capacity = dev->reserver.capacity;
	out->chunk_count = dev->chunk_count;
	out->chunk_capacity = dev->chunk_capacity;
}


#ifdef ZRAM_TESTING
uint8_t
zram_test_slot_state(const ZramDevice* dev, uint32_t slot)
{
	if (dev == NULL || slot >= dev->slot_count)
		return 0;
	return dev->slots[slot].state;
}


uint64_t
zram_test_stored_bytes(const ZramDevice* dev)
{
	return dev != NULL ? dev->counters.stored_bytes : 0;
}


uint64_t
zram_test_pool_bytes(const ZramDevice* dev)
{
	return dev != NULL ? dev->counters.pool_bytes : 0;
}


uint32_t
zram_test_reserver_count(const ZramDevice* dev)
{
	return dev != NULL ? dev->counters.reserver_count : 0;
}


uint64_t
zram_test_live_count(const ZramDevice* dev, uint32_t cls)
{
	if (dev == NULL || cls >= ZRAM_CLASS_COUNT)
		return 0;
	return dev->classes[cls].live_count;
}
#endif


bool
zram_check_invariants(ZramDevice* dev)
{
	if (dev == NULL)
		return false;

	if (dev->slot_count != (uint32_t)(dev->disk_bytes / ZRAM_PAGE_SIZE))
		return false;

	/* Aggregate per-CPU counters before checking invariants */
	zram_aggregate_counters(dev);

	uint64_t accounted = 0;
	for (uint32_t i = 0; i < dev->slot_count; i++) {
		if (dev->slots[i].state != ZRAM_EMPTY)
			accounted++;
	}

	if (dev->counters.pages_same + dev->counters.pages_raw + dev->counters.pages_lz4 != accounted)
		return false;

	uint64_t stored = 0;
	for (uint32_t c = 0; c < ZRAM_CLASS_COUNT; c++)
		stored += (uint64_t)dev->classes[c].live_count * kZramClassSize[c];

	if (stored != dev->counters.stored_bytes)
		return false;

	return true;
}


/*
 * ============================================================================
 * ESTADÍSTICAS FORMATO POSIX (snprintf/fprintf)
 * ============================================================================
 */

/* Calcula ratio de compresión en porcentaje (100 = 1:1, 200 = 2:1, etc.) */
static inline uint32_t
zram_calc_ratio(uint64_t orig, uint64_t stored)
{
	if (stored == 0)
		return orig > 0 ? 10000 : 100;	/* SAME pages: treat as 100:1 */
	return (uint32_t)((orig * 100) / stored);
}

/* Formatea bytes en notación humana (KiB, MiB, GiB) */
static void
zram_fmt_bytes(char* buf, size_t size, uint64_t bytes)
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

const char*
zram_engine_name(ZramEngine engine)
{
	switch (engine) {
	case ZRAM_ENGINE_CPU_LZ4:       return "cpu-lz4";
	case ZRAM_ENGINE_CPU_LZ4_HC:    return "cpu-lz4-hc (stub)";
	case ZRAM_ENGINE_GPU_OPENCL:    return "gpu-opencl (stub)";
	case ZRAM_ENGINE_GPU_VULKAN:    return "gpu-vulkan (stub)";
	case ZRAM_ENGINE_AUTO:          return "auto";
	default:                        return "unknown";
	}
}

int
zram_stats_snprintf(char* buf, size_t size, const ZramStats* st,
		    const ZramConfig* cfg)
{
	if (buf == NULL || size == 0 || st == NULL)
		return -1;

	char orig_buf[32], stored_buf[32], pool_buf[32];
	zram_fmt_bytes(orig_buf, sizeof(orig_buf), st->orig_bytes);
	zram_fmt_bytes(stored_buf, sizeof(stored_buf), st->stored_bytes);
	zram_fmt_bytes(pool_buf, sizeof(pool_buf), st->pool_bytes);

	uint64_t total_pages = st->pages_same + st->pages_raw + st->pages_lz4 + st->pages_empty;
	uint64_t active_pages = st->pages_same + st->pages_raw + st->pages_lz4;
	uint32_t ratio = zram_calc_ratio(st->orig_bytes, st->stored_bytes);

	/* Format config values first */
	char disk_size_buf[32] = "unknown";
	char pool_limit_buf[32] = "unknown";
	if (cfg) {
		zram_fmt_bytes(disk_size_buf, sizeof(disk_size_buf), cfg->disk_bytes);
		zram_fmt_bytes(pool_limit_buf, sizeof(pool_limit_buf), cfg->pool_limit);
	}

	/* Format dynamic values */
	char same_saved_buf[32];
	zram_fmt_bytes(same_saved_buf, sizeof(same_saved_buf), st->pages_same * ZRAM_PAGE_SIZE);
	uint32_t same_ratio = st->pages_same > 0 ? 10000 : 100;  /* 100:1 for SAME (0 stored bytes) */

	char lz4_stored_buf[32] = "0 B";
	uint32_t lz4_ratio = 100;
	if (st->pages_lz4 > 0) {
		uint64_t lz4_stored = st->stored_bytes - st->pages_raw * ZRAM_PAGE_SIZE;
		zram_fmt_bytes(lz4_stored_buf, sizeof(lz4_stored_buf), lz4_stored);
		lz4_ratio = zram_calc_ratio(st->pages_lz4 * ZRAM_PAGE_SIZE, lz4_stored);
	}

	char raw_stored_buf[32];
	zram_fmt_bytes(raw_stored_buf, sizeof(raw_stored_buf), st->pages_raw * ZRAM_PAGE_SIZE);

	double same_pct = active_pages > 0 ? (double)st->pages_same * 100.0 / active_pages : 0.0;
	double lz4_pct = active_pages > 0 ? (double)st->pages_lz4 * 100.0 / active_pages : 0.0;
	double raw_pct = active_pages > 0 ? (double)st->pages_raw * 100.0 / active_pages : 0.0;
	double active_pct = total_pages > 0 ? (double)active_pages * 100.0 / total_pages : 0.0;

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
		st->pages_empty,
		st->pages_same, same_pct, same_ratio, same_saved_buf,
		st->pages_lz4, lz4_pct, lz4_ratio, lz4_stored_buf,
		st->pages_raw, raw_pct, raw_stored_buf,
		orig_buf, stored_buf, ratio, pool_buf);

	/* Append class distribution */
	for (uint32_t c = 0; c < ZRAM_CLASS_COUNT && len < (int)size; c++) {
		if (st->class_live[c] > 0 || st->class_free[c] > 0) {
			len += snprintf(buf + len, size - len,
				"    class %2u (%4u B): live=%5u free=%5u\n",
				c, kZramClassSize[c], st->class_live[c], st->class_free[c]);
		}
	}

	/* Reserver info */
	if (len < (int)size) {
		snprintf(buf + len, size - len,
			"  reserver: spans=%u/%u\n",
			st->reserver_count, st->reserver_capacity);
	}

	return len;
}

int
zram_stats_fprintf(FILE* fp, const ZramStats* st, const ZramConfig* cfg)
{
	if (fp == NULL || st == NULL)
		return -1;

	char buf[2048];
	int len = zram_stats_snprintf(buf, sizeof(buf), st, cfg);
	if (len < 0)
		return -1;
	return fprintf(fp, "%s", buf);
}