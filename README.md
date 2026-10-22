# hzram - ZRAM Module for Haiku OS

Implementación de swap comprimido en RAM (`zram`) para Haiku OS integrado directamente en `VMAnonymousCache`.

Comprime páginas anónimas expulsadas por el `page_writer` en memoria RAM utilizando LZ4 antes de recurrir a la paginación en disco (`/var/swap`), reduciendo sustancialmente la latencia de E/S y el desgaste de almacenamiento físico.

---

## Arquitectura en 3 Capas

```
┌──────────────────────────────────────────────────────────┐
│ Capa 3: Parche VMAnonymousCache.cpp                     │
│  swap_file.zram, rama en Read/Write/WriteAsync,          │
│  hook en swap_slot_dealloc, prioridad en swap_slot_alloc │
├──────────────────────────────────────────────────────────┤
│ Capa 2: Pegamento del Kernel (zram_kernel.cpp/.h)        │
│  ZramPlatformOps, hilo de reposición, copia física/virt, │
│  comando de depuración del kernel `zram`                 │
├──────────────────────────────────────────────────────────┤
│ Capa 1: Núcleo Autónomo (zram_core.cpp/.h + lz4.c)       │
│  Asignador por clases (spans 64K, chunks 1M), 64 stripes, │
│  SAME page detection, LZ4 block codec, cero deps kernel   │
└──────────────────────────────────────────────────────────┘
```

### Capa 3: Integración VMAnonymousCache
- `src/system/kernel/vm/vm_anonymous_cache_patch.cpp`: Especificación del parche para `src/system/kernel/vm/VMAnonymousCache.cpp` en el árbol de Haiku.

### Capa 2: Interfaz del Kernel de Haiku
- `src/system/kernel/vm/zram/zram_kernel.h`: Declaración de la API de control del kernel.
- `src/system/kernel/vm/zram/zram_kernel.cpp`: Implementación de `ZramPlatformOps` con primitivas nativas de Haiku (`mutex`, `create_sem`, `spawn_thread`, `create_area`, `vm_memcpy_*_physical`) y debugger command `zram`.

### Capa 1: Núcleo Autónomo
- `src/system/kernel/vm/zram/zram_core.h`: API pública del núcleo y estructuras `ZramSlot` (16 bytes), `ZramConfig`, `ZramStats`.
- `src/system/kernel/vm/zram/zram_core.cpp`: Asignador por clases de tamaño, reserva de spans, detección de páginas idénticas/ceros (`SAME`), compresión LZ4 y tablas de slots.
- `src/system/kernel/vm/zram/lz4/`: Código fuente C de LZ4 (v1.10.x, kernel-safe).

---

## Pruebas en el Host (Linux / macOS)

El núcleo (Capa 1) está completamente aislado del kernel y cuenta con un banco de pruebas unitarias y de estrés con AddressSanitizer y ThreadSanitizer.

```bash
# Compilar y ejecutar pruebas unitarias
make -C tests/zram_host test

# Ejecutar con AddressSanitizer y UndefinedBehaviorSanitizer
make -C tests/zram_host asan
```

Suite de pruebas cubierta:
1. `test_create_destroy`: Inicialización y destrucción de pools y recursos.
2. `test_zero_page`: Detección instantánea de páginas en cero (estado `SAME` con coste 0 bytes).
3. `test_same_page`: Detección de patrones de 64 bits repetidos.
4. `test_lz4_page`: Compresión y descompresión fidedigna de datos típicos.
5. `test_raw_page`: Páginas incompresibles almacenadas en clase RAW (4096 bytes).
6. `test_overwrite`: Transiciones entre estados `SAME` <-> `LZ4` <-> `RAW` verificando liberación de memoria.
7. `test_free_slots`: Liberación idempotente de rangos de slots.
8. `test_oom`: Comportamiento robusto ante agotamiento del pool (falla limpia sin corromper slots).
9. `test_bulk_roundtrip`: Integridad bidireccional sobre 500 páginas mixtas.
10. `test_validation`: Validación estricta de límites y argumentos nulos.

---

## Compilación en Haiku

Para compilar dentro del árbol oficial de Haiku:

1. Copiar `src/system/kernel/vm/zram` a `build/haiku/src/system/kernel/vm/zram`.
2. Aplicar el parche documentado en `src/system/kernel/vm/vm_anonymous_cache_patch.cpp` a `src/system/kernel/vm/VMAnonymousCache.cpp`.
3. Añadir `zram.o` a `KernelMergeObject kernel_vm.o` en `src/system/kernel/vm/Jamfile`.
4. Compilar con Jam:
```bash
jam -q @nightly-anyboot
```

### Configuración en `virtual_memory`
En el archivo de ajustes `/boot/system/settings/kernel/drivers/virtual_memory`:
```
zram_size        536870912    # Tamaño nominal en bytes (p. ej. 512 MB)
zram_pool_limit  268435456    # Límite máximo de RAM física (p. ej. 256 MB)
```

### Depuración en tiempo de ejecución
En KDL (Kernel Debugging Land):
```
kdebug> zram
```

---

## Documentación de Diseño

- `h_zram_plan_implementacion.md`: Plan maestro de implementación y verificación (T0 a T10).
- `h_zram_gobernador.md`: Gobernador adaptativo de políticas según presión de memoria (`BYPASS`, `LAZY`, `NORMAL`, `ACTIVE`, `PANIC`).
- `h_zram_simd.md`: Catálogo y reglas de seguridad de optimizaciones SIMD en kernel.
- `h_zram_asm.md`: Análisis y diseño de optimizaciones en ensamblador con fallback multiplataforma.
