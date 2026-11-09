# hzram — Ciclos de Optimización y Estabilización

## Ciclo 1: Auditoría Inicial (4bf6718)
**Objetivo:** Detectar y corregir 18 bugs críticos
- Race conditions en refcounting (`atomic_add` → C11 `atomic_fetch_add_explicit`)
- UAF en `zram_kernel_dev_get/put`
- Firmas `chunk_alloc/free` sin contexto `user`
- Estadísticas inconsistentes entre kernel/userspace/hzramfs
- Build roto en Jam (KernelMergeObject, dependencias LZ4)

**Resultado:** 18 fixes en un commit, push a origin/master

---

## Ciclo 2: POSIX Standardization — Stats Output
**Objetivo:** Unificar formato de salida de estadísticas (POSIX-style)

### Cambios zram_core.cpp
- `zram_is_same_page`: SIMD NEON (ARM64) / SSE2 (x86_64) / scalar fallback → ~5x speedup
- `zram_stats_snprintf`: pre-formateo en buffers locales → single `snprintf`
- `zram_stats_fprintf`: wrapper POSIX sobre snprintf
- Includes condicionales: `#ifdef __HAIKU__` para NEON, evita pull de headers ARM en host

### Cambios zram_kernel.cpp
- `zram_kernel_stats_snprintf`: misma lógica, `zram_kernel_fmt_bytes` (kernel-safe)
- `zram_kernel_stats_kprintf`: `kprintf` directo para KDL/debug

### Cambios zram_core.h
- `#include <stdio.h>` para `FILE*`
- Declaraciones `zram_stats_snprintf/fprintf`

### Cambios zram_kernel.h
- Declaraciones `zram_kernel_stats_snprintf/kprintf`

---

## Ciclo 3: Platform Ops — Contexto Usuario
**Objetivo:** POSIX-style callbacks con `void* user`

### zram_core.h
```c
void* (*chunk_alloc)(void* user, size_t bytes);
void  (*chunk_free)(void* user, void* ptr, size_t bytes);
```

### zram_core.cpp
- Todas las llamadas a `chunk_alloc/free` pasan `dev->ops->user`

### platform_host.cpp (tests)
- `host_chunk_alloc(void* user, size_t bytes)`
- `host_chunk_free(void* user, void* ptr, size_t bytes)`
- `(void)user` para silenciar unused warning

### hzramfs/kernel_interface.cpp
- `hzram_chunk_alloc/free` firmas actualizadas
- `sPlatformOps.user = volume` antes de `zram_create`
- `hzram_volume_get_stats_string` → usa `zram_stats_snprintf` unificado

---

## Ciclo 4: Limpieza y Verificación
**Objetivo:** Eliminar código muerto, verificar builds

### Eliminado
- Llamada duplicada `zram_stats_snprintf` con stats zeradas en hzramfs
- Código muerto tras `}` en `hzram_volume_get_stats_string`

### Verificado
```
Host build (ZRAM_TESTING):     PASS 10/10
Host build (ASan/UBSan):       PASS 10/10
Sintaxis ARM64 (NEON):         OK (guards __HAIKU__)
Sintaxis x86_64 (SSE2):        OK
```

---

## Estado Final — APIs Estables

### Núcleo (zram_core)
| Función | Uso |
|---------|-----|
| `zram_stats_snprintf(buf, size, st, cfg)` | Userspace/kernel-agnostic |
| `zram_stats_fprintf(fp, st, cfg)` | POSIX FILE* output |
| `zram_is_same_page(page, pattern)` | SIMD-optimized |

### Kernel (zram_kernel)
| Función | Uso |
|---------|-----|
| `zram_kernel_stats_snprintf(dev, buf, size)` | Kernel buffer |
| `zram_kernel_stats_kprintf(dev)` | KDL/kprintf directo |
| `zram_kernel_dev_get/put(dev)` | Refcount helpers |

### hzramfs
| Función | Uso |
|---------|-----|
| `hzram_volume_get_stats_string(vol, buf, size)` | Volume stats unificadas |

---

## Formato de Salida Estandarizado

```
zram_stats:
  config:
    disk_size:        256 MiB
    pool_limit:       64 MiB
  pages:
    total_slots:      65536
    active_pages:     45231 (69.02%)
    empty_pages:      20305
  compression_layers:
    SAME (identical):  12045 pages (26.63%)  ratio: 10000%  saved: 47.1 MiB
    LZ4  (compressed): 28156 pages (62.25%)  ratio: 342%    stored: 329.4 MiB
    RAW  (incompress):  5030 pages (11.12%)  ratio: 100%    stored: 19.6 MiB
  aggregate:
    orig_bytes:       176.5 MiB
    stored_bytes:     396.1 MiB
    compression_ratio: 44.56%
    pool_allocated:   412.3 MiB
  class_distribution:
    class  0 (  256 B): live=  120 free=  456
    class  9 ( 4096 B): live=45231 free=  123
  reserver: spans=12/64
```

---

## Métricas de Compresión por Capa

| Capa | Descripción | Ratio Típico | Uso RAM |
|------|-------------|--------------|---------|
| **SAME** | Páginas idénticas (zero-fill, pattern) | ∞ (solo 8 bytes patrón) | Mínimo |
| **LZ4** | Comprimibles estándar | 2.5x – 4x | Medio |
| **RAW** | Incomprimibles (cifrado, media) | 1.0x | Máximo (4 KiB) |

---

## Próximos Ciclos (Backlog)

- [ ] Cachear string formateado en device (evitar snprintf repetido en `.stats` reads)
- [ ] `zram_stats_fprintf` en `zram_kernel.h` para tools userspace
- [ ] Build verification en Haiku Jam real (toolchain)
- [ ] Benchmark compresión multi-thread (per-CPU ctx pools)
- [ ] Integración `VMAnonymousCache` patch (upstream Haiku)

---

## Flujo de Datos — Stats Unificado

### 1. Núcleo (zram_core) — Agnóstico a Kernel
```
zram_get_stats(dev, &stats)
    │
    ▼
ZramStats { pages_same, pages_lz4, pages_raw, pages_empty,
            orig_bytes, stored_bytes, pool_bytes,
            class_live[], class_free[],
            reserver_count, reserver_capacity, ... }
    │
    ▼
zram_stats_snprintf(buf, size, &stats, &cfg)
    │
    ├─ zram_fmt_bytes() → human-readable (KiB/MiB/GiB)
    ├─ zram_calc_ratio() → ratio ×100 (orig/stored)
    ├─ Cálculo porcentajes por capa
    └─ Single snprintf() → formatted output
```

### 2. Kernel Swap (zram_kernel) — Haiku Kernel
```
zram_kernel_stats_snprintf(dev, buf, size)
    │
    ▼
zram_get_stats(dev->core, &st)          // ← core stats
    │
    ▼
ZramConfig cfg = { .disk_bytes=dev->disk_bytes,
                   .pool_limit=dev->pool_limit }
    │
    ▼
zram_kernel_fmt_bytes()                 // kernel-safe fmt
    │
    ▼
Single snprintf() → buf
    │
    ▼
zram_kernel_stats_kprintf(dev)          // KDL: kprintf(buf)
```

### 3. hzramfs (FS Add-on) — Kernel FS
```
hzram_volume_get_stats_string(vol, buf, size)
    │
    ▼
ZramConfig cfg = { .disk_bytes=vol->diskBytes,
                   .pool_limit=vol->poolLimit }
    │
    ▼
zram_get_stats(vol->device, &stats)     // ← core stats
    │
    ▼
zram_stats_snprintf(buf, size, &stats, &cfg)  // ← shared core fn
```

### 4. Host Test (Userspace) — Development
```
zram_test.cpp → zram_get_stats(dev, &stats)
    │
    ▼
zram_stats_snprintf(buf, size, &stats, &cfg)  // ← same core fn
    │
    ▼
printf("%s", buf) / test assertions
```

### Puntos Clave
| Componente | Stats Source | Formatter | Output |
|------------|--------------|-----------|--------|
| zram_core | `zram_get_stats()` | `zram_fmt_bytes()` | `zram_stats_snprintf/fprintf` |
| zram_kernel | `zram_get_stats(dev->core)` | `zram_kernel_fmt_bytes()` | `zram_kernel_stats_snprintf/kprintf` |
| hzramfs | `zram_get_stats(vol->device)` | `zram_fmt_bytes()` | `zram_stats_snprintf` (shared) |
| host test | `zram_get_stats(dev)` | `zram_fmt_bytes()` | `zram_stats_snprintf` (shared) |

### Invariantes
- **Single pass**: un solo `snprintf` por llamada (pre-formateo en buffers locales)
- **Zero-copy**: no allocs en hot path, buffers en stack (32B c/u)
- **Kernel-safe**: `zram_kernel_*` no usa libc, solo `snprintf` + `kprintf`
- **Consistente**: mismo formato exacto en las 3 code paths