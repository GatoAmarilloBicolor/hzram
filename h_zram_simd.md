# h\_zram: diseño de optimizaciones SIMD

Complemento de `h_zram_plan_implementacion.md` (plan v2) y `h_zram_gobernador.md`. Mismas convenciones: 

---

## 1. Principios

1. **Medir antes de optimizar.** Solo se activa una optimización SIMD si el perfil de S1 muestra que esa operación consume más del 5 % del tiempo de CPU por página y el kernel SIMD la acelera al menos un 15 % de extremo a extremo.
2. **Siempre existe una versión escalar de referencia.** Es la que se usa como oráculo en las pruebas y como respaldo en ejecución. Si SIMD no es seguro en un contexto, se usa la escalar.
3. **SIMD no cambia el formato de datos.** Cualquier nivel (escalar, SSE2, NEON) lee lo que escribió cualquier otro. Se puede cambiar de nivel en caliente sin migrar nada.
4. **El kernel no es un programa de usuario.** Usar registros vectoriales dentro del kernel puede corromper el estado de un proceso de usuario. Por eso hay una guarda explícita (§5) y SIMD solo se activa donde sea seguro.
5. **Primero lo que no es SIMD.** Varias de las mejoras con mejor relación beneficio/riesgo no usan vectores (§3.1). Se hacen antes.

---

## 2. Dónde puede ayudar SIMD (y dónde no)

El tiempo de una escritura a zram se reparte aproximadamente así (hipótesis a verificar en S1): la compresión LZ4 domina, y el resto son operaciones pequeñas alrededor.

| Operación | ¿SIMD ayuda? | Razonamiento |
| --- | --- | --- |
| Detección de página repetida (`SAME`) | Sí, en las páginas que sí son repetidas | La mayoría de páginas no repetidas salen en la primera línea de caché con un chequeo escalar. SIMD acelera el escaneo completo de las páginas realmente repetidas (ceros, patrones) |
| Copia de objeto comprimido al pool | Sí, posiblemente | Con stores no temporales se evita ensuciar la caché con datos fríos. El efecto real es incierto |
| Compresión LZ4 | Poco o nada propio | LZ4 ya es muy eficiente en C. Compilarlo con SSE2/NEON activado puede dar una mejora modesta (S5). Reescribirlo con intrinsics queda fuera de v1 |
| Descompresión LZ4 | Poco | Ya es de 2 a 4 GB/s; el cuello suele ser la memoria. Además corre en el contexto del fallo de página (peligroso para SIMD, §5) |
| Copia página ↔ búfer de contexto | Mejor eliminarla (§3.1) | Una copia de 4 KiB cuesta menos que la llamada a la guarda; lo ideal es no copiar |
| Cribado previo de incompresibilidad | Dudoso | LZ4 ya abandona rápido datos aleatorios y el predictor del gobernador ya evita enviarlos. Solo se implementa si S1 lo justifica |

---

## 3. Catálogo de optimizaciones

### 3.1 Sin SIMD (se hacen primero)

| Id | Optimización | Dónde | Notas |
| --- | --- | --- | --- |
| O1 | **Cero copia desde la página física.** En lugar de copiar la página a `ctx->page` con `vm_memcpy_from_physical`, mapear la página física y comprimir directamente desde ella; en lectura, descomprimir directamente sobre la página destino mapeada | Pegamento (T6) | Ahorra una copia de 4 KiB por operación. Requiere confirmar la API de mapeo temporal en S0 |
| O2 | **Rechazo rápido por primera línea** en `same_fill`: comparar 8 palabras de 64 bits antes de escanear el resto | Núcleo | La gran mayoría de páginas no repetidas terminan aquí |
| O3 | **Prefetch** del siguiente slot y del siguiente objeto comprimido al procesar varias páginas por llamada (`Write` síncrono con hasta 32 páginas) | Núcleo | `__builtin_prefetch`. Solo útil con lotes |
| O4 | **Alineación y relleno de líneas de caché**: `ZramCtx` y sus búferes alineados a 64 bytes; las clases de tamaño ya son múltiplos de 256, así que los objetos quedan alineados | Núcleo | Es un requisito previo de los kernels SIMD |
| O5 | **Búsqueda de clase sin ramas**: tabla de 16 entradas indexada por `(n + 255) >> 8` en lugar de un bucle | Núcleo | Trivial, evita 9 comparaciones |

### 3.2 Con SIMD

| Id | Kernel | x86\_64 | ARM64 | Prioridad |
| --- | --- | --- | --- | --- |
| K1 | `same_fill(page, &pattern)` | SSE2 (16 B); AVX2 opcional | NEON (16 B) | Alta |
| K2 | `copy_to_pool(dst, src, n)` con stores no temporales cuando `n >= 1024` | SSE2 `movntdq` | Sin equivalente portable; usar copia normal | Media |
| K3 | LZ4 compilado en una unidad con SIMD habilitado | `-msse2` | sin `-mgeneral-regs-only` | Media, solo si S5 lo justifica |
| K4 | Cribado previo de incompresibilidad | SSE2 | NEON | Baja, solo si S1 lo justifica |

**Decisión sobre AVX2:** queda como experimento opcional, desactivado por defecto. Requiere guardar un estado vectorial mayor (XSAVE), añade `vzeroupper` y en algunas CPUs puede bajar la frecuencia. Para operaciones de 4 KiB la ganancia sobre SSE2 es pequeña.

---

## 4. Arquitectura de dispatch

```
// zram_simd.h  (sin includes del kernel)
enum ZramSimdTier : uint8_t { ZSIMD_SCALAR = 0, ZSIMD_SSE2, ZSIMD_AVX2, ZSIMD_NEON };

struct ZramKernels {
    ZramSimdTier tier;
    // Devuelve true si todas las palabras de 8 bytes son iguales; *pattern recibe el valor.
    bool (*same_fill)(const void* page4k, uint64_t* pattern);
    // Copia n bytes (n multiplo de 16, dst alineado a 16) a memoria fría del pool.
    void (*copy_to_pool)(void* dst, const void* src, size_t n);
    // Compresión LZ4 con la misma semántica que LZ4_compress_fast_extState.
    int  (*lz4_compress)(void* state, const char* src, char* dst,
                         int srcSize, int dstCap, int accel);
    int  (*lz4_decompress)(const char* src, char* dst, int cSize, int dstSize);
    bool needs_guard;              // true si hay que llamar a simd_enter/leave alrededor
};

const ZramKernels* zram_kernels_scalar(void);          // siempre disponible
const ZramKernels* zram_kernels_sse2(void);            // NULL si no se compiló o no hay soporte
const ZramKernels* zram_kernels_neon(void);
const ZramKernels* zram_kernels_select(uint32_t cpu_features, ZramSimdTier forced /*0=auto*/);
```

**Selección:** al crear el dispositivo, una sola vez. El resultado se guarda en `ZramDevice::kernels` (puntero constante). La ruta de E/S nunca decide en caliente; usa `dev->kernels->...`.

**Orden de preferencia en** **`auto`:** NEON en ARM64; SSE2 en x86\_64; escalar si no hay soporte, si la guarda no está disponible o si el ajuste `zram_simd = off`.

### Estructura de archivos

```
src/system/kernel/vm/zram/
    zram_simd.h
    zram_kernels_scalar.cpp      # SWAR, sin intrinsics, compila en cualquier arquitectura
    zram_kernels_sse2.cpp        # solo x86_64, con -msse2
    zram_kernels_neon.cpp        # solo arm64
    zram_lz4_simd_sse2.cpp       # K3 (opcional): envuelve lz4.c con SIMD habilitado
    zram_lz4_simd_neon.cpp       # K3 (opcional)
tests/zram_host/
    test_simd_equiv.cpp
    bench_simd.cpp
```

---

## 5. Seguridad de SIMD dentro del kernel

### El problema

Cuando un proceso de usuario entra al kernel (llamada al sistema, fallo de página, interrupción), **sus registros vectoriales siguen vivos**. Si el kernel los usa sin guardarlos, al volver el programa de usuario tendrá datos corruptos, y el fallo será intermitente y casi imposible de depurar.

### Asimetría entre escritura y lectura

| Camino | Quién lo ejecuta | Riesgo para SIMD |
| --- | --- | --- |
| Escritura (`WriteAsync`, `Write`) | Normalmente el hilo `page_writer`, que es un hilo del kernel sin estado de usuario | Bajo, pero hay que confirmar que no lo llaman otros contextos (S0.4) |
| Lectura (`Read`) | El hilo de usuario que provocó el fallo de página | **Alto.** Los registros vectoriales del usuario están vivos |

**Decisión para v1:**

- La **escritura** puede usar SIMD (K1, K2, K3-compresión) con guarda.
- La **lectura** usa **solo el camino escalar**, salvo que S0 demuestre una guarda segura y el perfil la justifique.

### Interfaz de guarda (plataforma)

```
struct ZramSimdCtx {                     // área de guardado, alineada a 64 B
    alignas(64) uint8_t area[1024];      // 512 B para fxsave (x86_64) o 512 B para v0-v31 (arm64)
    bool saved;
};

// Devuelve false si SIMD no es seguro en este contexto: el llamador usa la versión escalar.
bool simd_enter(ZramSimdCtx*);
void simd_leave(ZramSimdCtx*);
```

Añadir a `ZramPlatformOps`: `uint32_t (*cpu_features)(void)`, `bool (*simd_enter)(ZramSimdCtx*)`, `void (*simd_leave)(ZramSimdCtx*)`.

### Reglas de `simd_enter` (kernel)

```
simd_enter(ctx):
    si el hilo actual NO es un hilo del kernel: return false
    si estamos en contexto de interrupción o con interrupciones deshabilitadas: return false
    si la CPU no soporta el nivel elegido: return false
    guardar el estado vectorial en ctx->area        // x86_64: fxsave; arm64: stp q0..q31
    ctx->saved = true
    return true

simd_leave(ctx):
    si !ctx->saved: return
    x86_64: restaurar con fxrstor (el estado AVX no se usa en v1)
    arm64 : ldp q0..q31
    ctx->saved = false
```

El coste de guardar y restaurar 512 B es del orden de decenas de ciclos, despreciable frente al trabajo de una página de 4 KiB. **Se hace una sola entrada/salida por escritura de página** (o por lote en `Write` síncrono), nunca por kernel individual.

### Cuando la guarda no está disponible

Si S0 concluye que no hay forma segura de usar SIMD en el kernel de la arquitectura objetivo, `simd_enter` devuelve siempre `false`, se usa la ruta escalar (SWAR + O1 a O5) y los kernels SIMD quedan compilados solo para el host. El diseño sigue siendo válido y no hay que cambiar nada más.

---

## 6. Especificación de los kernels

### Contrato común

- `page4k` y `src` pueden no estar alineados: usar cargas no alineadas (`loadu`, `vld1q`).
- `dst` en `copy_to_pool` está alineado a 64 B (garantizado por el asignador).
- **Los resultados de cada nivel deben ser idénticos al escalar** para `same_fill` y `copy_to_pool`. Para compresión solo se exige que **cualquier decodificador LZ4 lea el resultado y recupere los datos originales**.

### K1: `same_fill`

Algoritmo común a todos los niveles:

```
p = primera palabra de 64 bits
comprobar las palabras 1 a 7 (primera línea de caché) con comparación escalar  → si alguna difiere: false
resto de la página, en bloques de 512 B:
    acumulador = OR de (vector XOR vector_patrón) sobre el bloque
    si el acumulador no es cero: false
true, *pattern = p
```

**SSE2 (x86\_64):**

```
static bool same_fill_sse2(const void* page, uint64_t* pattern)
{
    const uint64_t* w = (const uint64_t*)page;
    const uint64_t p = w[0];
    if (w[1]!=p || w[2]!=p || w[3]!=p || w[4]!=p || w[5]!=p || w[6]!=p || w[7]!=p)
        return false;

    const __m128i ref  = _mm_set1_epi64x((long long)p);
    const __m128i zero = _mm_setzero_si128();
    const __m128i* v   = (const __m128i*)page;

    for (int blk = 0; blk < 8; blk++) {                // 8 bloques de 512 B
        __m128i acc = _mm_setzero_si128();
        for (int i = 0; i < 32; i += 4) {              // 4 vectores = 64 B por vuelta
            __m128i a = _mm_xor_si128(_mm_loadu_si128(v + blk*32 + i + 0), ref);
            __m128i b = _mm_xor_si128(_mm_loadu_si128(v + blk*32 + i + 1), ref);
            __m128i c = _mm_xor_si128(_mm_loadu_si128(v + blk*32 + i + 2), ref);
            __m128i d = _mm_xor_si128(_mm_loadu_si128(v + blk*32 + i + 3), ref);
            acc = _mm_or_si128(acc, _mm_or_si128(_mm_or_si128(a, b), _mm_or_si128(c, d)));
        }
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(acc, zero)) != 0xFFFF)
            return false;
    }
    *pattern = p;
    return true;
}
```

**NEON (arm64):**

```
static bool same_fill_neon(const void* page, uint64_t* pattern)
{
    const uint64_t* w = (const uint64_t*)page;
    const uint64_t p = w[0];
    if (w[1]!=p || w[2]!=p || w[3]!=p || w[4]!=p || w[5]!=p || w[6]!=p || w[7]!=p)
        return false;

    const uint64x2_t ref = vdupq_n_u64(p);
    for (int blk = 0; blk < 8; blk++) {
        uint64x2_t acc = vdupq_n_u64(0);
        const uint64_t* q = w + blk * 64;
        for (int i = 0; i < 64; i += 8) {              // 4 vectores de 16 B = 64 B
            acc = vorrq_u64(acc, veorq_u64(vld1q_u64(q + i + 0), ref));
            acc = vorrq_u64(acc, veorq_u64(vld1q_u64(q + i + 2), ref));
            acc = vorrq_u64(acc, veorq_u64(vld1q_u64(q + i + 4), ref));
            acc = vorrq_u64(acc, veorq_u64(vld1q_u64(q + i + 6), ref));
        }
        if (vmaxvq_u32(vreinterpretq_u32_u64(acc)) != 0)
            return false;
    }
    *pattern = p;
    return true;
}
```

**Escalar (SWAR, referencia):** bucle de 512 comparaciones de `uint64_t` con salida temprana en cada bloque de 64 palabras.

**AVX2 (opcional):** misma estructura con vectores de 32 B (`_mm256_*`), `_mm256_testz_si256` para el test y `_mm256_zeroupper()` antes de salir.

### K2: `copy_to_pool`

```
si n < 1024: copia normal (memcpy escalar)
sino:
    x86_64: bucle de _mm_loadu_si128 + _mm_stream_si128 (16 B por vuelta) y _mm_sfence() al final
    arm64 : copia normal (vld1q/vst1q), sin stores no temporales
```

**Precondición:** `n` se redondea hacia arriba a múltiplo de 16 antes de llamar; el objeto del pool siempre es al menos tan grande como ese valor (las clases son múltiplos de 256).

**Por qué solo para n ≥ 1024:** los datos del pool son fríos (se leerán mucho después, si se leen), así que no conviene que desplacen a la caché; pero para copias pequeñas el coste del `sfence` supera el beneficio.

### K3: LZ4 con SIMD habilitado

Se compila `lz4.c` en una unidad aparte con flags distintos, **sin modificarlo**, y sin colisión de símbolos:

```
// zram_lz4_simd_sse2.cpp   (se compila con -msse2)
#define LZ4LIB_API static          // todas las funciones públicas pasan a ser privadas de esta unidad
#define LZ4_HEAPMODE 0
#include "lz4/lz4.c"

int zram_lz4_compress_sse2(void* st, const char* s, char* d, int n, int cap, int accel)
{ return LZ4_compress_fast_extState(st, s, d, n, cap, accel); }
int zram_lz4_decompress_sse2(const char* s, char* d, int c, int n)
{ return LZ4_decompress_safe(s, d, c, n); }
```

(En ARM64: igual, en una unidad compilada sin la restricción de solo registros generales.)

**Compatibilidad:** al ser LZ4 estándar, el flujo comprimido es legible por la versión escalar.

**Se adopta solo si S5 muestra al menos un 10 % de mejora en compresión** sobre el corpus real. La descompresión se queda en la versión escalar (§5).

### K4: cribado previo (no se implementa salvo que S1 lo pida)

Idea: muestrear 256 posiciones de la página y estimar cuántos valores de 4 bytes se repiten. Si casi ninguno, saltarse LZ4. Descartado por defecto porque LZ4 ya abandona rápidamente los datos aleatorios y el predictor del gobernador ya manda esas fuentes a disco.

---

## 7. Cambios que requiere en el resto de la arquitectura

| Elemento | Cambio |
| --- | --- |
| `ZramPlatformOps` | Añadir `cpu_features`, `simd_enter`, `simd_leave` |
| `ZramDevice` | Añadir `const ZramKernels* kernels` y contadores `simd_used`, `simd_denied` |
| `ZramCtx` | Alinear a 64 B; añadir un `ZramSimdCtx` embebido |
| `zram_write_page` | Si `kernels->needs_guard`: `ok = simd_enter(&ctx->simd)`. Con `ok` usa `kernels->*`; si no, `zram_kernels_scalar()`. Un solo `simd_leave` al terminar el lote |
| `zram_read_page` | **Sin cambios**: siempre escalar |
| `ZramStats` | Añadir `simd_tier`, `simd_used`, `simd_denied` |
| Ajustes `virtual_memory` | `zram_simd = auto \| off \| sse2 \| neon \| avx2` |
| Comando `zram` del depurador | Imprimir el nivel activo y los contadores |
| Gobernador | Sin cambios: `compress_us_ewma` refleja automáticamente la mejora |

---

## 8. Tareas

Se insertan después de G5 del gobernador y antes de T6 del plan v2.

### S0. Verificaciones previas `[HAIKU-SRC]`

| # | Qué verificar | Dónde buscar |
| --- | --- | --- |
| S0.1 | Flags con los que se compila el kernel en x86\_64 y arm64 (`-mno-sse`, `-mgeneral-regs-only`, `-mno-red-zone` u otros) | `build/jam/BuildSetup`, `build/jam/ArchitectureRules`, `src/system/kernel/Jamfile` |
| S0.2 | Si el cambio de contexto guarda el estado FPU/vectorial de todos los hilos, incluidos los del kernel | `src/system/kernel/arch/x86/64/`, `src/system/kernel/arch/arm64/`, `arch_thread*.cpp` |
| S0.3 | API del kernel para detectar características de CPU (SSE2, AVX2) y funciones disponibles para guardar/restaurar estado vectorial | `headers/private/kernel/arch/x86/arch_cpu.h` (`x86_check_feature` o equivalente) |
| S0.4 | Desde qué contextos se llama a `VMAnonymousCache::Write` y `WriteAsync` (¿solo `page_writer`?) y cómo comprobar "este hilo es del kernel" | `src/system/kernel/vm/vm_page.cpp`, `VMCache.cpp`, `headers/private/kernel/thread.h` |
| S0.5 | Si en arm64 el kernel permite usar registros FP/NEON (control de acceso a FP habilitado) | `src/system/kernel/arch/arm64/` |
| S0.6 | Cómo aplicar flags de compilación a un solo archivo en Jam (`ObjectCcFlags` u otro) con un ejemplo existente | `src/system/kernel/arch/x86/Jamfile` y similares |
| S0.7 | API de mapeo temporal de una página física a dirección virtual (para O1) y sus restricciones de contexto | `headers/private/kernel/vm/vm.h`, `vm_page.h` |

**Aceptación:** tabla con ruta, línea y declaración citada. Si S0.2 o S0.4 dan un resultado negativo (no se garantiza guardar estado, o la escritura se llama desde contextos de usuario sin control), **`simd_enter`** **devuelve siempre** **`false`** **en esa arquitectura** y se sigue con la versión escalar.

### S1. Perfil de referencia `[HOST]`

`bench_profile.cpp`: sobre el corpus de T5 (ejecutables, texto, volcados de memoria, aleatorios) medir el tiempo por página, con el núcleo actual, de:

1. detección `SAME`,
2. compresión LZ4,
3. copia al pool,
4. descompresión,
5. el total de `zram_write_page` y de `zram_read_page`.

Informar media, percentil 99 y porcentaje del total de cada fase, por tipo de contenido.

**Aceptación y puerta de decisión:** tabla publicada. Cada kernel SIMD (K1 a K4) solo continúa si su fase supera el 5 % del tiempo total de su camino.

### S2. Dispatch y kernels escalares optimizados `[HOST]`

Implementar `zram_simd.h`, `zram_kernels_scalar.cpp` (SWAR) y las optimizaciones O2, O4 y O5. Integrar el dispatch en el núcleo con `ZSIMD_SCALAR` por defecto.

**Pruebas:**

- Todas las pruebas de T3 y T4 siguen pasando.
- `same_fill` escalar contra una implementación ingenua de referencia sobre 10⁶ páginas aleatorias y estructuradas.
- Caso límite: una sola palabra distinta en cada una de las 512 posiciones posibles. Debe devolver `false` siempre.

**Aceptación:** sin regresión de rendimiento en S1 y todas las pruebas limpias con sanitizers.

### S3. Kernels SSE2 `[HOST x86_64]`

Implementar K1 y K2 en `zram_kernels_sse2.cpp` con las firmas de §4 y la especificación de §6.

**Pruebas de equivalencia** (`test_simd_equiv.cpp`), comparando contra el escalar:

- `same_fill`: 10⁶ páginas, incluyendo ceros, palabra repetida, una palabra distinta en cada posición (512 casos), diferencia en el último byte y diferencia justo en los límites de 64 B y 512 B.
- `copy_to_pool`: tamaños 16 a 4096 en pasos de 16, con origen desalineado en 0 a 15 bytes. Se comprueba que no se escribe fuera de `[dst, dst+n)` con zonas centinela.
- Mismo contenido tras interrumpir entre bloques (no hay estado persistente).

**Aceptación:** equivalencia total y ASan limpio. Además, `bench_simd` informa el factor de mejora de cada kernel frente al escalar.

**Nota para el entorno de pruebas:** si el host es un Mac con Apple Silicon, los kernels SSE2 se prueban ejecutando un binario x86\_64 mediante Rosetta (`arch -x86_64`), que soporta SSE2. AVX2 puede no estar disponible allí; para AVX2 usar un equipo x86 o una máquina virtual.

### S4. Kernel NEON `[HOST arm64]`

Implementar K1 (K2 usa copia normal) en `zram_kernels_neon.cpp`. Mismas pruebas de equivalencia que S3.

**Aceptación:** equivalencia total y mejora medida frente al escalar.

### S5. LZ4 con SIMD habilitado `[HOST]`

Implementar K3 (§6) para la arquitectura disponible. Comparar en `bench_simd` el tiempo de compresión y el ratio frente a la versión escalar sobre todo el corpus.

**Pruebas:**

- Todo flujo producido por la variante SIMD se descomprime correctamente con la versión escalar, y viceversa (10⁶ páginas).
- El tamaño comprimido no difiere más de un 1 % de media entre variantes.

**Puerta de decisión:** adoptar solo si la compresión mejora al menos un 10 % de media. Si no, se descarta K3 y se documenta en "Hallazgos".

### S6. Cero copia (O1) `[HAIKU]`

Implementar en el pegamento: mapear la página física con la API confirmada en S0.7 y pasar el puntero directamente a `zram_write_page` (y, en lectura, a `zram_read_page`). Liberar el mapeo siempre, también en rutas de error.

**Aceptación:** el autotest del kernel y `zram_stress` del plan v2 siguen pasando; el comando `zram` muestra `compress_us_ewma` menor o igual que antes.

### S7. Guarda y activación en el kernel `[HAIKU]`

Implementar `simd_enter/leave` para la arquitectura (reglas de §5) y el ajuste `zram_simd`. Conectar el dispatch en `zram_write_page`. La lectura permanece escalar.

**Aceptación:**

- Con `zram_simd = off`, comportamiento idéntico al plan v2.
- Con `zram_simd = auto`, el comando `zram` muestra el nivel activo y `simd_denied` bajo.
- **Prueba de integridad del estado de usuario:** un programa de usuario llena todos los registros vectoriales con un patrón conocido, hace presión de memoria con `zram_stress` en otro proceso, y verifica el patrón en bucle durante 10 minutos. Cualquier discrepancia es un fallo bloqueante.

### S8. Medición en Haiku `[HAIKU]`

Repetir T8 a T10 con `zram_simd = off` y `auto`.

**Aceptación (objetivo):**

- Sin discrepancias de datos en ningún ciclo.
- `compress_us_ewma` con `auto` es menor o igual que con `off`.
- Si no mejora, se desactiva `auto` por defecto y se documenta.

---

## 9. Reglas innegociables

1. Nunca ejecutar código SIMD en el kernel sin haber pasado por `simd_enter` devolviendo `true`.
2. Nunca usar SIMD en el camino de lectura (fallo de página) en v1.
3. Nunca usar SIMD en contexto de interrupción ni con interrupciones deshabilitadas.
4. Toda variante SIMD debe tener su prueba de equivalencia contra la escalar antes de poder activarse.
5. No cambiar el formato de datos en ningún nivel. El flujo comprimido debe ser siempre LZ4 estándar.
6. La versión escalar siempre se compila y siempre está disponible como respaldo.
7. No usar coma flotante en ningún kernel de este documento.
8. Ninguna optimización se acepta sin medición que la respalde (puerta de S1/S5).

---

## 10. Riesgos

| Riesgo | Probabilidad | Mitigación |
| --- | --- | --- |
| Corrupción del estado vectorial de procesos de usuario | Baja si se respeta §5, impacto muy alto | Guarda explícita, lectura solo escalar y prueba de integridad de S7 |
| La ganancia real es pequeña porque LZ4 domina | Alta | Puertas de decisión de S1 y S5; O1 a O5 se hacen primero |
| El kernel no permite usar SIMD de forma segura en alguna arquitectura | Media | S0 lo decide; la ruta escalar sigue siendo completa |
| Errores sutiles en los kernels (límites, alineación) | Media | Pruebas de equivalencia exhaustivas con centinelas y ASan |
| Dependencia de características de CPU no presentes | Baja | Selección por `cpu_features` y respaldo escalar |
| Mantenimiento de tres variantes del mismo código | Media | Contrato común y un solo banco de pruebas de equivalencia |

---

## 11. Resumen de decisiones

| Tema | Decisión |
| --- | --- |
| Niveles en v1 | Escalar (SWAR), SSE2 (x86\_64) y NEON (arm64). AVX2 queda experimental y desactivado |
| Dónde se usa SIMD | Solo en escritura. La lectura es siempre escalar |
| Kernels propios | `same_fill` (K1) y copia no temporal al pool (K2) |
| LZ4 | Se usa estándar. Variante con SIMD habilitado (K3) solo si mejora ≥ 10 % |
| Orden de trabajo | S0 → S1 → S2 → S3/S4 → S5 → S6 → S7 → S8 |
| Mejoras sin SIMD (O1 a O5) | Se hacen antes y se miden primero |
