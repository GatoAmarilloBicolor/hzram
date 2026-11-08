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

/*
 * ============================================================================
 * ESTRUCTURAS INTERNAS
 * ============================================================================
 */

/* Per-CPU cache for lock-free allocation (H-005) */
#define ZRAM_CPU_CACHE_SIZE		16u

struct ZramCpuCache {
	void*		entries[ZRAM_CPU_CACHE_SIZE];
	uint32_t	count;
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
	alignas(64) uint8_t	out_buf[ZRAM_PAGE_SIZE];
	alignas(64) uint8_t	page[ZRAM_PAGE_SIZE];
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

	/* Hot counters - aligned to cache lines to avoid false sharing (H-001) */
	alignas(64) volatile uint32_t		reserver_count;
	alignas(64) volatile uint64_t		pool_bytes;
	alignas(64) volatile uint64_t		pages_same;
	alignas(64) volatile uint64_t		pages_raw;
	alignas(64) volatile uint64_t		pages_lz4;
	alignas(64) volatile uint64_t		orig_bytes;
	alignas(64) volatile uint64_t		stored_bytes;
	alignas(64) volatile uint64_t		writes;
	alignas(64) volatile uint64_t		reads;
	alignas(64) volatile uint64_t		frees;
	alignas(64) volatile uint64_t		write_nomem;
	alignas(64) volatile uint64_t		read_corrupt;
	alignas(64) volatile uint64_t		read_empty;

	void**					chunks;
	uint32_t				chunk_count;
	uint32_t				chunk_capacity;

	struct ZramClass		classes[ZRAM_CLASS_COUNT];
	struct ZramReserver		reserver;

	void*					stripe_locks[ZRAM_STRIPES];
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
	for (uint32_t c = 0; c < ZRAM_CLASS_COUNT; c++) {
		dev->classes[c].cpu_cache_count = dev->ctx_count;
		dev->classes[c].cpu_caches = (struct ZramCpuCache*)dev->ops->mem_alloc(
			dev->ctx_count * sizeof(struct ZramCpuCache));
		if (dev->classes[c].cpu_caches == NULL)
			return ZRAM_E_NOMEM;
		memset(dev->classes[c].cpu_caches, 0, dev->ctx_count * sizeof(struct ZramCpuCache));
	}
	return ZRAM_OK;
}

static void
zram_cpu_caches_fini(ZramDevice* dev)
{
	for (uint32_t c = 0; c < ZRAM_CLASS_COUNT; c++) {
		if (dev->classes[c].cpu_caches != NULL) {
			dev->ops->mem_free(dev->classes[c].cpu_caches);
			dev->classes[c].cpu_caches = NULL;
			dev->classes[c].cpu_cache_count = 0;
		}
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
		dev->pool_bytes -= ZRAM_SPAN_BYTES;
		return;
	}
	dev->reserver.spans[dev->reserver.count++] = span;
	dev->reserver_count = dev->reserver.count;
}


/* Reserva un objeto de la clase c. Devuelve NULL si no hay memoria.
 * El llamador debe tener class_mutexes[c] tomado. */
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
	
	/* Fallback to global free list (requires class mutex) */
	if (dev->classes[c].free_list == NULL) {
		dev->ops->mutex_lock(dev->reserver_lock);
		void* span = zram_reserver_pop(dev);
		if (span == NULL) {
			dev->ops->request_refill(dev->ops->user);
			span = zram_reserver_pop(dev);
		}
		dev->ops->mutex_unlock(dev->reserver_lock);
		
		if (span == NULL)
			return NULL;
		zram_carve_span(dev, span, c);
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
	if (cache->count < ZRAM_CPU_CACHE_SIZE) {
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
			&& dev->pool_bytes + ZRAM_CHUNK_BYTES <= dev->pool_limit) {
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
			dev->pool_bytes += ZRAM_CHUNK_BYTES;
			dev->reserver_count = dev->reserver.count;
		} else {
			/* Some spans freed - free the chunk allocation */
			dev->ops->chunk_free(dev->ops->user, chunk, ZRAM_CHUNK_BYTES);
			/* reserver_count already updated in loop */
			dev->reserver_count = dev->reserver.count;
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

	if (dev->reserver_count > 0)
		return true;
	return dev->pool_bytes + ZRAM_CHUNK_BYTES <= dev->pool_limit;
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
			/* AUTO heuristic: could use different acceleration levels based on entropy */
			/* For now, always use LZ4 fast (acceleration=1) */
			engine = ZRAM_ENGINE_CPU_LZ4;
		}

		/* H-004: Reset LZ4 stream state for deterministic compression */
		LZ4_resetStream(&ctx->state);

		/* LZ4 Fast mode (default) */
		int n = LZ4_compress_fast_extState(&ctx->state, (const char*)page,
			(char*)ctx->out_buf, ZRAM_PAGE_SIZE, ZRAM_MAX_COMPRESSED, 1);

		if (n > 0) {
			uint32_t cls = zram_class_for_size((uint32_t)n);
			dev->ops->mutex_lock(dev->class_mutexes[cls]);
			void* obj = zram_class_alloc(dev, cls, ctx->cpu_index);
			dev->ops->mutex_unlock(dev->class_mutexes[cls]);

			if (obj == NULL) {
				dev->write_nomem++;
				return ZRAM_E_NOMEM;
			}
			memcpy(obj, ctx->out_buf, n);

			nuevo.data = obj;
			nuevo.size = (uint32_t)n;
			nuevo.state = ZRAM_LZ4;
			nuevo.cls = (uint8_t)cls;
		} else {
			dev->ops->mutex_lock(dev->class_mutexes[ZRAM_CLASS_RAW]);
			void* obj = zram_class_alloc(dev, ZRAM_CLASS_RAW, ctx->cpu_index);
			dev->ops->mutex_unlock(dev->class_mutexes[ZRAM_CLASS_RAW]);

			if (obj == NULL) {
				dev->write_nomem++;
				return ZRAM_E_NOMEM;
			}
			memcpy(obj, page, ZRAM_PAGE_SIZE);

			nuevo.data = obj;
			nuevo.size = ZRAM_PAGE_SIZE;
			nuevo.state = ZRAM_RAW;
			nuevo.cls = (uint8_t)ZRAM_CLASS_RAW;
		}
	}

	uint32_t stripe = slot % ZRAM_STRIPES;
	dev->ops->mutex_lock(dev->stripe_locks[stripe]);

	ZramSlot viejo = dev->slots[slot];
	dev->slots[slot] = nuevo;

	/* Contadores: restar lo viejo, sumar lo nuevo. */
	switch (viejo.state) {
		case ZRAM_SAME: dev->pages_same--; break;
		case ZRAM_RAW:  dev->pages_raw--;  break;
		case ZRAM_LZ4:  dev->pages_lz4--;  break;
		default: break;
	}
	if (viejo.state != ZRAM_EMPTY) {
		dev->orig_bytes -= ZRAM_PAGE_SIZE;
		if (viejo.state == ZRAM_RAW || viejo.state == ZRAM_LZ4)
			dev->stored_bytes -= kZramClassSize[viejo.cls];
	}

	switch (nuevo.state) {
		case ZRAM_SAME: dev->pages_same++; break;
		case ZRAM_RAW:  dev->pages_raw++;  break;
		case ZRAM_LZ4:  dev->pages_lz4++;  break;
		default: break;
	}
	dev->orig_bytes += ZRAM_PAGE_SIZE;
	if (nuevo.state == ZRAM_RAW || nuevo.state == ZRAM_LZ4)
		dev->stored_bytes += kZramClassSize[nuevo.cls];
	dev->writes++;

	dev->ops->mutex_unlock(dev->stripe_locks[stripe]);

	if (viejo.state == ZRAM_RAW || viejo.state == ZRAM_LZ4) {
		dev->ops->mutex_lock(dev->class_mutexes[viejo.cls]);
		zram_class_free(dev, viejo.cls, viejo.data, ctx->cpu_index);
		dev->ops->mutex_unlock(dev->class_mutexes[viejo.cls]);
	}

	return ZRAM_OK;
}


int
zram_read_page(ZramDevice* dev, uint32_t slot, void* dst)
{
	if (dev == NULL || dst == NULL || slot >= dev->slot_count)
		return ZRAM_E_INVAL;

	uint32_t stripe = slot % ZRAM_STRIPES;
	dev->ops->mutex_lock(dev->stripe_locks[stripe]);

	const ZramSlot* s = &dev->slots[slot];
	int result = ZRAM_OK;

	switch (s->state) {
		case ZRAM_EMPTY:
			dev->read_empty++;
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
			dev->reads++;
			break;

		case ZRAM_RAW:
			memcpy(dst, s->data, ZRAM_PAGE_SIZE);
			dev->reads++;
			break;

		case ZRAM_LZ4: {
			int r = LZ4_decompress_safe((const char*)s->data, (char*)dst,
				s->size, ZRAM_PAGE_SIZE);
			if (r != (int)ZRAM_PAGE_SIZE) {
				dev->read_corrupt++;
				result = ZRAM_E_CORRUPT;
			} else {
				dev->reads++;
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
		uint32_t stripe = i % ZRAM_STRIPES;
		dev->ops->mutex_lock(dev->stripe_locks[stripe]);

		ZramSlot* s = &dev->slots[i];
		if (s->state != ZRAM_EMPTY) {
			ZramSlot viejo = *s;

			switch (viejo.state) {
				case ZRAM_SAME: dev->pages_same--; break;
				case ZRAM_RAW:  dev->pages_raw--;  break;
				case ZRAM_LZ4:  dev->pages_lz4--;  break;
				default: break;
			}
			dev->orig_bytes -= ZRAM_PAGE_SIZE;
			if (viejo.state == ZRAM_RAW || viejo.state == ZRAM_LZ4)
				dev->stored_bytes -= kZramClassSize[viejo.cls];

			s->state = ZRAM_EMPTY;
			s->size = 0;
			s->cls = 0;
			s->data = NULL;
			dev->frees++;

			if (viejo.state == ZRAM_RAW || viejo.state == ZRAM_LZ4) {
				void* old_data = viejo.data;
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

	ZramDevice* dev = (ZramDevice*)ops->mem_alloc(sizeof(ZramDevice));
	if (dev == NULL)
		return ZRAM_E_NOMEM;
	memset(dev, 0, sizeof(*dev));

	dev->ops = ops;
	dev->disk_bytes = cfg->disk_bytes;
	dev->pool_limit = cfg->pool_limit;
	dev->slot_count = slot_count;
	dev->ctx_count = ctx_count;
	/* H-007: Engine selection */
	dev->engine = cfg->engine != ZRAM_ENGINE_AUTO && cfg->engine <= ZRAM_ENGINE_CPU_LZ4 
		? cfg->engine : ZRAM_ENGINE_AUTO;

	dev->slots = (ZramSlot*)ops->mem_alloc(slot_count * sizeof(ZramSlot));
	if (dev->slots == NULL)
		goto fail;

	dev->ctxs = (ZramCtx**)ops->mem_alloc(ctx_count * sizeof(ZramCtx*));
	dev->ctx_in_use = (bool*)ops->mem_alloc(ctx_count * sizeof(bool));
	if (dev->ctxs == NULL || dev->ctx_in_use == NULL)
		goto fail;

	dev->reserver.capacity = (uint32_t)(cfg->pool_limit / ZRAM_SPAN_BYTES);
	dev->reserver.spans = (void**)ops->mem_alloc(
		dev->reserver.capacity * sizeof(void*));
	if (dev->reserver.spans == NULL)
		goto fail;

	dev->chunk_capacity = (uint32_t)(cfg->pool_limit / ZRAM_CHUNK_BYTES);
	dev->chunks = (void**)ops->mem_alloc(dev->chunk_capacity * sizeof(void*));
	if (dev->chunks == NULL)
		goto fail;

	for (uint32_t i = 0; i < ZRAM_STRIPES; i++) {
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
		dev->slots[i].data = NULL;
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

	/* 5. Delete synchronization primitives (class_mutexes before stripe_locks) */
	if (dev->ctx_sem != NULL)
		dev->ops->sem_delete(dev->ctx_sem);
	if (dev->ctx_mutex != NULL)
		dev->ops->mutex_delete(dev->ctx_mutex);
	if (dev->reserver_lock != NULL)
		dev->ops->mutex_delete(dev->reserver_lock);
	
	/* C-008: class_mutexes BEFORE stripe_locks to avoid deadlock with free_slots */
	for (uint32_t i = 0; i < ZRAM_CLASS_COUNT; i++) {
		if (dev->class_mutexes[i] != NULL)
			dev->ops->mutex_delete(dev->class_mutexes[i]);
	}
	for (uint32_t i = 0; i < ZRAM_STRIPES; i++) {
		if (dev->stripe_locks[i] != NULL)
			dev->ops->mutex_delete(dev->stripe_locks[i]);
	}

	dev->ops->mem_free(dev);
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

	memset(out, 0, sizeof(*out));

	out->pages_same = dev->pages_same;
	out->pages_raw = dev->pages_raw;
	out->pages_lz4 = dev->pages_lz4;
	out->pages_empty = dev->slot_count
		- (dev->pages_same + dev->pages_raw + dev->pages_lz4);
	out->orig_bytes = dev->orig_bytes;
	out->stored_bytes = dev->stored_bytes;
	out->pool_bytes = dev->pool_bytes;
	out->writes = dev->writes;
	out->reads = dev->reads;
	out->frees = dev->frees;
	out->write_nomem = dev->write_nomem;
	out->read_corrupt = dev->read_corrupt;
	out->read_empty = dev->read_empty;

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
	return dev != NULL ? dev->stored_bytes : 0;
}


uint64_t
zram_test_pool_bytes(const ZramDevice* dev)
{
	return dev != NULL ? dev->pool_bytes : 0;
}


uint32_t
zram_test_reserver_count(const ZramDevice* dev)
{
	return dev != NULL ? dev->reserver.count : 0;
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

	uint64_t accounted = 0;
	for (uint32_t i = 0; i < dev->slot_count; i++) {
		if (dev->slots[i].state != ZRAM_EMPTY)
			accounted++;
	}

	if (dev->pages_same + dev->pages_raw + dev->pages_lz4 != accounted)
		return false;

	uint64_t stored = 0;
	for (uint32_t c = 0; c < ZRAM_CLASS_COUNT; c++)
		stored += (uint64_t)dev->classes[c].live_count * kZramClassSize[c];

	if (stored != dev->stored_bytes)
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
		"    compression_ratio: %u.%02u%%\n"
		"    pool_allocated:   %s\n"
		"  class_distribution:\n",
		disk_size_buf, pool_limit_buf,
		total_pages,
		active_pages, active_pct,
		st->pages_empty,
		st->pages_same, same_pct, ratio, same_saved_buf,
		st->pages_lz4, lz4_pct, lz4_ratio, lz4_stored_buf,
		st->pages_raw, raw_pct, raw_stored_buf,
		orig_buf, stored_buf, ratio / 100, ratio % 100, pool_buf);

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