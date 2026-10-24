/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		hzram developers
 */

#include "zram_core.h"

#include <string.h>

#include "lz4/lz4.h"

/*
 * ============================================================================
 * ESTRUCTURAS INTERNAS
 * ============================================================================
 */

struct ZramClass {
	uint32_t	size;			/* tamaño de objeto */
	uint32_t	free_count;		/* objetos en la lista libre */
	uint32_t	live_count;		/* objetos entregados a slots */
	void*		free_list;		/* lista intrusiva: primeros 8 bytes = next */
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

	volatile uint32_t		reserver_count;
	volatile uint64_t		pool_bytes;

	uint64_t				pages_same;
	uint64_t				pages_raw;
	uint64_t				pages_lz4;
	uint64_t				orig_bytes;
	uint64_t				stored_bytes;
	uint64_t				writes;
	uint64_t				reads;
	uint64_t				frees;
	uint64_t				write_nomem;
	uint64_t				read_corrupt;
	uint64_t				read_empty;

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
	if (dev->reserver.count >= dev->reserver.capacity)
		return;
	dev->reserver.spans[dev->reserver.count++] = span;
	dev->reserver_count = dev->reserver.count;
}


/* Reserva un objeto de la clase c. Devuelve NULL si no hay memoria.
 * El llamador debe tener class_mutexes[c] tomado. */
static void*
zram_class_alloc(ZramDevice* dev, uint32_t c)
{
	if (dev->classes[c].free_list == NULL) {
		dev->ops->mutex_lock(dev->reserver_lock);
		void* span = zram_reserver_pop(dev);
		uint32_t remaining = dev->reserver.count;
		dev->ops->mutex_unlock(dev->reserver_lock);

		if (span == NULL) {
			dev->ops->request_refill(dev->ops->user);
			dev->ops->mutex_lock(dev->reserver_lock);
			span = zram_reserver_pop(dev);
			dev->ops->mutex_unlock(dev->reserver_lock);
			if (span == NULL)
				return NULL;
		} else if (remaining < 2) {
			dev->ops->request_refill(dev->ops->user);
		}
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
zram_class_free(ZramDevice* dev, uint32_t c, void* obj)
{
	*(void**)obj = dev->classes[c].free_list;
	dev->classes[c].free_list = obj;
	dev->classes[c].free_count++;
	dev->classes[c].live_count--;
}


/* Lookup O(1) sin ramas (O5): (n + 255) >> 8 mapea a la clase adecuada */
static const uint8_t kSizeToClass[16] = {
	0, 0, 1, 2, 3, 4, 5, 6, 6, 7, 7, 8, 8, 9, 9, 9
};

static inline uint32_t
zram_class_for_size(uint32_t n)
{
	uint32_t idx = (n + 255u) >> 8;
	return idx < 16u ? (uint32_t)kSizeToClass[idx] : ZRAM_CLASS_RAW;
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
		void* chunk = dev->ops->chunk_alloc(ZRAM_CHUNK_BYTES);
		if (chunk == NULL)
			break;

		if (dev->chunk_count < dev->chunk_capacity)
			dev->chunks[dev->chunk_count++] = chunk;

		for (uint32_t i = 0; i < ZRAM_CHUNK_BYTES / ZRAM_SPAN_BYTES; i++) {
			zram_reserver_push(dev,
				(uint8_t*)chunk + i * ZRAM_SPAN_BYTES);
		}
		dev->pool_bytes += ZRAM_CHUNK_BYTES;
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
 * DETECCIÓN DE PÁGINA REPETIDA
 * ============================================================================
 */

static bool
zram_is_same_page(const void* page, uint64_t* _pattern)
{
	const uint64_t* words = (const uint64_t*)page;
	uint64_t first = words[0];

	/* Check all words in one pass */
	for (uint32_t i = 1; i < (ZRAM_PAGE_SIZE / sizeof(uint64_t)); i++) {
		if (words[i] != first)
			return false;
	}

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
	for (uint32_t i = 0; i < dev->ctx_count; i++) {
		if (!dev->ctx_in_use[i]) {
			dev->ctx_in_use[i] = true;
			ctx = dev->ctxs[i];
			break;
		}
	}
	dev->ops->mutex_unlock(dev->ctx_mutex);

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
		int n = LZ4_compress_fast_extState(&ctx->state, (const char*)page,
			(char*)ctx->out_buf, ZRAM_PAGE_SIZE, ZRAM_MAX_COMPRESSED, 1);

		if (n > 0) {
			uint32_t cls = zram_class_for_size((uint32_t)n);
			dev->ops->mutex_lock(dev->class_mutexes[cls]);
			void* obj = zram_class_alloc(dev, cls);
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
			void* obj = zram_class_alloc(dev, ZRAM_CLASS_RAW);
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
		zram_class_free(dev, viejo.cls, viejo.data);
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

			dev->ops->mutex_unlock(dev->stripe_locks[stripe]);

			if (viejo.state == ZRAM_RAW || viejo.state == ZRAM_LZ4) {
				dev->ops->mutex_lock(dev->class_mutexes[viejo.cls]);
				zram_class_free(dev, viejo.cls, viejo.data);
				dev->ops->mutex_unlock(dev->class_mutexes[viejo.cls]);
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
	if (ctx_count > 8)
		ctx_count = 8;

	ZramDevice* dev = (ZramDevice*)ops->mem_alloc(sizeof(ZramDevice));
	if (dev == NULL)
		return ZRAM_E_NOMEM;
	memset(dev, 0, sizeof(*dev));

	dev->ops = ops;
	dev->disk_bytes = cfg->disk_bytes;
	dev->pool_limit = cfg->pool_limit;
	dev->slot_count = slot_count;
	dev->ctx_count = ctx_count;

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
		dev->slots[i].reserved = 0;
	}

	zram_classes_init(dev);

	for (uint32_t i = 0; i < ctx_count; i++) {
		dev->ctxs[i] = (ZramCtx*)ops->mem_alloc(sizeof(ZramCtx));
		if (dev->ctxs[i] == NULL)
			goto fail;
		memset(dev->ctxs[i], 0, sizeof(ZramCtx));
		dev->ctx_in_use[i] = false;
	}

	/* Primera reposición síncrona: la reserva nace llena. */
	zram_refill(dev);

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

	if (dev->ctxs != NULL) {
		for (uint32_t i = 0; i < dev->ctx_count; i++) {
			if (dev->ctxs[i] != NULL)
				dev->ops->mem_free(dev->ctxs[i]);
		}
		dev->ops->mem_free(dev->ctxs);
	}
	if (dev->ctx_in_use != NULL)
		dev->ops->mem_free(dev->ctx_in_use);
	if (dev->slots != NULL)
		dev->ops->mem_free(dev->slots);
	if (dev->reserver.spans != NULL)
		dev->ops->mem_free(dev->reserver.spans);

	for (uint32_t i = 0; i < dev->chunk_count; i++)
		dev->ops->chunk_free(dev->chunks[i], ZRAM_CHUNK_BYTES);
	if (dev->chunks != NULL)
		dev->ops->mem_free(dev->chunks);

	if (dev->ctx_sem != NULL)
		dev->ops->sem_delete(dev->ctx_sem);
	if (dev->ctx_mutex != NULL)
		dev->ops->mutex_delete(dev->ctx_mutex);
	if (dev->reserver_lock != NULL)
		dev->ops->mutex_delete(dev->reserver_lock);
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
