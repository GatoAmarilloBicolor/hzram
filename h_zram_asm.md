# h\_zram: optimizaciones en ensamblador con respaldo multiplataforma

Complemento de `h_zram_plan_implementacion.md`, `h_zram_gobernador.md` y `h_zram_simd.md`. Mismas convenciones: `[HOST]` se prueba sin Haiku, `[HAIKU]` requiere el kernel. **(SUPUESTO)** significa que no se pudo confirmar en el código de Haiku y se resuelve en la tarea A0. **Ninguna cifra de rendimiento de este documento está medida**; son hipótesis que el banco de pruebas debe confirmar o descartar.

---

## 1. Respuesta corta

**Casi todo lo que parece candidato a ensamblador se puede conseguir con intrínsecos del compilador o con una línea de ensamblador en línea**, sin los riesgos de un archivo `.S` completo. Solo hay **un** candidato donde el ensamblador puro tiene una razón real de ser: el **bucle rápido del decodificador LZ4** (el camino de lectura, que es el que importa para la latencia de un fallo de página).

| Parte | ¿Ensamblador puro (`.S`)? | Mejor opción | Por qué |
| --- | --- | --- | --- |
| Bucle de descompresión LZ4 | **Sí, único caso real** | `.S` por arquitectura, solo registros generales | Control total del bucle; no hay intrínseco que lo exprese; y sin registros vectoriales no necesita guarda de FPU, así que sirve en el contexto del fallo de página |
| Bucle de compresión LZ4 | No | C estándar | Está limitado por la latencia de la tabla hash, no por instrucciones; el ensamblador rara vez mejora aquí |
| CRC32C (verificación opcional de integridad) | No | Intrínsecos (`crc32` en x86\_64, extensión CRC en arm64) con flags por archivo | Generan el mismo código que escribirías a mano y no hay riesgo de ABI |
| Relleno de página (`SAME` en lectura) | No | `rep stosq` en línea, o `memset` del kernel | Una instrucción; probablemente el `memset` del kernel ya lo hace |
| Copia de objetos | No | `memcpy` del kernel (verificar en A0) | Ya optimizado |
| Detección de página repetida | No | Intrínsecos SSE2/NEON (ver `h_zram_simd.md`) | Cubierto |
| Guardado y restauración del estado vectorial | Ensamblador **en línea** | `fxsave`/`fxrstor`, `stp q0..q31` | Es una necesidad, no una optimización |
| Detección de características de CPU | Ensamblador **en línea** | `cpuid`, `xgetbv`; en arm64 lectura de `ID_AA64ISAR0_EL1` | Necesario; muy corto |
| Asignador, tabla de slots, locking, gobernador | No | C | No son cuellos de botella y el ensamblador solo añadiría riesgo |

**Honestidad sobre la ganancia:** un compilador moderno (GCC 11 o posterior con `-O2`/`-O3`) genera un decodificador LZ4 muy bueno. Es posible que un `.S` hecho a mano gane entre nada y un 15 %, y es posible que no gane nada. Por eso hay una **puerta de decisión** (A7): si el ensamblador no supera al mejor C compilado en al menos un 15 % medido, se descarta y se deja el C. El respaldo siempre existe.

---

## 2. Por qué "solo registros generales" lo cambia todo

En `h_zram_simd.md` la lectura (fallo de página) quedó **solo escalar** porque corre en el contexto del proceso de usuario, cuyos registros vectoriales están vivos. Una rutina que use **únicamente registros de propósito general** no toca ese estado, así que **no necesita guarda y es segura en la ruta de lectura**.

Consecuencias:

- El decodificador en ensamblador es válido para la lectura precisamente porque no usa SSE/AVX/NEON.
- En x86\_64 se mueven 8 bytes por instrucción con registros generales.
- En arm64 hay un truco útil: `ldp x4, x5, [x1], #16` / `stp x4, x5, [x0], #16` mueven **16 bytes por instrucción usando registros generales**, sin NEON. En arm64 el decodificador GPR-only no pierde casi nada frente a uno con NEON.

**Enmienda a** **`h_zram_simd.md`:** la regla 2 ("nunca SIMD en lectura") se mantiene, pero el camino de lectura pasa a poder usar **kernels GPR-only** marcados `uses_vector_regs = false`.

---

## 3. Matriz de plataformas y jerarquía de respaldo

Haiku soporta varias arquitecturas, y cada una necesita su propio ensamblador (no hay portabilidad entre x86\_64, x86 de 32 bits y arm64). La solución es una **jerarquía de cuatro niveles por kernel**, y el sistema usa el más alto disponible:

```
Nivel 3: ensamblador (.S) por arquitectura        ← solo donde se justifique con medición
Nivel 2: intrínsecos / ensamblador en línea       ← SSE2, NEON, crc32, rep stosq
Nivel 1: C portable optimizado (SWAR, 64 bits)    ← compila en cualquier plataforma
Nivel 0: C ingenuo de referencia                  ← oráculo de pruebas, nunca se borra
```

| Kernel | x86\_64 | x86 (32 bits) | arm64 | Otras (riscv64, arm32, ppc…) |
| --- | --- | --- | --- | --- |
| Decodificador LZ4 (bucle rápido) | N3: `.S` GPR-only | N1: C | N3: `.S` GPR-only con `ldp`/`stp` | N1: C |
| CRC32C (opcional) | N2: `crc32` (SSE4.2) con detección | N1: tablas *slicing-by-8* | N2: `crc32cx` con detección | N1: *slicing-by-8* |
| Relleno de página | N2: `rep stosq` | N1: C | N2: `stp xzr, xzr` en bucle (o `dc zva` si procede) | N0/N1: `memset` |
| Detección página repetida | N2: SSE2 | N1: SWAR de 32 bits | N2: NEON | N1: SWAR |
| Guarda de estado vectorial | N2: `fxsave` | **Sin SIMD** | N2: `stp q` | **Sin SIMD** |
| Compresión LZ4 | N1: C | N1: C | N1: C | N1: C |

**x86 de 32 bits:** no se escribe ensamblador en v1. Hay solo 8 registros generales, la rentabilidad es baja, la plataforma tiene menos demanda y la cadena de herramientas puede ser más restrictiva (a confirmar en A0). Todo queda en el nivel 1.

**Otras plataformas:** nivel 1 siempre. Nada de este diseño impide añadirlas más adelante.

### Selección

Se hace **una vez** al crear el dispositivo, igual que en el diseño SIMD, y se guarda en una tabla de punteros constante:

```
struct ZramAsmKernels {
    const char* name;                         // "scalar", "x86_64-asm", "arm64-asm"…
    // Devuelve ZRAM_E_* o la longitud descomprimida (debe ser 4096).
    int  (*lz4_decode)(const uint8_t* src, uint32_t csize, uint8_t* dst);
    uint32_t (*crc32c)(const void* page4k);   // NULL si no hay checksum
    void (*fill_page)(void* page4k, uint64_t pattern);
    bool uses_vector_regs;                    // false para todos los de este documento
};
const ZramAsmKernels* zram_asm_select(uint32_t cpu_features, int forced /* auto/off */);
```

Ajuste en `virtual_memory`: `zram_asm = auto | off`. Con `off` se usa siempre el nivel 1.

---

## 4. El caso principal: bucle rápido del decodificador LZ4

### 4.1 Diseño híbrido (el que hace viable el ensamblador)

Escribir un decodificador LZ4 **completo y seguro** en ensamblador sería largo y propenso a errores. En cambio:

```
zram_lz4_decode(src, csize, dst):
    (ip, op) = zram_lz4_fast(src, src+csize-IP_MARGIN, dst, dst+4096-OP_MARGIN)   ← ensamblador
    return  zram_lz4_tail_c(ip, src+csize, op, dst+4096, dst)                      ← C seguro
```

- El **bucle rápido (ensamblador)** procesa la mayor parte de la página bajo márgenes conservadores y **se detiene al inicio de una secuencia** en cuanto cualquier comprobación falla o se acerca a un borde.
- La **cola (C)** termina el trabajo con todas las comprobaciones, y devuelve error si el flujo está corrupto.
- El mismo código C de la cola es el **decodificador de referencia** de las pruebas.

La página de destino mide siempre exactamente 4096 bytes. Con margen de 48 bytes al final, la cola en C procesa solo los últimos 48 bytes o menos: es una parte pequeña del tiempo.

### 4.2 Formato LZ4 (lo mínimo necesario)

Cada secuencia es: `[token][bytes extra de longitud de literales]*[literales][offset (2 bytes little endian)][bytes extra de longitud de coincidencia]*`.

- `token`: nibble alto = longitud de literales; nibble bajo = longitud de coincidencia menos 4.
- Si un nibble vale 15, se suman bytes adicionales hasta encontrar uno distinto de 255.
- La **última secuencia contiene solo literales** (sin offset), y el flujo termina ahí.
- La copia de coincidencia puede **solaparse** (offset menor que la longitud), lo que replica un patrón.

### 4.3 Modelo en C del bucle rápido (la especificación exacta)

El implementador escribe **primero este código en C**, lo prueba, y solo después lo traduce a ensamblador instrucción a instrucción. Cada iteración del ensamblador debe tener el mismo efecto que esta del modelo.

```
#define IP_MARGIN 32
#define OP_MARGIN 48

// Devuelve en *pip y *pop el inicio de la primera secuencia NO procesada.
void lz4_fast_model(const uint8_t** pip, const uint8_t* ip_limit,
                    uint8_t** pop, uint8_t* op_limit, const uint8_t* dst_base)
{
    const uint8_t* ip = *pip;
    uint8_t* op = *pop;

    while (ip < ip_limit && op < op_limit) {
        const uint8_t* seq_ip = ip;           // para retroceder si hay que abandonar
        uint8_t* seq_op = op;

        uint32_t token = *ip++;
        size_t lit = token >> 4;
        if (lit == 15) {
            uint32_t b;
            do {
                if (ip >= ip_limit) goto bail;
                b = *ip++;
                lit += b;
            } while (b == 255);
        }
        if (lit > (size_t)(ip_limit - ip) || lit > (size_t)(op_limit - op)) goto bail;
        wildcopy16(op, ip, lit);               // puede copiar hasta 15 bytes de más
        ip += lit; op += lit;

        if (ip + 2 > ip_limit) goto bail;
        uint32_t offset = ip[0] | (ip[1] << 8);
        ip += 2;
        if (offset == 0 || offset > (size_t)(op - dst_base)) goto bail;

        size_t ml = token & 15;
        if (ml == 15) {
            uint32_t b;
            do {
                if (ip >= ip_limit) goto bail;
                b = *ip++;
                ml += b;
            } while (b == 255);
        }
        ml += 4;
        if (ml > (size_t)(op_limit - op)) goto bail;

        const uint8_t* match = op - offset;
        if (offset >= 16)      wildcopy16(op, match, ml);
        else if (offset >= 8)  wildcopy8(op, match, ml);
        else                   bytecopy(op, match, ml);          // solape corto: byte a byte
        op += ml;
        continue;
    bail:
        ip = seq_ip; op = seq_op;
        break;
    }
    *pip = ip; *pop = op;
}
```

**Propiedades que el ensamblador debe preservar (invariantes):**

1. **Nunca escribe fuera de** **`[dst_base, dst_base+4096)`.** Con `op_limit = dst_base + 4096 - 48`, ningún exceso de `wildcopy16` (15 bytes como máximo) sale de la página, aunque el flujo de entrada sea basura.
2. **Nunca lee fuera de** **`[src, src+csize)`.** Los bytes de la entrada se comprueban contra `ip_limit = iend - 32` antes de usarse, y el exceso de lectura de `wildcopy16` es de 15 bytes como máximo.
3. **Offset válido o abandono.** `offset == 0` o `offset > op - dst_base` provoca `bail`; el ensamblador no lo trata como error, lo delega a la cola en C, que decide si es corrupción.
4. **Retroceso exacto.** En `bail` se restauran `ip` y `op` al **inicio de la secuencia**, de forma que la cola en C la procese entera.
5. **Sin pila.** El ensamblador no necesita memoria en pila más allá de guardar registros de la ABI.

### 4.4 Cola segura en C

Implementación completa del decodificador con todas las comprobaciones (longitudes, offsets, bordes, tamaño final exacto de 4096). Devuelve `ZRAM_E_CORRUPT` si algo no cuadra. Se escribe antes que el ensamblador y se valida contra `LZ4_decompress_safe` del LZ4 oficial.

### 4.5 Traducción a x86\_64 (convención System V)

| Elemento | Decisión |
| --- | --- |
| Registros de argumentos | `rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9` según la ABI System V |
| Registros que hay que preservar | `rbx`, `rbp`, `r12` a `r15`. Preferir usar solo los de uso libre (`rax`, `rcx`, `rdx`, `rsi`, `rdi`, `r8` a `r11`) para no guardar nada |
| Zona roja | **No usar** (el kernel se compila sin ella; confirmar en A0) |
| Copias | `mov (%rsi), %rax` / `mov %rax, (%rdi)` de 8 bytes, desenrolladas 2 veces para 16 bytes |
| Longitudes grandes | Para copias de más de 128 bytes, valorar `rep movsb` (rápido en CPUs con ERMS/FSRM); medir antes de decidir |
| Cuenta de ceros | `tzcnt` se codifica como `rep bsf` y en CPUs antiguas se comporta como `bsf`: mismo resultado con entrada distinta de cero |
| Alineación | `.p2align 4` antes de cada etiqueta de bucle |
| Estado vectorial | **No tocar ningún registro** **`xmm`,** **`ymm`** **ni** **`st`** |

### 4.6 Traducción a arm64 (convención AAPCS64)

| Elemento | Decisión |
| --- | --- |
| Registros de argumentos | `x0` a `x7`. Valor de retorno en `x0` |
| Registros libres | `x0` a `x15`; `x16` y `x17` como temporales. **Evitar** **`x18`** (registro de plataforma: confirmar en A0 si Haiku lo reserva) |
| Registros que hay que preservar | `x19` a `x28`, `x29`, `x30`. Evitar tocarlos |
| Copias | `ldp x4, x5, [x1], #16` y `stp x4, x5, [x0], #16`: 16 bytes por instrucción con registros generales |
| Cargas y saltos | Acceso a datos con `adrp` más `add`; no usar direcciones absolutas |
| Condicionales | Preferir `csel`, `ccmp` y `cbz`/`cbnz` cuando eviten saltos |
| Estado vectorial | **No tocar ningún registro** **`v0`** **a** **`v31`** |

### 4.7 Archivos

```
src/system/kernel/vm/zram/asm/
    zram_asm.h                    # prototipos y macros portables
    zram_lz4_fast_x86_64.S
    zram_lz4_fast_arm64.S
    zram_lz4_fast_model.c         # el modelo en C (nivel 1 y oráculo)
    zram_lz4_tail.c               # cola segura (todas las plataformas)
    zram_crc32c_x86_64.cpp        # intrínsecos, flags por archivo (-msse4.2)
    zram_crc32c_arm64.cpp         # intrínsecos (-march=armv8-a+crc)
    zram_crc32c_generic.cpp       # slicing-by-8
tests/zram_host/asm/
    abi_check.S                   # banco de pruebas de la ABI
    guard_buffers.cpp
    test_lz4_asm.cpp, test_crc.cpp, bench_asm.cpp
```

---

## 5. Los demás candidatos

### 5.1 CRC32C (verificación opcional de integridad)

**Motivo:** `LZ4_decompress_safe` detecta flujos mal formados, pero un bit alterado dentro de los bytes literales produce **datos erróneos silenciosos** que acaban en la memoria de un proceso. Una suma de comprobación del objeto comprimido lo detecta.

- Ajuste `zram_checksum = off | on` (por defecto `off`; `on` en compilaciones de depuración).
- Se guardan los 16 bits bajos del CRC32C en `ZramSlot::reserved`. La probabilidad de no detectar un error es de 1 entre 65 536.
- **Implementación por intrínsecos**, no `.S`: `_mm_crc32_u64` (x86\_64, SSE4.2) y `__crc32cd` (arm64, cabecera `arm_acle.h`). Generan el mismo código y no tienen riesgo de ABI.
- Las instrucciones `crc32` y `crc32cx` operan sobre registros generales, así que **no requieren guarda**.
- **Rendimiento:** una sola cadena de `crc32` tiene latencia de unos 3 ciclos por instrucción, así que 512 palabras cuestan \~1500 ciclos (hipótesis). Con **3 corrientes intercaladas** de 1360 bytes más 16 de cola se acerca a 1 palabra por ciclo; las tres sumas se combinan con tablas de desplazamiento precalculadas. Empezar con la versión simple y medir antes de complicarlo.
- Si en la CPU no existe la instrucción (detección en tiempo de ejecución), se usa *slicing-by-8* en C.

### 5.2 Relleno de página

`fill_page(page, pattern)` se usa al leer slots `SAME`. En x86\_64 basta una instrucción en línea:

```
asm volatile("rep stosq" : "+D"(p), "+c"(n) : "a"(pattern) : "memory");   // n = 512
```

Antes de nada, **comprobar si el** **`memset`** **del kernel ya lo hace** (A0). En arm64, `dc zva` rellena a cero un bloque entero de caché y es muy rápido para páginas de ceros, pero exige conocer el tamaño de bloque (`DCZID_EL0`) y solo sirve con memoria normal cacheable; se valora tras medir.

### 5.3 Primitivas de la guarda de SIMD (`h_zram_simd.md`, sección 5)

Ensamblador en línea, necesario aunque no sea una optimización:

```
// x86_64: área de 512 bytes alineada a 16
asm volatile("fxsave (%0)"  :: "r"(area) : "memory");
asm volatile("fxrstor (%0)" :: "r"(area) : "memory");
// arm64: guarda v0..v31 en 512 bytes
asm volatile(
    "stp q0,  q1,  [%0, #0]\n  stp q2,  q3,  [%0, #32]\n"
    /* … hasta q30, q31 … */ ::"r"(area) : "memory");
```

### 5.4 Lo que se descarta explícitamente

- **Compresión LZ4 en ensamblador:** el cuello es la latencia de la tabla hash y la imprevisibilidad de las coincidencias; un `.S` rara vez gana al C y es mucho más difícil de validar. Fuera de v1.
- **Compresores alternativos con ensamblador (ZSTD, Huffman):** fuera de alcance.
- **Ensamblador de 32 bits x86:** fuera de v1.

---

## 6. Cómo se integran los archivos `.S` en Haiku (a verificar en A0)

| Tema | Estado |
| --- | --- |
| Macros de funciones (`FUNCTION`, `FUNCTION_END`) en un encabezado privado del kernel | **SUPUESTO** que existe algo así; confirmar su nombre y uso en los `.S` existentes de `src/system/kernel/arch/x86/64/` y `arm64/` |
| Cómo se añaden fuentes `.S` al kernel en Jam | **SUPUESTO**; copiar el patrón de un `.S` existente |
| Flags por archivo (`-msse4.2`, `-march=armv8-a+crc`) | **SUPUESTO**; ver patrón de `ObjectCcFlags` u equivalente |
| Zona roja y registro `x18` | **SUPUESTO** (kernel x86\_64 sin zona roja; `x18` posiblemente reservado); confirmar |
| Directivas CFI y `.type`/`.size` para trazas de pila | Confirmar qué espera el depurador del kernel |

### Portabilidad del ensamblador entre el host y Haiku

Haiku usa formato **ELF**. En macOS el ensamblador usa **Mach-O**, con diferencias (prefijo `_` en los símbolos, sin `.type`, reubicaciones distintas en arm64). Para que los mismos `.S` funcionen en ambos:

```
// zram_asm.h (se incluye desde los .S)
#if defined(__APPLE__)
#  define ZASM_SYM(n)   _##n
#  define ZASM_BEGIN(n) .globl _##n ; .p2align 4 ; _##n:
#  define ZASM_END(n)
#else
#  define ZASM_SYM(n)   n
#  define ZASM_BEGIN(n) .globl n ; .type n, @function ; .p2align 4 ; n:
#  define ZASM_END(n)   .size n, .-n
#endif
```

**Recomendación práctica:** ejecutar las pruebas del ensamblador en **Linux** (x86\_64 y aarch64), que usa ELF igual que Haiku. En un Mac con Apple Silicon, un contenedor Linux aarch64 se ejecuta de forma nativa; para x86\_64 se puede usar emulación (más lenta). Los `.S` solo se prueban de verdad en el formato en que se van a usar.

---

## 7. Verificación del ensamblador

El ensamblador no se puede revisar con ASan ni con UBSan, así que hacen falta mecanismos específicos.

| Mecanismo | Qué detecta |
| --- | --- |
| **Búferes con páginas de guarda** | La entrada se coloca justo antes de una página `PROT_NONE`, y el destino justo antes y justo después de páginas `PROT_NONE`. Cualquier lectura o escritura fuera de límites provoca un fallo inmediato |
| **Centinelas** | Zona de 4 KiB con patrón conocido antes y después del destino; se comprueba intacta tras cada ejecución |
| **Comprobador de ABI** (`abi_check.S`) | Antes de llamar, carga valores centinela en todos los registros que la ABI exige preservar; tras volver, verifica que no cambiaron. Además comprueba la alineación de la pila |
| **Prueba diferencial** | Todo flujo se decodifica con `LZ4_decompress_safe` oficial, con la cola en C y con el ensamblador; los tres deben coincidir byte a byte |
| **Fuzzing por mutación** | Flujos válidos con 1 a 8 bits alterados al azar: el ensamblador más la cola deben dar **el mismo resultado que el decodificador C seguro** (error o datos) y no escribir ni leer fuera de rango |
| **Casos extremos** | Offset 1, 2, 7, 8, 15, 16 y 17; longitudes 14, 15, 16, 269 y 270 (límites de los bytes extra); coincidencias que terminan exactamente en `op_limit`; entrada que termina exactamente en `ip_limit` |
| **Pruebas de la cola** | Páginas cuya última secuencia queda dentro del margen y páginas completamente dentro de él |

---

## 8. Tareas

Se insertan después de S8 del diseño SIMD.

### A0. Verificaciones previas `[HAIKU-SRC]`

| # | Qué verificar |
| --- | --- |
| A0.1 | Macros de ensamblador y estilo de los `.S` existentes del kernel para x86\_64 y arm64 |
| A0.2 | Regla de Jam para compilar `.S` y para flags por archivo |
| A0.3 | Zona roja desactivada en el kernel x86\_64; uso de `x18` en arm64 |
| A0.4 | Implementación de `memcpy` y `memset` del kernel (¿usan `rep movs`/`rep stos`, `ldp`/`stp`, `dc zva`?) |
| A0.5 | API de detección de características (SSE4.2, BMI, extensión CRC, FSRM) |
| A0.6 | Si el compilador del kernel de x86 de 32 bits admite el C que usa el núcleo (para confirmar el nivel 1 allí) |
| A0.7 | Expectativas del depurador del kernel sobre marcos de pila en funciones hoja |

**Aceptación:** tabla con ruta, línea y cita. Un resultado negativo en A0.1 o A0.2 baja el ensamblador a "no se hace en v1" y todo queda en intrínsecos y C.

### A1. Base de pruebas del ensamblador `[HOST]`

Macros `ZASM_*` de §6, `abi_check.S`, `guard_buffers.cpp` (reserva con `mmap` y `mprotect`) y un `Makefile` con objetivos para Linux x86\_64 y aarch64.

**Aceptación:** una función de ensamblador de juguete (suma de dos enteros, y otra que escribe en un búfer) pasa el comprobador de ABI y detecta intencionadamente un acceso fuera de límites.

### A2. Referencias en C `[HOST]`

`zram_lz4_fast_model.c`, `zram_lz4_tail.c`, `zram_crc32c_generic.cpp` y la versión naíf de `fill_page`.

**Pruebas:** modelo más cola contra `LZ4_decompress_safe` sobre todo el corpus de T5 y sobre 10⁶ páginas generadas; CRC32C contra vectores de prueba conocidos del estándar; fuzzing por mutación (§7).

**Aceptación:** coinciden en todos los casos, limpio con ASan y UBSan.

### A3. CRC32C por intrínsecos `[HOST]`

Implementar las versiones de x86\_64 y arm64 con detección en tiempo de ejecución y respaldo a *slicing-by-8*. Primero la versión de una cadena; la de tres corrientes solo si el rendimiento lo exige.

**Aceptación:** igualdad bit a bit con la referencia en 10⁶ páginas; el banco de pruebas informa el factor de mejora frente a *slicing-by-8*.

### A4. Relleno de página `[HOST]`

`fill_page` con `rep stosq` (x86\_64) y bucle `stp` (arm64).

**Puerta:** si no supera al `memset` del kernel (A0.4) en al menos un 15 %, se usa `memset`.

### A5. Decodificador LZ4 en ensamblador x86\_64 `[HOST]`

Traducir el modelo de §4.3 siguiendo §4.5.

**Aceptación:** pasa todas las pruebas de §7 y el comprobador de ABI. Rendimiento medido en `bench_asm` frente a:

- el decodificador C seguro propio,
- `LZ4_decompress_safe` oficial compilado con `-O2` y con `-O3`.

### A6. Decodificador LZ4 en ensamblador arm64 `[HOST arm64 Linux]`

Traducir el modelo siguiendo §4.6. Mismas pruebas y comparaciones que A5.

### A7. Puerta de decisión `[HOST]`

**Regla:** cada kernel en ensamblador solo se activa si cumple las dos condiciones siguientes en el corpus real, medido en al menos dos modelos distintos de CPU de esa arquitectura:

1. Es al menos un **15 % más rápido** que la mejor versión compilada en C o con intrínsecos.
2. Pasa todas las pruebas de §7.

Si no las cumple, se deja el nivel 2 o 1 y se anota en "Hallazgos". El código en ensamblador queda en el repositorio, desactivado, solo si su mantenimiento es trivial; en otro caso se elimina.

### A8. Integración y medición en Haiku `[HAIKU]`

Añadir `ZramAsmKernels` al dispatch, el ajuste `zram_asm`, y las estadísticas `asm_kernel_name`. Repetir las pruebas T8 a T10.

**Aceptación:**

- Con `zram_asm = off` el comportamiento coincide con el plan anterior.
- Con `auto` no hay discrepancias de datos en `zram_stress` ni en la prueba de integridad del estado vectorial de usuario (S7), que debe seguir pasando porque estos kernels no tocan registros vectoriales.
- El tiempo medio de lectura (`zram_read_page`) con `auto` es menor o igual que con `off`.

---

## 9. Reglas innegociables

1. **Todo kernel en ensamblador tiene una versión en C de nivel 0 o 1 que se compila siempre**, y que se puede seleccionar con `zram_asm = off`.
2. **Ningún ensamblador de este documento toca registros vectoriales ni de coma flotante.**
3. **Nunca lee ni escribe fuera de sus búferes**, aunque la entrada sea corrupta. La cola en C decide qué es un error.
4. **Preserva todos los registros que exige la ABI**, y en x86\_64 no usa la zona roja.
5. **No se escribe ensamblador antes de su modelo en C probado.** El ensamblador se traduce del modelo.
6. **No se adopta sin la puerta de decisión (A7).**
7. **Funciones pequeñas:** un archivo `.S` por función, de menos de 200 líneas cada una.
8. **Ninguna instrucción opcional sin detección en tiempo de ejecución** (CRC32, BMI, ERMS/FSRM).
9. **No hay ensamblador para x86 de 32 bits ni para otras plataformas en v1.**

---

## 10. Riesgos

| Riesgo | Probabilidad | Mitigación |
| --- | --- | --- |
| El ensamblador no supera al C compilado | Alta | Puerta A7; el C es el respaldo y ya es bueno |
| Error sutil de ABI o de límites en el `.S` | Media | Comprobador de ABI, páginas de guarda, fuzzing y modelo en C previo |
| El ensamblador generado por un modelo de lenguaje contiene errores | Media | Se traduce de un modelo probado, funciones pequeñas, la prueba diferencial es obligatoria antes de cualquier uso |
| Diferencias entre ELF y Mach-O al probar en macOS | Alta si se prueba allí | Probar en Linux; macros portables de §6 |
| La cadena de herramientas o Jam no soporta `.S` por archivo como se supone | Media | A0 lo decide; alternativa: ensamblador en línea dentro de C |
| Mantenimiento de dos variantes por arquitectura | Media | Un solo modelo de referencia y un solo banco de pruebas |

---

## 11. Orden recomendado

`A0` → `A1` → `A2` → `A3` → `A4` → `A5` → `A6` → `A7` → `A8`

Con las tareas SIMD (`S0` a `S8`), `A3` y `A4` pueden hacerse antes que `S3`, porque no dependen de la guarda de FPU. La única que realmente justifica ensamblador (`A5` y `A6`) va después de tener el modelo en C y el banco de pruebas funcionando.
