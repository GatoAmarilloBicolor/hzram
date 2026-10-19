# h_zram

A zram implementation for Haiku, integrating with VMAnonymousCache.

## Architecture

This project uses a 3-layer architecture:

### Capa 3: VMAnonymousCache Integration
- `vm_anonymous_cache_patch.cpp` - Patches the VMAnonymousCache class to support zram-backed anonymous memory

### Capa 2: Kernel Module
- `zram_kernel.h` - Kernel module API declarations
- `zram_kernel.cpp` - Kernel module implementation (read-only, no code changes to kernel)

### Capa 1: Core + LZ4
- `zram_core.h` - Public API and structures
- `zram_core.cpp` - Full standalone core implementation
- `lz4/` - LZ4 compression library

## Build

```bash
cd src/system/kernel/vm/zram
jamc -o zram.o Jamfile
```

## Test

```bash
g++ -o zram_test tests/zram_host/zram_test.cpp -I. -L. -lzram_core
./zram_test
```

## Files

- `src/system/kernel/vm/zram/Jamfile` - Build rules
- `src/system/kernel/vm/zram/zram_core.h` - Public API
- `src/system/kernel/vm/zram/zram_core.cpp` - Core implementation
- `src/system/kernel/vm/zram/zram_kernel.h` - Kernel module API
- `src/system/kernel/vm/zram/zram_kernel.cpp` - Kernel module implementation
- `src/system/kernel/vm/vm_anonymous_cache_patch.cpp` - VMAnonymousCache integration
- `tests/zram_host/zram_test.cpp` - Test harness
- `lz4/lz4.c` / `lz4/lz4.h` - LZ4 compression library
