# h\_zram: gobernador adaptativo de compresión

Complemento de `h_zram_plan_implementacion.md`. Define el componente que decide **si, cómo y adónde** se comprime cada página según la presión de memoria. Mismas convenciones que el plan: `[HOST]` se prueba sin Haiku, `[HAIKU]` requiere el kernel; **(VERIFICADO)** se leyó en el código fuente de Haiku, **(SUPUESTO)** se confirma en la tarea G0.

---

## 1. Qué puede decidir el gobernador y qué no

**No decide qué páginas se expulsan.** Eso lo hace el `page_daemon`/`page_writer` de Haiku con su propia política. El gobernador solo actúa desde que el kernel dice "escribe esta página al swap".

| Decisión | ¿La toma el gobernador? | Cómo |
| --- | --- | --- |
| ¿Esta página va a zram o a disco? | Sí | Enrutado al asignar el slot (§6) |
| ¿Cuánto esfuerzo de compresión? | Sí | Parámetro `accel` de LZ4 |
| ¿Qué ahorro mínimo justifica guardarla comprimida? | Sí | `min_saving` |
| ¿Cuánta RAM puede tomar el pool ahora? | Sí | Límite blando dinámico |
| ¿Guardar páginas incompresibles tal cual (RAW)? | Sí | Solo si no hay swap en disco |
| ¿Qué página sacar de RAM? | No | `page_daemon` |
| ¿Mover páginas frías del pool a disco? | No en v1 | Idea de v2 (§12) |

### Por qué la presión de RAM es la señal principal, pero no la única

Haiku usa mucha RAM como caché de archivos, y el pool de zram es memoria cableada que **no se puede reclamar como caché**. Cada byte que toma el pool se lo quita al caché. Por eso:

- **Con RAM holgada**, escribir una página a zram suele ser un desperdicio: el `page_writer` también limpia páginas por adelantado, y el original sigue en RAM mientras la copia comprimida ocupa pool. Solo vale la pena guardar lo que comprime muy bien.
- **Con RAM muy presionada**, cualquier ahorro cuenta y descomprimir es órdenes de magnitud más rápido que ir a disco, así que conviene aceptar más y comprimir más rápido.
- **Con datos incompresibles** (vídeo, JPEG, ya comprimidos), zram no ayuda en ningún nivel de presión: hay que mandarlos a disco sin gastar CPU.

---

## 2. Señales de entrada

| Señal | Fuente | Estado |
| --- | --- | --- |
| Nivel de recurso bajo de páginas (ninguno, nota, advertencia, crítico) | `low_resource_state(uint32 resources)` | **VERIFICADO** que existe con esa firma. Nombres exactos de las constantes: **SUPUESTO** |
| Evento inmediato de escalada | `register_low_resource_handler(low_resource_func, void* data, uint32 resources, int32 priority)` | **VERIFICADO** firma. Firma de `low_resource_func`: **SUPUESTO** |
| Tick periódico | `register_resource_resizer(daemon_hook, void* arg, int frequency)`, con frecuencia en décimas de segundo | **VERIFICADO** (el swap la usa así) |
| Páginas libres y totales | `vm_page_num_pages()` existe (**VERIFICADO**); accesor de páginas libres: **SUPUESTO** | G0 |
| Páginas de swap libres en disco | Hay que sumar `bmp->free_slots` de los `swap_file` sin zram; `swap_available_pages()` existe pero cuenta también zram (**VERIFICADO**) | Añadir `swap_disk_free_pages()` |
| Tasa de escrituras y lecturas de zram | Contadores propios | Núcleo |
| Ahorro medio por intento de compresión | Estimador propio | Núcleo |
| Coste de compresión en µs | Medición muestreada con reloj de la plataforma | Núcleo |
| Idle de CPU | Opcional; si no se conoce se toma 100 % | v2 |

---

## 3. Arquitectura

```
 SENSORES (pegamento kernel)        NÚCLEO PURO (host-testable)                ACTUADORES
┌───────────────────────────┐   ┌──────────────────────────────────┐   ┌────────────────────────┐
│ low_resource_state        │   │ Estimadores (EWMA enteros)       │   │ ZramPolicy en una     │
│ handler de baja memoria   │──▶│  - tasa swap-out / swap-in       │   │ palabra atómica de    │
│ páginas libres / totales  │   │  - ahorro por intento            │──▶│ 64 bits, leída por la │
│ swap en disco libre       │   │  - µs por compresión             │   │ ruta de E/S           │
│ contadores de zram        │   │ Controlador                      │   │                        │
└───────────────────────────┘   │  - puntuación de presión P       │   │ Tabla de predicción   │
                                │  - máquina de estados + histér.  │   │ por fuente (cache)    │
                                │  - ajuste de accel, min_saving   │   │ consultada al asignar │
                                └──────────────────────────────────┘   │ el slot               │
                                                                        └────────────────────────┘
```

Principio: el controlador es una **función pura** `step(entradas, estado) → (estado', política)`. Eso permite probarla con trazas sintéticas en el host y ajustar parámetros sin arrancar Haiku.

---

## 4. Estructuras exactas

```
// zram_gov.h  (sin includes del kernel)
enum ZramMode : uint8_t {
    ZMODE_BYPASS = 0,   // zram no compensa: enrutar casi todo a disco
    ZMODE_LAZY,         // RAM holgada: solo lo que comprime muy bien
    ZMODE_NORMAL,
    ZMODE_ACTIVE,
    ZMODE_PANIC         // presión crítica: aceptar cualquier ahorro útil
};

struct ZramPolicy {              // se empaqueta en 64 bits (ver abajo)
    uint8_t  mode;
    uint8_t  accel;              // 1, 2, 4 u 8: aceleración de LZ4
    uint16_t min_saving;         // bytes: ahorro mínimo para guardar comprimida
    uint8_t  allow_raw;          // 1 si se permite guardar página sin comprimir
    uint8_t  soft_pool_pct;      // 25, 50, 75 o 100: % del límite duro permitido ahora
    uint16_t generation;         // se incrementa en cada cambio, para depuración
};

struct GovInputs {
    uint64_t now_us;
    uint8_t  lowres;             // 0 ninguno, 1 nota, 2 advertencia, 3 crítico (el pegamento traduce)
    uint32_t free_pages;
    uint32_t total_pages;
    uint32_t disk_swap_free_pages;   // 0 si no hay swap en disco
    bool     disk_swap_present;
    uint8_t  cpu_idle_pct;           // 100 si se desconoce
    ZramStats stats;                 // contadores acumulados del núcleo
};

struct GovConfig {               // todos ajustables, ver §10
    uint32_t tick_ms;            // 250
    uint32_t dwell_ms;           // 2000
    uint32_t dwell_panic_ms;     // 5000
    uint8_t  enter_pct[3];       // {30, 55, 80}  P que activa NORMAL, ACTIVE, PANIC
    uint8_t  exit_margin;        // 10
    uint16_t min_saving[5];      // por modo: {BYPASS 4096, LAZY 2048, NORMAL 1024, ACTIVE 512, PANIC 256}
    uint8_t  soft_pct[5];        // {25, 25, 50, 75, 100}
    uint8_t  cpu_budget_pct;     // 15 (30 en PANIC)
    uint16_t bypass_enter_saving;// 512  bytes de ahorro medio por intento
    uint16_t bypass_exit_saving; // 1024
    uint32_t bypass_min_attempts;// 256
    uint32_t pageout_hot_rate;   // 2000 páginas/s: por encima suma a P
};

struct GovState {
    uint8_t  mode;
    uint64_t mode_since_us;
    uint64_t last_us;
    ZramStats last_stats;
    int32_t  rate_out_ewma;      // páginas/s escritas
    int32_t  rate_in_ewma;       // páginas/s leídas
    int32_t  saving_ewma;        // bytes de ahorro medio por intento (0 si no cabe)
    int32_t  compress_us_ewma;   // µs por página comprimida
    uint8_t  accel;
    uint16_t generation;
};

// Función pura. Devuelve true si la política cambió.
bool zram_gov_step(const GovConfig*, GovState*, const GovInputs*, ZramPolicy* out);
uint64_t zram_policy_pack(const ZramPolicy&);
ZramPolicy zram_policy_unpack(uint64_t);
```

**Empaquetado atómico:** la política se guarda en un `uint64_t` (mode 8 bits, accel 8, min\_saving 16, allow\_raw 8, soft\_pool\_pct 8, generation 16). La ruta de E/S hace **una sola lectura atómica**; nunca ve una política a medias ni toma locks.

---

## 5. Algoritmo del controlador

### 5.1 Estimadores (aritmética entera, sin coma flotante)

Todos con EWMA `x += (muestra - x) >> k`:

- `rate_out`, `rate_in`: derivadas de `stats.writes` y `stats.reads` entre ticks, k = 2.
- `saving_ewma`: por cada intento de compresión, muestra = `4096 - tamaño_comprimido`, o 0 si no cupo. El núcleo mantiene el contador `attempts` y `saving_sum` acumulado; el gobernador calcula la media del último tick y la suaviza, k = 2. Se ignora un tick con menos de 16 intentos.
- `compress_us_ewma`: el núcleo mide con el reloj de plataforma **1 de cada 16** escrituras y acumula `compress_us_sum` y `compress_samples`. k = 3.

### 5.2 Puntuación de presión P (0 a 100)

```
lowres_score = { 0, 30, 65, 100 }[inputs.lowres]

free_pct  = 100 * free_pages / total_pages
free_score = clamp( (15 - free_pct) * 100 / 15 , 0, 100 )      // 0 si hay >=15 % libre

hot_bonus = (rate_out > cfg.pageout_hot_rate) ? 15 : 0

thrash_bonus = (rate_in > rate_out && rate_out > 50) ? 10 : 0  // lecturas superan escrituras: se están recuperando páginas enseguida

P = clamp( max(lowres_score, free_score) + hot_bonus + thrash_bonus , 0, 100 )
```

`lowres_score` domina porque el kernel ya calcula sus propios umbrales de baja memoria; `free_score` es un respaldo por si el nivel no se actualiza a tiempo.

### 5.3 Modo por presión con histéresis

Modos de presión ordenados: `LAZY(0) < NORMAL(1) < ACTIVE(2) < PANIC(3)`. Umbrales de entrada `enter_pct = {30, 55, 80}`; umbral de salida de un modo = su entrada menos `exit_margin`.

```
objetivo = el modo más alto k tal que P >= enter_pct[k-1]   (LAZY si P < 30)

si objetivo > modo_actual:
    modo = objetivo                          // ESCALADA INMEDIATA, sin espera
sino si objetivo < modo_actual:
    // bajar solo si P está por debajo del umbral de salida del modo actual
    y ha pasado >= dwell (2 s; 5 s si el modo actual es PANIC) desde mode_since
    entonces modo = modo_actual - 1          // se baja de un escalón por vez
```

Escalar rápido y bajar despacio evita el "aleteo" y protege justo cuando más importa.

### 5.4 Modo BYPASS (zram no compensa)

BYPASS no es un nivel de presión: se superpone a `LAZY`, `NORMAL` y `ACTIVE`, nunca a `PANIC`.

```
entrar en BYPASS si:  modo en {LAZY, NORMAL, ACTIVE}
                  y   intentos_acumulados >= bypass_min_attempts
                  y   saving_ewma < bypass_enter_saving        (por defecto 512 B)
                  y   disk_swap_present

salir de BYPASS si:   saving_ewma >= bypass_exit_saving        (1024 B)
                  o   modo == PANIC
                  o   no hay swap en disco
```

En BYPASS el enrutado manda a disco casi todo, pero deja pasar **1 de cada 64** asignaciones a zram como sonda. Así `saving_ewma` sigue actualizándose y se detecta cuándo cambia la carga.

### 5.5 Parámetros de salida

```
min_saving   = cfg.min_saving[modo]            // BYPASS usa 4096: nada se acepta salvo páginas SAME
soft_pool_pct= cfg.soft_pct[modo]
allow_raw    = !inputs.disk_swap_present       // sin disco no hay alternativa a guardar tal cual
```

**Por qué** **`allow_raw`** **solo sin disco:** una página RAW no ahorra nada (se libera la original pero el pool toma 4 KiB). Su única utilidad es evitar que el `page_writer` reintente sin fin cuando no hay a dónde mandar la página.

### 5.6 Elección de `accel` (velocidad frente a ratio)

```
coste_cpu_pct = rate_out * compress_us_ewma / 10000      // % de un núcleo
presupuesto   = cfg.cpu_budget_pct * (modo == PANIC ? 2 : 1)

si coste_cpu_pct > presupuesto          y accel < 8:  accel *= 2     (una vez por tick)
si coste_cpu_pct < presupuesto / 3      y accel > 1:  accel /= 2
```

Con `accel = 1` se obtiene el mejor ratio. Si la compresión empieza a consumir demasiado CPU (por ejemplo durante una ráfaga de escrituras), se acelera a costa de algo de ratio.

---

## 6. Predictor por fuente y enrutado

**Problema:** el slot de swap se asigna **antes** de ver los datos (`swap_slot_alloc` se llama antes de escribir), y el slot ya es de zram o de disco. No se puede comprimir primero para decidir. Hace falta predecir.

**Solución:** una tabla pequeña que recuerda cuánto comprimen las páginas de cada fuente (cada `VMAnonymousCache`, es decir, cada área de memoria de un proceso).

```
struct ZramPredEntry {            // se guarda en un std::atomic<uint64_t>, direct-mapped
    uint32_t key;                 // hash de 32 bits del puntero de la caché
    uint16_t avg_size;            // tamaño comprimido medio (EWMA, bytes); 4096 = no comprime
    uint8_t  samples;             // saturado a 255
    uint8_t  probe;               // contador de sondas
};
static const uint32_t ZRAM_PRED_ENTRIES = 256;   // 2 KiB de tabla
```

- **Actualización** (ruta de escritura, tras cada intento): `avg_size += (nuevo - avg_size) >> 2`, con `nuevo = tamaño_comprimido` o 4096 si no cupo. Se escribe con una sola operación atómica de 64 bits (las carreras entre hilos son benignas: lo peor es perder una muestra).
- **Colisiones:** si la clave no coincide, la entrada se reinicia (reemplazo directo). Un puntero reutilizado por otra caché se corrige solo en pocas muestras.
- **Sin historial** (`samples < 4`): se usa el `saving_ewma` global como predicción.

### Función de enrutado

Esta función **sustituye a** **`zram_can_accept`** del plan v2:

```
zram_route(dev, hint) → bool   // true = usar zram para este slot

  p = leer_política_atómica()
  si !disk_swap_present: return true            // no hay alternativa
  si pool_lleno_según(p.soft_pool_pct): return false
  si p.mode == BYPASS: return (contador_sondas++ % 64) == 0

  tam_previsto = predecir(hint)
  ahorro_previsto = 4096 - tam_previsto
  si (contador_sondas++ % 32) == 0 y ahorro_previsto < p.min_saving:
       return true                              // sonda: reevaluar fuentes que antes no comprimían
  return ahorro_previsto >= p.min_saving
```

`pool_lleno_según(pct)`: `pool_bytes >= pool_limit * pct / 100` **y** no hay objetos libres en las clases. Los objetos libres se reutilizan siempre, sin crecer el pool.

### Qué hace la ruta de escritura con la política

```
p = leer_política_atómica()
si es_repetida(page): guardar SAME            // siempre, no cuesta nada
sino:
    tope = min(3072, 4096 - p.min_saving)      // capacidad de destino que se pide a LZ4
    n = LZ4_compress_fast_extState(state, src, dst, 4096, tope, p.accel)
    si n > 0: guardar LZ4
    sino si p.allow_raw: guardar RAW
    sino: actualizar predictor con 4096; return ZRAM_E_REJECT
actualizar predictor y contadores de intentos
```

Al pedirle a LZ4 un destino máximo de `4096 - min_saving`, **LZ4 falla rápido (devuelve 0)** cuando no se alcanza el ahorro exigido. El rechazo no cuesta más que la compresión parcial.

**Tras un rechazo:** el pegamento devuelve `B_NO_MEMORY` (a confirmar en G0 cómo lo trata `page_writer`). El slot se libera por el camino de error ya existente. El predictor ya aprendió que esa fuente no comprime, así que el siguiente slot de esa fuente va a disco y no hay tormenta de reintentos.

---

## 7. Cambios en la ruta caliente (enmiendas al plan v2)

| Elemento del plan v2 | Cambio |
| --- | --- |
| `ZramPlatformOps` | Añadir `uint64_t (*now_us)(void)` |
| `ZramDevice` | Añadir `std::atomic<uint64_t> policy_word`, `ZramPredEntry pred[256]`, `uint64_t attempts, saving_sum, compress_us_sum, compress_samples` |
| `zram_write_page(dev, ctx, slot, page)` | Pasa a `zram_write_page(dev, ctx, slot, page, uint32_t hint)`. Lee la política, aplica §6, actualiza predictor y contadores |
| `zram_can_accept(dev)` | Se reemplaza por `zram_route(dev, hint)`. Se conserva `zram_pool_has_room(dev)` para uso interno |
| Códigos de retorno | Añadir `ZRAM_E_REJECT = -4` |
| `ZramStats` | Añadir `attempts`, `rejects`, `mode`, `accel`, `saving_ewma`, `compress_us_ewma` |
| `swap_slot_alloc(count)` (T7.2) | Pasa a `swap_slot_alloc(count, const void* hint)`. `VMAnonymousCache::Write` y `WriteAsync` pasan `this`. `zram_kernel_route(dev, hint)` decide el primer intento por zram |
| `swap_file_add` / `swap_zram_add` | Sin cambios |
| Nueva función en `VMAnonymousCache.cpp` | `swap_disk_free_pages()` y `swap_has_disk_files()`: recorren `sSwapFileList` ignorando archivos con `zram != NULL` |
| Núcleo | Nuevo archivo `zram_gov.cpp/.h` con `zram_gov_step` y el predictor; sin includes del kernel |
| Pegamento | Hilo gobernador, manejador de baja memoria, comando de depuración ampliado |
| Reglas innegociables | Se mantienen. El gobernador corre en su propio hilo y la ruta de E/S solo lee una palabra atómica y una entrada de la tabla |

---

## 8. Sensores y actuación en el kernel (pegamento)

**Hilo gobernador.** Un solo hilo del kernel (puede ser el mismo que repone spans, con un semáforo con timeout):

```
cada tick_ms (250) o al ser despertado por el manejador de baja memoria:
    in = leer_sensores()
    cambiado = zram_gov_step(&cfg, &state, &in, &pol)
    si cambiado: atomic_store(policy_word, pack(pol))
```

**Manejador de baja memoria.** Registrado con `register_low_resource_handler` sobre el recurso de páginas. No reclama memoria (el pool no se encoge en v1). Solo guarda el nivel recibido en un atómico y despierta al hilo para que **escale sin esperar al siguiente tick**.

**Traducción de niveles.** El pegamento convierte las constantes del kernel a `0..3` de `GovInputs.lowres`. Los nombres exactos se confirman en G0.

**Depuración.** El comando `zram` del depurador del kernel imprime: modo, `accel`, `min_saving`, `P` calculada, `saving_ewma`, `compress_us_ewma`, tasas, tamaño del pool y su porcentaje frente al límite blando, aciertos del predictor y contadores de rechazos.

**Modo fijo para comparar (A/B).** Ajuste `zram_governor` en `virtual_memory`: `auto` (por defecto), `off` (política constante `NORMAL`, `accel=1`) o un nombre de modo fijo. Esencial para medir en G7 que el gobernador realmente mejora algo.

---

## 9. Tareas

Se insertan después de T5 y antes de T6 en el plan v2. G1 a G5 son `[HOST]`.

### G0. Verificaciones previas `[HAIKU-SRC]`

| # | Qué verificar | Dónde buscar |
| --- | --- | --- |
| G0.1 | Nombres exactos de `B_KERNEL_RESOURCE_PAGES` y de los niveles (`B_NO_LOW_RESOURCE`, `B_LOW_RESOURCE_NOTE`, `B_LOW_RESOURCE_WARNING`, `B_LOW_RESOURCE_CRITICAL`) y su orden numérico | `headers/private/kernel/low_resource_manager.h`, `src/system/kernel/low_resource_manager.cpp` |
| G0.2 | Firma de `low_resource_func` y qué se espera que haga el manejador | mismo archivo |
| G0.3 | Umbrales con los que el kernel decide cada nivel de páginas (para calibrar `free_score`) | `low_resource_manager.cpp` |
| G0.4 | Accesor de páginas libres y reclamables | `headers/private/kernel/vm/vm_page.h` |
| G0.5 | Cómo reacciona `page_writer` cuando `WriteAsync` termina con `B_NO_MEMORY`: reintento, penalización, pánico | `src/system/kernel/vm/vm_page.cpp` (`PageWriteTransfer::IOFinished`) |
| G0.6 | `system_time()` disponible y su coste en el kernel | `headers/os/kernel/OS.h`, `src/system/kernel/timer.cpp` |
| G0.7 | Si `WriteAsync` y `Write` pueden pasar `this` a `swap_slot_alloc` sin problemas de locking (`sSwapFileListLock`) | `VMAnonymousCache.cpp` |

**Aceptación:** tabla con ruta, línea y declaración citada. Un resultado de G0.5 distinto de "la página queda sucia y se reintenta más tarde" obliga a revisar §6 antes de seguir.

### G1. Estimadores y empaquetado `[HOST]`

Implementar `zram_policy_pack/unpack`, las EWMA enteras y el cálculo de P (§5.1 y §5.2).

**Pruebas:**

- `pack(unpack(x)) == x` para 100 000 políticas aleatorias.
- P con entradas límite: sin overflow con `total_pages = 0`, con 2^32 páginas, con tasas negativas por desbordamiento del contador (usar diferencias sin signo).
- Monotonía: a igualdad de lo demás, mayor `lowres` nunca baja P.

**Aceptación:** pasa bajo UBSan.

### G2. Controlador y simulador `[HOST]`

Implementar `zram_gov_step` completo (§5.3 a §5.6) y un simulador `sim_gov.cpp` que alimenta trazas de entradas y registra la evolución del modo.

**Escenarios obligatorios (cada uno es un test con aserciones):**

| Escenario | Aserción |
| --- | --- |
| Calma constante 60 s | Modo `LAZY` siempre, sin cambios de política |
| Rampa de presión: nivel 0→1→2→3 en 10 s | Escala en el mismo tick en que cambia el nivel; modo final `PANIC` |
| Pico de 1 s y vuelta a calma | Entra en `PANIC` en el tick del pico; no sale antes de 5 s; baja de un escalón por vez |
| Presión oscilante entre nivel 1 y 2 cada 500 ms durante 60 s | Máximo 6 cambios de modo en el minuto (no hay aleteo) |
| Carga incompresible (`saving` ≈ 0) a presión media | Entra en `BYPASS` tras `bypass_min_attempts` intentos |
| BYPASS y cambio a carga compresible | Sale de `BYPASS` en menos de 3 s mientras las sondas lleguen |
| Presión crítica con carga incompresible | Nunca entra en `BYPASS` (PANIC lo prohíbe) |
| Sin swap en disco | `allow_raw = 1` y nunca `BYPASS` |
| Ráfaga de escrituras con compresión cara | `accel` sube un escalón por tick hasta 8; vuelve a 1 cuando la ráfaga acaba |

**Aceptación:** todos los escenarios pasan y el simulador imprime una tabla de transiciones legible para revisión humana.

### G3. Predictor por fuente y enrutado `[HOST]`

Implementar la tabla de predicción atómica y `zram_route` (§6). Integrar la política en `zram_write_page` (nueva firma con `hint`) conservando todas las pruebas de T3 a T4.

**Pruebas:**

- Dos fuentes sintéticas: una de texto (comprime \~3:1) y una aleatoria. Tras 20 escrituras cada una, `zram_route` devuelve `true` para la primera y `false` para la segunda en modo `NORMAL`.
- La fuente aleatoria recibe una sonda cada \~32 enrutados. Si pasa a comprimir bien, vuelve a `true` en menos de 10 escrituras.
- Colisión de clave: una entrada pisada se reinicia sin que `avg_size` quede inválido.
- TSan con 8 hilos escribiendo con hints mezclados: sin carreras reportadas sobre la tabla (usar atómicos de 64 bits).
- Rechazo (`ZRAM_E_REJECT`): no altera el slot previo ni los contadores de pool.

**Aceptación:** pasa ASan y TSan.

### G4. Integración núcleo-gobernador `[HOST]`

Conectar los contadores del núcleo (`attempts`, `saving_sum`, `compress_us_sum`, `compress_samples`) con `GovInputs.stats`. Añadir un reloj falso en `platform_host.cpp` para controlar `now_us`.

**Prueba de sistema:** un test que escribe 100 000 páginas mientras una "presión simulada" sube y baja, y que comprueba que (a) la política vista por la ruta de E/S siempre es una de las válidas, (b) nunca hay lecturas rotas de `policy_word` (hilo escritor que alterna dos políticas distintas y lectores que verifican coherencia interna: `mode` y `min_saving` siempre emparejados).

**Aceptación:** limpio con `make tsan`.

### G5. Benchmark de políticas `[HOST]`

`bench_policy.cpp`: reproduce tres trazas (calma con ráfagas, presión sostenida, carga incompresible) con tres configuraciones (`off`, `auto`, `LAZY` fijo) y reporta pool usado, ahorro neto en bytes, CPU gastada en compresión y número de páginas rechazadas.

**Aceptación:** en la traza incompresible, `auto` gasta menos CPU que `off`; en presión sostenida, `auto` consigue al menos el 95 % del ahorro neto de `off`.

**Punto de control humano:** revisar tablas del simulador y del benchmark antes de pasar a kernel.

### G6. Pegamento del gobernador `[HAIKU]`

Implementar el hilo gobernador, el manejador de baja memoria, la traducción de niveles, `swap_disk_free_pages`/`swap_has_disk_files`, el paso de `hint` por `swap_slot_alloc` y el ajuste `zram_governor`. Ampliar el comando `zram` del depurador (§8).

**Aceptación:**

- Sin carga: el comando `zram` muestra modo `LAZY` estable.
- Con `zram_stress` del plan v2: el modo sube al aparecer la presión y baja al terminar, sin oscilar.
- Con `zram_governor = off`, el comportamiento coincide con el plan v2.

### G7. Medición en Haiku `[HAIKU]`

Repetir T8 a T10 del plan v2 con `zram_governor = auto` y con `off`, y añadir un caso con memoria incompresible (datos aleatorios).

**Aceptación (objetivos):**

- Mismo número de ciclos correctos que en T8 sin discrepancias.
- Con carga incompresible, `auto` reduce el uso de pool y CPU frente a `off`.
- Con presión alta, `auto` no empeora el tiempo total de `off` en más de un 5 %.

---

## 10. Parámetros iniciales y cómo ajustarlos

Todos los valores de `GovConfig` son punto de partida razonado, no medido. **Se calibran con G5 y G7**, no a ojo.

| Parámetro | Valor | Si el sistema... | Ajuste |
| --- | --- | --- | --- |
| `tick_ms` | 250 | reacciona tarde | bajar a 100 |
| `dwell_ms` | 2000 | oscila entre modos | subir a 4000 |
| `enter_pct` | 30, 55, 80 | usa zram demasiado pronto | subir los dos primeros |
| `min_saving[LAZY]` | 2048 | llena el pool con calma | subir a 2560 |
| `min_saving[PANIC]` | 256 | rechaza demasiado en crisis | no bajar de 256 (una clase) |
| `soft_pct` | 25, 25, 50, 75, 100 | quita demasiada RAM al caché | bajar en LAZY y NORMAL |
| `cpu_budget_pct` | 15 | compresión satura un núcleo | bajar a 10 |
| `bypass_enter_saving` | 512 | entra en BYPASS con datos útiles | bajar a 384 |

---

## 11. Reglas innegociables del gobernador

1. La ruta de E/S nunca espera al gobernador: lee `policy_word` y listo. Si el hilo gobernador se queda parado, la última política sigue vigente.
2. El controlador es una función pura: sin globales, sin tiempo leído internamente (recibe `now_us`), sin asignación de memoria.
3. Escalar es inmediato; bajar tiene espera y va de un escalón.
4. `PANIC` nunca se sustituye por `BYPASS`.
5. Con `zram_governor = off`, la política es constante y no hay hilo gobernador activo.
6. Ninguna decisión del gobernador puede causar pérdida de datos: lo peor es rechazar una escritura (la página queda sucia en RAM) o mandar el slot a disco.
7. No usar coma flotante en el kernel.

---

## 12. Límites conocidos e ideas para v2

- **El gobernador no elige víctimas.** Si el `page_daemon` expulsa páginas calientes, zram solo mitiga el coste del error.
- **El pool no se encoge.** El límite blando impide crecer, pero no devuelve memoria ya tomada. Tras un pico, la RAM del pool queda reservada hasta el reinicio.
- **Predicción por caché, no por página.** Áreas de memoria heterogéneas (por ejemplo, un heap con datos mixtos) se predicen peor.
- **Ideas de v2:**
    - Escritura diferida de objetos fríos del pool a disco cuando la presión baja (estilo zswap), para recuperar RAM.
    - Segundo algoritmo (ZSTD) para páginas muy frías, con LZ4 para las calientes.
    - Idle de CPU real como entrada.
    - Etiquetas por tipo de área (pila, heap, media) en lugar de por puntero.
    - Aprendizaje de los parámetros del §10 a partir de trazas reales.

---

## 13. Resumen de qué automatiza cada nivel de presión

| Modo | Cuándo | Ahorro mínimo | Pool permitido | Comportamiento |
| --- | --- | --- | --- | --- |
| `BYPASS` | Los datos no comprimen | solo páginas repetidas | según modo base | Casi todo a disco, 1 de 64 sondas a zram |
| `LAZY` | RAM holgada | 2048 B (50 %) | 25 % | Solo lo muy compresible, máximo ratio (`accel` 1) |
| `NORMAL` | Presión ligera | 1024 B (25 %) | 50 % | Equilibrio |
| `ACTIVE` | Advertencia | 512 B (12,5 %) | 75 % | Acepta más, `accel` sube si falta CPU |
| `PANIC` | Crítico | 256 B | 100 % | Acepta cualquier ahorro útil, presupuesto de CPU doble, sin BYPASS |
