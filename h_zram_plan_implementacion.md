# h\_zram: plan de implementación para un LLM coder

 Reemplaza el enfoque de "driver de bloques + swapon" por un diseño verificado contra el código fuente de Haiku (`VMAnonymousCache.cpp`, rama r1beta4).

**Cómo usar este documento (instrucciones para el implementador)**

1. Haz las tareas en orden. Una tarea = un commit. No empieces otra hasta que se cumpla el criterio de aceptación.
2. Cada tarea indica dónde se puede verificar: `[HOST]` (compilar y probar en Linux/macOS con g++, sin Haiku) o `[HAIKU]` (requiere compilar el kernel de Haiku y arrancarlo en una VM; lo ejecuta una persona).
3. **No inventes APIs del kernel.** Antes de usar cualquier función, cabecera o macro de Haiku, búscala en el árbol (`grep -rn nombre headers/ src/system/kernel/`) y pega su declaración real como comentario sobre la primera llamada. Si no existe o difiere, detente y anótalo en la sección "Hallazgos" al final.
4. Las secciones "Decisiones cerradas" y "Reglas innegociables" no se reabren. Si una tarea parece obligarte a romperlas, detente y pregunta.
5. Lo marcado **(VERIFICADO)** se leyó directamente del código fuente de Haiku. Lo marcado **(SUPUESTO)** debe confirmarse en la tarea T0.

---

## 1. Qué se construye

Un backend de swap comprimido en RAM para Haiku, integrado en el sistema de swap existente. Cuando el `page_writer` expulsa una página anónima, esta se comprime con LZ4 y se guarda en un pool de memoria del kernel en lugar de escribirse a `/var/swap`. Cuando el pool se llena, el swap en disco sigue funcionando como hasta ahora.

### Por qué NO un driver de bloques (decisión tomada tras leer el código)

| Hecho en el código de Haiku | Consecuencia |
| --- | --- |
| `swap_slot_dealloc()` solo libera bits en el `radix_bitmap`; no avisa al medio de respaldo (**VERIFICADO**) | Con un driver de bloques, los datos comprimidos de slots ya liberados se quedarían ocupando RAM hasta ser sobrescritos. Es una fuga lógica. Con un backend integrado se puede liberar en el mismo punto. |
| `swap_slot_alloc()` reparte entre swap files en orden de lista y cambia al siguiente cuando el actual supera el 90 % de uso; nunca vuelve atrás hasta agotar los demás (**VERIFICADO**) | No hay prioridades. Hay que modificar `swap_slot_alloc` para preferir siempre el backend comprimido. |
| `swap_file_add()` acepta archivos regulares y nodos de carácter o bloque, pero exige `st_size >= B_PAGE_SIZE` leído con `fstat` (**VERIFICADO**) | Un nodo de dispositivo solo sirve si devfs informa un `st_size` correcto (**SUPUESTO**, y es dudoso). |
| La E/S de swap usa `vfs_read_pages`, `vfs_write_pages` y `vfs_asynchronous_write_pages` sobre `swap_file->vnode` (**VERIFICADO**) | Para un driver habría que entender además cómo devfs traduce un `io_request` a los hooks del driver. Con backend integrado esas tres llamadas se sustituyen por una rama local. |

**Conclusión:** `zram` será un tipo de `swap_file` dentro de `VMAnonymousCache.cpp`, apoyado en una biblioteca núcleo independiente del kernel.

---

## 2. Arquitectura en tres capas

```
┌──────────────────────────────────────────────────────────┐
│ Capa 3: parche en VMAnonymousCache.cpp (≈ 80 líneas)     │
│  swap_file.zram, rama en Read/Write/WriteAsync,          │
│  hook en swap_slot_dealloc, prioridad en swap_slot_alloc │
├──────────────────────────────────────────────────────────┤
│ Capa 2: pegamento del kernel (zram_kernel.cpp)           │
│  ops de plataforma, hilo de reposición, copia desde/hacia │
│  vectores físicos, comando de depuración `zram`          │
├──────────────────────────────────────────────────────────┤
│ Capa 1: núcleo (zram_core.cpp/.h + lz4.c)                │
│  SIN includes del kernel. Probable al 100 % en host.     │
└──────────────────────────────────────────────────────────┘
```

**Principio de diseño:** todo lo que pueda fallar de forma sutil (tabla de slots, asignador, locking, compresión) vive en la capa 1 y se prueba en el host con sanitizers. Las capas 2 y 3 son finas.

Árbol de archivos nuevo:

```
src/system/kernel/vm/zram/
    Jamfile
    zram_core.h          # API pública de la capa 1
    zram_core.cpp
    zram_kernel.h        # API de la capa 2 usada por el parche
    zram_kernel.cpp
    lz4/lz4.c  lz4/lz4.h # LZ4 oficial v1.9.x, sin modificar (BSD-2)
    lz4/LICENSE
tests/zram_host/
    Makefile
    test_*.cpp
    platform_host.cpp    # implementación de ZramPlatformOps para el host
```

---

## 3. Decisiones cerradas

| Tema | Decisión |
| --- | --- |
| Algoritmo | LZ4 (`LZ4_compress_fast_extState`, aceleración 1; `LZ4_decompress_safe`). Sin ZSTD en v1. |
| Tamaño de página | 4096 bytes (`B_PAGE_SIZE`). El núcleo asume 4096 y falla en compilación si no. |
| Estados de slot | `EMPTY`, `SAME` (página con una palabra de 8 bytes repetida; incluye ceros), `RAW` (sin comprimir), `LZ4`. |
| Umbral de compresión | Se pide a LZ4 un destino máximo de 3072 bytes. Si no cabe, devuelve 0 y la página se guarda `RAW`. |
| Clases de tamaño | `{256, 512, 768, 1024, 1280, 1536, 2048, 2560, 3072, 4096}`. La clase 4096 es solo para `RAW`. |
| Asignador | Spans de 64 KiB tallados en objetos de tamaño fijo por clase, con lista libre intrusiva. Los spans salen de chunks de 1 MiB pedidos a la plataforma. |
| Tabla de slots | Un arreglo plano de `ZramSlot` (16 bytes) preasignado al crear. Sin estructuras dinámicas en la ruta de E/S. |
| Locking | 64 mutex "stripe" por slot (`idx % 64`), un mutex por clase de tamaño, un mutex para la reserva de spans. Ver orden en §5. |
| Contextos de compresión | Pool de `N = clamp(cpus, 1, 8)` contextos con un semáforo contador. |
| Ruta de E/S | Nunca bloquea esperando memoria. Si no hay objeto libre, devuelve `B_NO_MEMORY` y la página queda sucia en RAM (comportamiento seguro del `page_writer`). |
| Reposición de memoria | Un hilo del kernel rellena la reserva de spans fuera de la ruta de E/S. |
| Prioridad | Los `swap_file` con `zram != NULL` se prueban primero en `swap_slot_alloc`. |
| Configuración | Archivo de ajustes `virtual_memory` (el mismo que ya lee `swap_init_post_modules`): `zram_size` y `zram_pool_limit`, en bytes. Sin ellos, zram está desactivado. |
| Encogimiento del pool | Fuera de v1. El pool crece hasta el límite y no devuelve memoria. |

---

## 4. Estructuras de datos exactas

```
// zram_core.h
#define ZRAM_PAGE_SIZE      4096u
#define ZRAM_SPAN_BYTES     (64u * 1024u)
#define ZRAM_CHUNK_BYTES    (1024u * 1024u)       // 16 spans por chunk
#define ZRAM_STRIPES        64u
#define ZRAM_MAX_COMPRESSED 3072u
#define ZRAM_CLASS_COUNT    10u

static const uint32_t kZramClassSize[ZRAM_CLASS_COUNT] =
    { 256, 512, 768, 1024, 1280, 1536, 2048, 2560, 3072, 4096 };
#define ZRAM_CLASS_RAW 9u

enum ZramState : uint8_t { ZRAM_EMPTY = 0, ZRAM_SAME, ZRAM_RAW, ZRAM_LZ4 };

struct ZramSlot {            // exactamente 16 bytes (static_assert)
    union {
        void*    data;       // RAW o LZ4: puntero al objeto
        uint64_t pattern;    // SAME: palabra repetida
    };
    uint32_t size;           // bytes comprimidos (LZ4); 4096 (RAW); 0 en otros casos
    uint8_t  state;          // ZramState
    uint8_t  cls;            // clase de tamaño (RAW, LZ4)
    uint16_t reserved;
};

struct ZramConfig {
    uint64_t disk_bytes;     // tamaño nominal; slots = disk_bytes / 4096
    uint64_t pool_limit;     // tope de memoria física del pool (múltiplo de CHUNK)
};

struct ZramStats {
    uint64_t pages_same, pages_raw, pages_lz4;
    uint64_t orig_bytes;          // bytes sin comprimir representados
    uint64_t stored_bytes;        // bytes de objetos en uso (suma de clases)
    uint64_t pool_bytes;          // memoria total tomada de la plataforma
    uint64_t writes, reads, frees;
    uint64_t write_nomem, read_corrupt, read_empty;
};

struct ZramCtx;                    // opaco: state LZ4 + buf[4096] + page[4096]
void* zram_ctx_page(ZramCtx*);     // búfer de 4 KiB reservado para el pegamento

struct ZramPlatformOps {
    void* (*mem_alloc)(size_t);            // puede bloquear; solo en create/destroy
    void  (*mem_free)(void*);
    void* (*chunk_alloc)(size_t bytes);    // NO bloquea; NULL si no hay; memoria cableada
    void  (*chunk_free)(void*, size_t);
    void* (*mutex_new)(void);
    void  (*mutex_lock)(void*);
    void  (*mutex_unlock)(void*);
    void  (*mutex_delete)(void*);
    void* (*sem_new)(uint32_t count);      // semáforo contador para contextos
    void  (*sem_acquire)(void*);
    void  (*sem_release)(void*);
    void  (*sem_delete)(void*);
    uint32_t (*cpu_count)(void);
    void  (*request_refill)(void* user);   // despierta al hilo de reposición
    void* user;
};
```

### API pública del núcleo

```
int  zram_create(const ZramConfig*, const ZramPlatformOps*, ZramDevice** out);
void zram_destroy(ZramDevice*);

ZramCtx* zram_ctx_get(ZramDevice*);                 // puede bloquear (semáforo)
void     zram_ctx_put(ZramDevice*, ZramCtx*);

// Códigos: 0 = OK, -1 = ZRAM_E_INVAL, -2 = ZRAM_E_NOMEM, -3 = ZRAM_E_CORRUPT
int  zram_write_page(ZramDevice*, ZramCtx*, uint32_t slot, const void* page4k);
int  zram_read_page (ZramDevice*, uint32_t slot, void* page4k);
void zram_free_slots(ZramDevice*, uint32_t first, uint32_t count);  // idempotente

bool zram_can_accept(ZramDevice*);                  // no bloquea
int  zram_refill(ZramDevice*);                      // lo llama el hilo; puede bloquear en chunk_alloc
void zram_get_stats(ZramDevice*, ZramStats*);
bool zram_check_invariants(ZramDevice*);            // solo para pruebas
```

---

## 5. Algoritmos y reglas de concurrencia

### Orden de locks (nunca invertir)

```
sSwapFileListLock (del kernel)  →  [contexto: semáforo]  →  mutex de clase  →  mutex de reserva
mutex stripe: hoja (nunca se toma otro lock estando dentro, salvo copiar datos)
```

Reglas derivadas:

- El hook `zram_free_slots` se llama con `sSwapFileListLock` tomado (desde `swap_slot_dealloc`). El núcleo nunca debe llamar de vuelta al código de swap.
- Los objetos reemplazados o liberados se devuelven al asignador **después** de soltar el stripe.
- Ninguna función de la ruta de E/S llama a `mem_alloc` ni a `chunk_alloc`.

### Detección de página repetida

```
palabras = (uint64_t*)page; p = palabras[0]
si todas las 512 palabras == p  → SAME con pattern = p
```

### zram\_write\_page (pseudocódigo exacto)

```
si slot >= slot_count o page == NULL: return INVAL
nuevo = {EMPTY}

si es_repetida(page, &p):
    nuevo = {state=SAME, pattern=p}
sino:
    n = LZ4_compress_fast_extState(ctx->state, page, ctx->buf, 4096, 3072, 1)
    si n > 0:
        cls = primera clase con size >= n          // nunca la clase RAW
        obj = clase_alloc(cls)
        si obj == NULL: stats.write_nomem++; return NOMEM
        memcpy(obj, ctx->buf, n)
        nuevo = {LZ4, data=obj, size=n, cls=cls}
    sino:
        obj = clase_alloc(ZRAM_CLASS_RAW)
        si obj == NULL: stats.write_nomem++; return NOMEM
        memcpy(obj, page, 4096)
        nuevo = {RAW, data=obj, size=4096, cls=RAW}

lock(stripe[slot % 64])
    viejo = tabla[slot]; tabla[slot] = nuevo
    actualizar contadores (restar viejo, sumar nuevo)
unlock

si viejo.state es RAW o LZ4: clase_free(viejo.cls, viejo.data)
return OK
```

Un fallo `NOMEM` **no modifica** la tabla: el contenido anterior del slot sigue intacto.

### zram\_read\_page

```
lock(stripe[slot % 64])
  s = tabla[slot]
  EMPTY: stats.read_empty++; memset(dst, 0, 4096)
  SAME : rellenar dst con la palabra
  RAW  : memcpy(dst, s.data, 4096)
  LZ4  : r = LZ4_decompress_safe(s.data, dst, s.size, 4096)
         si r != 4096: stats.read_corrupt++; unlock; return CORRUPT
unlock
return OK
```

La descompresión ocurre con el stripe tomado: es corta (\~microsegundos) y evita copiar a un búfer intermedio.

### Asignador por clases

```
clase_alloc(c):
    lock(clase[c].mutex)
    si lista_libre vacía:
        span = reserva_tomar()            // toma el mutex de reserva; NULL si vacía
        si span == NULL:
            unlock; request_refill(); return NULL
        tallar span en objetos de size[c]; todos a lista_libre
    obj = pop(lista_libre); unlock
    return obj

clase_free(c, obj): lock; push(lista_libre, obj); unlock
```

- La lista libre es intrusiva: los primeros 8 bytes de un objeto libre guardan el siguiente puntero. Tamaño mínimo de objeto 256, así que cabe.
- `reserva_tomar` saca un span de una pila. Cuando quedan menos de 2 spans en la reserva, llama a `request_refill`.
- `zram_refill`: mientras `reserva < 4` y `pool_bytes + 1 MiB <= pool_limit`: pide un chunk (1 MiB), lo parte en 16 spans y los apila.

### zram\_can\_accept

Devuelve `true` si la reserva tiene al menos un span, o si `pool_bytes + ZRAM_CHUNK_BYTES <= pool_limit`. No toma locks bloqueantes (lecturas atómicas).

### zram\_free\_slots(first, count)

Para cada slot en rango: tomar stripe, poner `EMPTY`, soltar, y devolver el objeto si lo había. Ignora índices fuera de rango y slots ya vacíos.

---

## 6. Tareas

### T0. Verificaciones previas `[HAIKU-SRC]`

Solo lectura del código fuente de Haiku (no hace falta compilar). Responde cada punto con ruta de archivo, línea y la declaración real. Anótalo todo en "Hallazgos".

| # | Qué verificar | Dónde buscar |
| --- | --- | --- |
| 0.1 | Nombre exacto del flag de vectores físicos (`B_PHYSICAL_IO_REQUEST` u otro) y cómo se distingue una `generic_io_vec` física de una virtual | `headers/private/kernel/vfs.h`, `src/system/kernel/device_manager/IORequest.h`, `src/system/kernel/vm/vm_page.cpp` (`page_writer`) |
| 0.2 | Firmas de `vm_memcpy_from_physical` y `vm_memcpy_to_physical` | `headers/private/kernel/vm/vm.h` |
| 0.3 | API de mutex y semáforos usable desde el kernel (`mutex_init`, `mutex_lock`, `create_sem`, `acquire_sem`, `release_sem`) | `headers/private/kernel/lock.h`, `headers/os/kernel/OS.h` |
| 0.4 | Cómo obtener memoria cableada y no bloqueante de 1 MiB (`create_area` con `B_FULL_LOCK`, o API de páginas). Confirmar que no entra en un `panic` bajo presión | `headers/private/kernel/vm/vm.h`, `vm_page.h`, ejemplos en otros subsistemas |
| 0.5 | `smp_get_num_cpus()` y cabecera | `headers/private/kernel/smp.h` |
| 0.6 | Cómo se añaden fuentes al kernel en Jam (lista de `KernelMergeObject kernel_vm.o`) | `src/system/kernel/vm/Jamfile` |
| 0.7 | `DoublyLinkedList` y su iteración en `swap_file_list` (ya usada en `VMAnonymousCache.cpp`) | `headers/private/kernel/util/DoublyLinkedList.h` |
| 0.8 | Ruta del archivo de ajustes `virtual_memory` y funciones de lectura (`load_driver_settings`, `get_driver_parameter`) | Ya usadas en `swap_init_post_modules()` (**VERIFICADO** que existen) |
| 0.9 | Cómo se invoca un `AsyncIOCallback` al terminar una escritura asíncrona síncronamente (llamar a `IOFinished(B_OK, false, bytes)` directamente) | `src/system/kernel/vm/VMAnonymousCache.cpp`, `WriteCallback` |
| 0.10 | Máximo de áreas del kernel y coste de crear muchas (para decidir chunks de 1 MiB) | `src/system/kernel/vm/vm.cpp` |

**Aceptación:** tabla completa con ruta, línea y declaración citada. Cualquier discrepancia con lo asumido en este documento se anota y se decide con una persona antes de seguir.

---

### T1. Esqueleto del núcleo + LZ4 `[HOST]`

**Archivos:** `zram_core.h`, `zram_core.cpp` (vacío con stubs), `lz4/*`, `tests/zram_host/Makefile`, `platform_host.cpp`.

1. Copiar `lz4.c` y `lz4.h` de la versión 1.9.x oficial sin modificar, con su `LICENSE`.
2. Compilar `lz4.c` con `-DLZ4_HEAPMODE=0`. No usar nunca `LZ4_createStream` ni nada que haga `malloc`.
3. En `zram_core.h`: las constantes y structs de §4, `static_assert(sizeof(ZramSlot) == 16)` y `static_assert(ZRAM_PAGE_SIZE == 4096)`.
4. `platform_host.cpp`: implementar `ZramPlatformOps` con `aligned_alloc`, `pthread_mutex_t` y un semáforo hecho con `pthread_cond_t`. Debe incluir contadores de asignaciones vivas y una variable global `g_fail_chunk_alloc` para simular falta de memoria.
5. `Makefile` con objetivos `test`, `asan`, `tsan` (`-fsanitize=address,undefined` y `-fsanitize=thread`).

**Aceptación:** `make test` compila y ejecuta un test que comprime y descomprime 1000 páginas aleatorias con LZ4 directamente y verifica igualdad.

---

### T2. Asignador por clases `[HOST]`

**Archivos:** `zram_core.cpp` (sección del asignador).

Implementar según §5: clases, lista libre intrusiva, reserva de spans, `zram_refill`, `zram_can_accept`.

**Pruebas** (`test_allocator.cpp`):

- Asignar y liberar 100 000 objetos de clases aleatorias; verificar que no hay dos objetos solapados (guardar un mapa de rangos) y que cada puntero está alineado a 8.
- Con `g_fail_chunk_alloc = true`, `clase_alloc` devuelve `NULL` y llama a `request_refill`.
- Tras liberar todo, `stored_bytes == 0`.
- Cada objeto asignado se rellena con un patrón dependiente de su dirección y se verifica antes de liberarlo (detecta corrupción por solape).

**Aceptación:** pasa bajo ASan y UBSan sin errores.

---

### T3. Tabla de slots, escritura y lectura `[HOST]`

**Archivos:** `zram_core.cpp` (resto), `test_roundtrip.cpp`.

Implementar `zram_create`, `zram_destroy`, `zram_ctx_get/put`, `zram_write_page`, `zram_read_page`, `zram_free_slots`, `zram_get_stats` y `zram_check_invariants` tal como en §5.

**Pruebas obligatorias:**

| Prueba | Qué comprueba |
| --- | --- |
| Patrones | Páginas de ceros, de una palabra repetida distinta de cero, aleatorias (incompresibles), texto repetitivo y mezcla. Lectura devuelve exactamente lo escrito. |
| Estados | Ceros y repetidas quedan `SAME` sin consumir objetos. Aleatorias quedan `RAW`. Texto queda `LZ4` con `size < 3072`. |
| Sobrescritura | Pasar un mismo slot por `SAME → LZ4 → RAW → SAME` y comprobar que `stored_bytes` vuelve a su línea base y que no hay fugas. |
| Falta de memoria | Con `g_fail_chunk_alloc` y pool agotado, `zram_write_page` devuelve `NOMEM` y el contenido previo del slot sigue legible. |
| Idempotencia | Llamar dos veces a `zram_free_slots` sobre el mismo rango no altera contadores. Rangos fuera de límites se ignoran sin fallar. |
| Invariantes | `zram_check_invariants` tras cada prueba: la suma de `pages_*` coincide con el recuento real de la tabla, y `stored_bytes` coincide con la suma de clases. |
| Destrucción | Tras `zram_destroy`, el contador de asignaciones vivas del host es 0. |

**Aceptación:** todo pasa con `make asan`.

---

### T4. Concurrencia y fuzz `[HOST]`

**Archivos:** `test_concurrency.cpp`, `test_fuzz.cpp`.

- **Concurrencia:** 8 hilos; cada hilo es dueño de un subconjunto disjunto de slots y mantiene su "verdad" local (hash de la última página escrita). Operaciones aleatorias: escribir, leer y verificar, liberar. Otro hilo ejecuta `zram_refill` en bucle simulando el hilo de reposición. 5 minutos sin discrepancias.
- **Lecturas sobre slots compartidos:** además, un grupo de hilos solo lee slots que otros reescriben constantemente; la lectura debe devolver siempre una de las versiones válidas completas (nunca mezcla ni datos parciales). Cada versión incluye su hash dentro de la página para poder comprobarlo.
- **Fuzz de descompresión:** una función de prueba (declarada solo bajo `#ifdef ZRAM_TESTING`) corrompe bytes del objeto almacenado de un slot `LZ4`. `zram_read_page` debe devolver `CORRUPT` o datos distintos, pero nunca fallar ni leer fuera de límites bajo ASan.

**Aceptación:** `make tsan` y `make asan` limpios; sin deadlocks (el test tiene timeout).

---

### T5. Medición de ratio y velocidad `[HOST]`

**Archivo:** `bench_ratio.cpp`.

Comprimir y descomprimir con el núcleo (no con LZ4 suelto) estos corpus: ejecutables del sistema, texto, páginas de memoria reales (volcado de un proceso con `/proc/<pid>/mem` en Linux o un archivo de ejemplo), datos aleatorios.

Informar: ratio `orig_bytes / stored_bytes`, MB/s de escritura y lectura, y porcentaje de páginas `SAME`, `RAW` y `LZ4`.

**Aceptación:** el ratio sobre datos de texto y binarios es ≥ 2:1 y la lectura supera 1 GB/s por hilo. Si no, revisar el cálculo de clases antes de seguir.

**Punto de control humano:** revisar los números antes de pasar a las tareas de kernel.

---

### T6. Pegamento del kernel `[HAIKU]`

**Archivos:** `zram_kernel.h`, `zram_kernel.cpp`, `Jamfile`.

Usando solo las APIs confirmadas en T0, implementar:

```
// zram_kernel.h
struct ZramDevice;
status_t zram_kernel_create(uint64 diskBytes, uint64 poolLimit, ZramDevice** out);

// pos = desplazamiento en bytes dentro del dispositivo (slot * 4096)
status_t zram_swap_read (ZramDevice*, off_t pos, const generic_io_vec* vecs,
                         size_t count, uint32 flags, generic_size_t* numBytes);
status_t zram_swap_write(ZramDevice*, off_t pos, const generic_io_vec* vecs,
                         size_t count, uint32 flags, generic_size_t* numBytes);
void     zram_kernel_free(ZramDevice*, swap_addr_t firstSlot, uint32 count);
bool     zram_kernel_can_accept(ZramDevice*);
```

**Implementación de** **`zram_swap_write`:**

```
ctx = zram_ctx_get(dev)
slot = pos / 4096
para cada vec en vecs:
    asegurar vec.length % 4096 == 0
    para cada página k de ese vec:
        origen: si vec es físico → vm_memcpy_from_physical(zram_ctx_page(ctx), base + k*4096, 4096, false)
                     y pasar zram_ctx_page(ctx) al núcleo
                si es virtual del kernel → pasar (void*)(base + k*4096) directamente
        r = zram_write_page(dev, ctx, slot, origen)
        si r != OK: zram_ctx_put; devolver B_NO_MEMORY (o B_ERROR si CORRUPT/INVAL)
        slot++
zram_ctx_put(dev, ctx)
*numBytes = total
```

`zram_swap_read` es análogo: lee en el búfer de contexto y copia a destino con `vm_memcpy_to_physical`, o lee directo si el destino es virtual del kernel.

**Hilo de reposición:**

- Creado en `zram_kernel_create` con `spawn_kernel_thread`. Duerme en un semáforo que `request_refill` libera.
- Al despertar, llama a `zram_refill` en bucle hasta que devuelva "nada que hacer".
- `chunk_alloc` pide memoria cableada de 1 MiB (según T0.4). Debe devolver `NULL` sin bloquear ni entrar en pánico si no hay memoria.
- La primera reposición se hace de forma síncrona dentro de `zram_kernel_create` (fuera de la ruta de E/S) para que la reserva nazca llena.

**Comando de depuración** `zram` (con `add_debugger_command_etc`): imprime `ZramStats`, `pool_bytes`, ratio y estado de la reserva.

**Autotest opcional** bajo `#if ZRAM_KERNEL_SELFTEST`: al crear el dispositivo, escribe y lee 1000 páginas de patrón conocido y hace `panic` si no coinciden.

**Aceptación:** compila dentro del kernel de Haiku para x86\_64 y, con el autotest activado, el sistema arranca y el autotest pasa.

---

### T7. Parche en `VMAnonymousCache.cpp` `[HAIKU]`

Hacer exactamente estos cambios y ninguno más. Después del parche, con zram desactivado el comportamiento debe ser idéntico al original.

**7.1 Estructura** **`swap_file`:** añadir `struct ZramDevice* zram;` y ponerlo a `NULL` en `swap_file_add`.

**7.2** **`swap_slot_alloc`:** al principio, bajo `sSwapFileListLock`, antes del bucle round-robin:

```
for (SwapFileList::Iterator it = sSwapFileList.GetIterator();
        swap_file* f = it.Next();) {
    if (f->zram == NULL || !zram_kernel_can_accept(f->zram))
        continue;
    swap_addr_t a = radix_bitmap_alloc(f->bmp, count);
    if (a != SWAP_SLOT_NONE) {
        mutex_unlock(&sSwapFileListLock);
        return a + f->first_slot;
    }
}
```

El bucle existente sigue intacto para el resto de archivos. Importante: el bucle round-robin actual puede elegir un archivo zram; añadir en él `if (sSwapFileAlloc->zram != NULL && !zram_kernel_can_accept(sSwapFileAlloc->zram)) { avanzar; continue; }` para que un zram lleno no se vuelva a elegir.

**7.3** **`swap_slot_dealloc`:** justo después de calcular `slotIndex -= swapFile->first_slot` y antes de liberar en el bitmap, añadir:

```
if (swapFile->zram != NULL)
    zram_kernel_free(swapFile->zram, slotIndex, count);
```

**7.4** **`VMAnonymousCache::Read`:** tras obtener `swapFile` y `pos`, si `swapFile->zram != NULL` llamar a `zram_swap_read(swapFile->zram, pos, vecs + i, j - i, flags, _numBytes)` en lugar de `vfs_read_pages`.

**7.5** **`VMAnonymousCache::Write`:** igual con `zram_swap_write` en lugar de `vfs_write_pages` (conservar el manejo de error existente).

**7.6** **`VMAnonymousCache::WriteAsync`:** si `swapFile->zram != NULL`: llamar a `zram_swap_write` de forma síncrona y terminar con `callback->IOFinished(status, false, numBytes)` (o la firma confirmada en T0.9). `WriteCallback::IOFinished` ya se encarga de construir el bloque de swap o liberar el slot si hubo error.

**7.7** **`swap_zram_add(uint64 diskBytes, uint64 poolLimit)`:** nueva función junto a `swap_file_add`:

- Crea el `ZramDevice` con `zram_kernel_create`.
- Reserva un `swap_file` con `fd = -1`, `vnode = NULL`, `cookie = NULL`, `zram = dev`, un `radix_bitmap_create(pageCount)` y los slots `first_slot`/`last_slot` con la misma regla de hueco de una página que `swap_file_add`.
- Suma `diskBytes` a `sAvailSwapSpace`.
- Lo añade a `sSwapFileList` y aumenta `sSwapFileCount`.

**7.8** **`swap_init_post_modules`:** al **inicio** de la función (antes de la comprobación `gReadOnlyBootDevice`, para que zram funcione también en medios de solo lectura), leer `virtual_memory` con `load_driver_settings`. Si existen `zram_size` y `zram_pool_limit` con valores válidos (≥ 16 MiB, `pool_limit` múltiplo de 1 MiB), llamar a `swap_zram_add`. Descargar los ajustes con `unload_driver_settings` y continuar con la lógica original.

**7.9** **`dump_swap_info`:** si el archivo es zram, imprimir "zram" en lugar del vnode.

**7.10** **`swap_file_delete`:** rechazar (devolver `B_NOT_SUPPORTED`) cualquier archivo con `zram != NULL`.

**Aceptación:**

- Con los ajustes ausentes, el sistema arranca y se comporta como antes (`swap` en el depurador muestra solo `/var/swap`).
- Con `zram_size` y `zram_pool_limit` presentes, `swap` en el depurador muestra el archivo zram primero y `/var/swap` después.

---

### T8. Pruebas funcionales en Haiku x86\_64 `[HAIKU]`

Entorno: VM x86\_64 con 1 GiB de RAM, `zram_size = 512 MiB`, `zram_pool_limit = 256 MiB`.

Programa de prueba de usuario `zram_stress` (C++, solo APIs POSIX):

1. Reserva `N = 1.5 × RAM` con `mmap` anónimo.
2. Escribe en cada página un patrón derivado de `(semilla, número de página)`, con tres tipos de contenido mezclados: ceros, texto repetitivo y bytes pseudoaleatorios.
3. Duerme 30 s para dar tiempo al `page_daemon`.
4. Lee todas las páginas en orden aleatorio y verifica el patrón. Cualquier discrepancia termina con código distinto de 0 e imprime la dirección.
5. Repite el ciclo 20 veces con semillas distintas.

**Aceptación:**

- 20 ciclos sin discrepancias ni `panic`.
- El comando `zram` muestra `pages_lz4 + pages_same > 0` y `read_corrupt == 0`.
- `write_nomem` puede ser \> 0, pero entonces `/var/swap` debe estar en uso (`swap` en el depurador).

---

### T9. Prueba de presión extrema y liberación `[HAIKU]`

1. Con `zram_pool_limit` pequeño (64 MiB), ejecutar `zram_stress` con 3× RAM. Esperado: el sistema sigue vivo, zram se llena, el resto va a `/var/swap`.
2. Terminar `zram_stress`. Tras unos segundos, `stored_bytes` del comando `zram` debe bajar a un valor cercano a 0, lo que confirma que `swap_slot_dealloc` libera los objetos.
3. Repetir 10 veces seguidas: `pool_bytes` no debe crecer más allá del límite y no debe haber fugas de áreas (comparar `listarea` antes y después).

**Aceptación:** los tres puntos se cumplen.

---

### T10. Comparativa de rendimiento `[HAIKU]`

Mismo `zram_stress` con tres configuraciones: solo `/var/swap`, zram + disco, zram con pool grande. Medir tiempo total y actividad de disco.

**Aceptación (objetivo, no bloqueante):** zram + disco reduce el tiempo total frente a solo disco en al menos 30 % en la VM.

---

### T11. Pulido opcional (v2, no empezar sin autorización)

- Devolver spans totalmente libres al sistema (requiere contador de objetos vivos por span).
- Comando de usuario o syscall para activar zram sin reiniciar.
- Compactación del pool.
- Selección de algoritmo (ZSTD).
- Estadísticas visibles desde `ProcessController` o `vmstat`.

---

## 7. Reglas innegociables

1. Cero llamadas a `malloc`, `new`, `create_area` ni cualquier cosa que pueda bloquear dentro de `zram_write_page`, `zram_read_page`, `zram_free_slots`, `zram_swap_read` y `zram_swap_write`.
2. Cero memoria en la pila de más de 256 bytes en cualquiera de esas funciones. Los búferes de 4 KiB viven en `ZramCtx`.
3. Un fallo de escritura nunca altera el contenido previo de un slot ni deja objetos huérfanos.
4. Todo puntero y tamaño que llega a una función pública se valida (`slot < slot_count`, `vec.length % 4096 == 0`).
5. El orden de locks de §5 es ley. Cualquier cambio requiere revisión humana.
6. Con zram desactivado por configuración, el código de swap original debe comportarse igual que antes del parche.
7. Nada de ZSTD, compactación ni threads adicionales en v1.

---

## 8. Riesgos y mitigaciones

| Riesgo | Probabilidad | Mitigación |
| --- | --- | --- |
| `chunk_alloc` en el kernel no puede garantizarse no bloqueante | Media | T0.4 lo decide antes de escribir código. Alternativa: usar memoria obtenida al crear el dispositivo (reservar el pool completo en `zram_kernel_create`, a costa de RAM fija). |
| Los vectores de swap no son siempre físicos o no están alineados a página | Baja | T0.1 y las aserciones de T6. Si aparece un caso, devolver `B_BAD_VALUE` y registrar con `dprintf`. |
| Presión de memoria hace que `write_nomem` sea frecuente | Media | Es seguro (la página queda sucia), y `zram_can_accept` evita elegir zram cuando no hay espacio. Medir en T9. |
| El pool nunca se encoge, así que tras un pico la RAM queda reservada | Alta | Aceptado en v1. Documentado en T11. El límite `zram_pool_limit` acota el daño. |
| Cambios en `VMAnonymousCache.cpp` entre revisiones de Haiku | Media | El parche toca 7 puntos concretos. Escribirlo como serie de commits pequeños sobre la revisión exacta usada. |
| Pérdida de datos por bug en el asignador | Alta si no se prueba | T2 a T4 son obligatorias y con sanitizers antes de tocar el kernel. |

---

## 9. Qué puede hacer el implementador solo y qué no

| Tarea | Autonomía del LLM | Necesita persona |
| --- | --- | --- |
| T0 | Alta si tiene acceso al árbol de fuentes | Revisar discrepancias |
| T1 a T5 | Total (se prueba en host) | Revisar los números de T5 |
| T6, T7 | Escritura de código completa | Compilar el kernel y corregir errores de build |
| T8 a T10 | Escribir `zram_stress` | Arrancar la VM, ejecutar y reportar resultados |

**Orden recomendado:** T0 → T1 → T2 → T3 → T4 → T5 → (punto de control humano) → T6 → T7 → T8 → T9 → T10.

---

## 10. Entorno

- **Host de pruebas (T1 a T5):** Linux o macOS con `g++ >= 11`, `make`. No depende de Haiku.
- **Compilación de Haiku:** requiere el árbol de Haiku y sus herramientas cruzadas (`buildtools`), normalmente en Linux o dentro de Haiku. Usar x86\_64 como objetivo principal.
- **Ejecución:** QEMU o UTM con la imagen x86\_64. Es más lento en un Mac con Apple Silicon porque emula x86\_64, pero es la plataforma estable. Dejar ARM64 para una validación posterior; el núcleo (capa 1) es portable y no cambia.

---

## 11. Hallazgos (rellenar durante T0 y durante el desarrollo)

| Fecha | Tarea | Hallazgo | Archivo y línea | Impacto en el plan |
| --- | --- | --- | --- | --- |
|  |  |  |  |  |

---

## 12. Definición de "terminado" para v1

- \[ \] T1 a T5 pasan en el host bajo ASan, UBSan y TSan.
- \[ \] El parche T7 aplica sobre la revisión de Haiku elegida y no cambia nada con zram desactivado.
- \[ \] `zram_stress` completa 20 ciclos sin discrepancias en VM x86\_64.
- \[ \] Liberación de objetos al terminar procesos confirmada (T9).
- \[ \] Sin fugas de áreas tras 10 ciclos de presión.
- \[ \] Estadísticas legibles con el comando `zram` del depurador del kernel.
- \[ \] README breve con los ajustes `zram_size` y `zram_pool_limit` y un ejemplo de archivo `virtual_memory`.
