/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		hzram developers
 */
#ifndef _ZRAM_KERNEL_H
#define _ZRAM_KERNEL_H

#include <OS.h>
#include <sys/uio.h>

#include "zram_core.h"

/*
 * swap_addr_t se define en VMAnonymousCache.h; lo referenciamos sin
 * duplicar la definición para no romper el tipo real de Haiku.
 */
#include "../VMAnonymousCache.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ZramKernelDevice;

/*
 * Crea un dispositivo zram.
 *
 * diskBytes: tamaño nominal en bytes (múltiplo de B_PAGE_SIZE).
 * poolLimit: tope de memoria cableada (múltiplo de ZRAM_CHUNK_BYTES).
 * _out: recibe el dispositivo creado.
 */
status_t	zram_kernel_create(uint64_t diskBytes, uint64_t poolLimit,
				ZramKernelDevice** _out);

/* Destruye el dispositivo. */
void		zram_kernel_destroy(ZramKernelDevice* dev);

/*
 * Lee páginas del dispositivo.
 * pos: desplazamiento en bytes (slot * B_PAGE_SIZE).
 * flags: puede incluir B_PHYSICAL_IO_REQUEST para vectores físicos.
 */
status_t	zram_swap_read(ZramKernelDevice* dev, off_t pos,
				const generic_io_vec* vecs, size_t count, uint32_t flags,
				generic_size_t* _numBytes);

/* Escribe páginas en el dispositivo. */
status_t	zram_swap_write(ZramKernelDevice* dev, off_t pos,
				const generic_io_vec* vecs, size_t count, uint32_t flags,
				generic_size_t* _numBytes);

/* Libera un rango de slots locales [firstSlot, firstSlot + count). */
void		zram_kernel_free(ZramKernelDevice* dev, swap_addr_t firstSlot,
				uint32_t count);

/* ¿Queda sitio para al menos una página? */
bool		zram_kernel_can_accept(ZramKernelDevice* dev);

void		zram_kernel_get_stats(ZramKernelDevice* dev, ZramStats* out);
bool		zram_kernel_check_invariants(ZramKernelDevice* dev);

/* Registra el comando de depuración "zram" en KDL (se llama desde swap_init). */
void		zram_kernel_debug_init(void);

#ifdef __cplusplus
}
#endif

#endif /* _ZRAM_KERNEL_H */
