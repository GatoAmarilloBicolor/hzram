/*
 * zram_core.h - Standalone zram core (no kernel dependencies)
 *
 * Capa 1 del diseño (h_zram_plan_implementacion.md, §4). No incluye ninguna
 * cabecera del kernel: se compila y se prueba en el host con g++.
 *
 * La capa 2 (zram_kernel.cpp) implementa ZramPlatformOps con las primitivas
 * reales de Haiku/BeOS (mutex_*, create_sem, vm_memcpy_*, spawn_thread).
 */

#ifndef ZRAM_CORE_H
#define ZRAM_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Constantes
 */
#define ZRAM_PAGE_SIZE      4096u
#define ZRAM_SPAN_BYTES     (64u * 1024u)
#define ZRAM_CHUNK_BYTES    (1024u * 1024u)       /* 16 spans por chunk */
#define ZRAM_STRIPES        64u
#define ZRAM_MAX_COMPRESSED 3072u
#define ZRAM_CLASS_COUNT    10u

static const uint32_t kZramClassSize[ZRAM_CLASS_COUNT] =
    { 256, 512, 768, 1024, 1280, 1536, 2048, 2560, 3072, 4096 };
#define ZRAM_CLASS_RAW 9u

/* Tamaño de la reserva inicial de spans */
#define ZRAM_RESERVE_TARGET 4u

enum ZramState {
    ZRAM_EMPTY = 0,
    ZRAM_SAME  = 1,
    ZRAM_RAW   = 2,
    ZRAM_LZ4   = 3,
};

#ifdef __cplusplus
static_assert(sizeof(ZramSlot) == 16, "ZramSlot must be exactly 16 bytes");
static_assert(ZRAM_PAGE_SIZE == 4096, "zram requires 4096-byte pages");
#endif

/*
 * ZramSlot - exactamente 16 bytes (verificado con static_assert)
 *
 *   [0:8]   data (void*)     -> RAW o LZ4: puntero al objeto
 *           pattern (uint64_t) -> SAME: palabra repetida
 *   [8:12]  size (uint32_t)  -> bytes comprimidos (LZ4); 4096 (RAW); 0 resto
 *   [12:13] state (uint8_t)  -> ZramState
 *   [13:14] cls (uint8_t)    -> clase de tamaño (RAW o LZ4)
 *   [14:16] reserved (uint16_t)
 */
struct ZramSlot {
    union {
        void*    data;
        uint64_t pattern;
    };
    uint32_t size;
    uint8_t  state;
    uint8_t  cls;
    uint16_t reserved;
};

struct ZramConfig {
    uint64_t disk_bytes;     /* tamaño nominal; slots = disk_bytes / 4096 */
    uint64_t pool_limit;     /* tope de memoria física (múltiplo de CHUNK) */
};

struct ZramStats {
    uint64_t pages_same;
    uint64_t pages_raw;
    uint64_t pages_lz4;
    uint64_t orig_bytes;
    uint64_t stored_bytes;
    uint64_t pool_bytes;
    uint64_t writes;
    uint64_t reads;
    uint64_t frees;
    uint64_t write_nomem;
    uint64_t read_corrupt;
    uint64_t read_empty;
};

/*
 * ZramCtx - opaco: estado LZ4 + búfer de salida + página de trabajo.
 * Los búferes de 4 KiB viven aquí para no usar la pila en la ruta de E/S.
 */
struct ZramCtx;

/* Búfer de 4 KiB reservado para el pegamento (capa 2). */
void* zram_ctx_page(ZramCtx* ctx);

/*
 * Operaciones de plataforma - abstraen las primitivas de memoria y locking.
 *
 * Reglas:
 *   - mem_alloc/mem_free: pueden bloquear; solo se llaman en create/destroy.
 *   - chunk_alloc: NO debe bloquear. Devuelve NULL si no hay memoria.
 *   - chunk_free: libera un chunk.
 *   - mutex_*: mutex estándar.
 *   - sem_*: semáforo contador.
 *   - cpu_count: número de CPUs lógicas.
 *   - request_refill: despierta al hilo de reposición (lo llama el núcleo).
 */
struct ZramPlatformOps {
    void*  (*mem_alloc)(size_t);
    void   (*mem_free)(void*);
    void*  (*chunk_alloc)(size_t bytes);
    void   (*chunk_free)(void*, size_t);
    void*  (*mutex_new)(void);
    void   (*mutex_lock)(void*);
    void   (*mutex_unlock)(void*);
    void   (*mutex_delete)(void*);
    void*  (*sem_new)(uint32_t count);
    void   (*sem_acquire)(void*);
    void   (*sem_release)(void*);
    void   (*sem_delete)(void*);
    uint32_t (*cpu_count)(void);
    void   (*request_refill)(void* user);
    void*  user;
};

struct ZramDevice;

/*
 * API pública del núcleo
 *
 * Códigos de retorno: 0 = OK, -1 = ZRAM_E_INVAL, -2 = ZRAM_E_NOMEM,
 *                     -3 = ZRAM_E_CORRUPT
 */
int  zram_create(const ZramConfig* cfg, const ZramPlatformOps* ops,
                 ZramDevice** out);
void zram_destroy(ZramDevice* dev);

ZramCtx* zram_ctx_get(ZramDevice* dev);          /* puede bloquear (semáforo) */
void     zram_ctx_put(ZramDevice* dev, ZramCtx* ctx);

int  zram_write_page(ZramDevice* dev, ZramCtx* ctx, uint32_t slot,
                     const void* page);
int  zram_read_page(ZramDevice* dev, uint32_t slot, void* dst);
void zram_free_slots(ZramDevice* dev, uint32_t first, uint32_t count);

bool zram_can_accept(ZramDevice* dev);           /* no bloquea */
int  zram_refill(ZramDevice* dev);               /* lo llama el hilo */
void zram_get_stats(ZramDevice* dev, ZramStats* out);
bool zram_check_invariants(ZramDevice* dev);     /* solo para pruebas */

/*
 * Accesores de prueba (solo se compilan con -DZRAM_TESTING).
 * Permiten observar estados y contadores internos sin romper la opacidad.
 */
#ifdef ZRAM_TESTING
uint8_t  zram_test_slot_state(const ZramDevice* dev, uint32_t slot);
uint64_t zram_test_stored_bytes(const ZramDevice* dev);
uint64_t zram_test_pool_bytes(const ZramDevice* dev);
uint32_t zram_test_reserver_count(const ZramDevice* dev);
uint64_t zram_test_live_count(const ZramDevice* dev, uint32_t cls);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ZRAM_CORE_H */
