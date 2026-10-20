/*
 * vm_anonymous_cache_patch.cpp
 *
 * Parche de integracion de zram en Haiku (T7 de
 * h_zram_plan_implementacion.md). Se aplica SOLO sobre
 * src/system/kernel/vm/VMAnonymousCache.cpp (rama master).
 *
 * Este archivo NO se compila: es la especificacion exacta del parche.
 * Cada apartado indica el punto de insercion y el codigo a anadir.
 * Con zram desactivado por configuracion, el comportamiento debe ser
 * identico al original.
 *
 * Dependencias de la capa 2: zram_kernel_create/destroy,
 * zram_swap_read/write, zram_kernel_free, zram_kernel_can_accept,
 * zram_kernel_debug_init (zram_kernel.h).
 *
 * ---------------------------------------------------------------------------
 * 7.1  Estructura swap_file: anadir el campo zram y ponerlo a NULL.
 * ---------------------------------------------------------------------------
 *
 *   struct swap_file : DoublyLinkedListLinkImpl<swap_file> {
 *       int             fd;
 *       struct vnode*   vnode;
 *       void*           cookie;
 *       swap_addr_t     first_slot;
 *       swap_addr_t     last_slot;
 *       radix_bitmap*   bmp;
 *   +   struct ZramKernelDevice* zram;
 *   };
 *
 *   Y en swap_file_add(), tras crear `swap`:
 *
 *   +   swap->zram = NULL;
 *
 * ---------------------------------------------------------------------------
 * 7.2  swap_slot_alloc(): prioridad para los swap_file con zram.
 * ---------------------------------------------------------------------------
 *
 *   Justo despues de mutex_lock(&sSwapFileListLock); y antes del bucle
 *   round-robin existente, insertar:
 *
 *   +   for (SwapFileList::Iterator it = sSwapFileList.GetIterator();
 *   +           swap_file* f = it.Next();) {
 *   +       if (f->zram == NULL || !zram_kernel_can_accept(f->zram))
 *   +           continue;
 *   +       swap_addr_t a = radix_bitmap_alloc(f->bmp, count);
 *   +       if (a != SWAP_SLOT_NONE) {
 *   +           mutex_unlock(&sSwapFileListLock);
 *   +           return a + f->first_slot;
 *   +       }
 *   +   }
 *
 *   Y en el bucle round-robin, despues de seleccionar sSwapFileAlloc:
 *
 *   +   if (sSwapFileAlloc->zram != NULL
 *   +           && !zram_kernel_can_accept(sSwapFileAlloc->zram)) {
 *   +       sSwapFileAlloc = sSwapFileList.GetNext(sSwapFileAlloc);
 *   +       continue;
 *   +   }
 *
 * ---------------------------------------------------------------------------
 * 7.3  swap_slot_dealloc(): liberar tambien el objeto comprimido.
 * ---------------------------------------------------------------------------
 *
 *   Tras `slotIndex -= swapFile->first_slot;` y antes de
 *   radix_bitmap_dealloc(...):
 *
 *   +   if (swapFile->zram != NULL)
 *   +       zram_kernel_free(swapFile->zram, slotIndex, count);
 *
 * ---------------------------------------------------------------------------
 * 7.4  VMAnonymousCache::Read(): rama zram.
 * ---------------------------------------------------------------------------
 *
 *   Tras `swap_file* swapFile = find_swap_file(startSlotIndex);` y el calculo
 *   de pos, sustituir la llamada a vfs_read_pages por:
 *
 *   +   if (swapFile->zram != NULL) {
 *   +       status_t status = zram_swap_read(swapFile->zram, pos,
 *   +           vecs + i, j - i, flags, _numBytes);
 *   +       if (status != B_OK)
 *   +           return status;
 *   +   } else {
 *   +       status_t status = vfs_read_pages(swapFile->vnode, swapFile->cookie,
 *   +           pos, vecs + i, j - i, flags, _numBytes);
 *   +       if (status != B_OK)
 *   +           return status;
 *   +   }
 *
 * ---------------------------------------------------------------------------
 * 7.5  VMAnonymousCache::Write(): rama zram.
 * ---------------------------------------------------------------------------
 *
 *   Igual que 7.4 pero con zram_swap_write en lugar de vfs_write_pages.
 *
 * ---------------------------------------------------------------------------
 * 7.6  VMAnonymousCache::WriteAsync(): rama zram sincrona.
 * ---------------------------------------------------------------------------
 *
 *   Tras obtener `swapFile` y `pos`, si swapFile->zram != NULL:
 *
 *   +   generic_size_t written = 0;
 *   +   status_t status = zram_swap_write(swapFile->zram, pos, vecs, 1,
 *   +       numBytes, flags, &written);
 *   +   callback->IOFinished(status, false, written);
 *   +   return status;
 *
 *   (WriteCallback::IOFinished ya construye/libera el bloque de swap.)
 *
 * ---------------------------------------------------------------------------
 * 7.7  Nueva funcion swap_zram_add().
 * ---------------------------------------------------------------------------
 *
 *   Se anade junto a swap_file_add():
 *
 *   status_t
 *   swap_zram_add(uint64_t diskBytes, uint64_t poolLimit)
 *   {
 *       ZramKernelDevice* dev = NULL;
 *       status_t status = zram_kernel_create(diskBytes, poolLimit, &dev);
 *       if (status != B_OK)
 *           return status;
 *
 *       uint32_t pageCount = (uint32_t)(diskBytes / B_PAGE_SIZE);
 *
 *       swap_file* swap = new(std::nothrow) swap_file;
 *       if (swap == NULL) {
 *           zram_kernel_destroy(dev);
 *           return B_NO_MEMORY;
 *       }
 *       swap->fd = -1;
 *       swap->vnode = NULL;
 *       swap->cookie = NULL;
 *       swap->zram = dev;
 *       swap->bmp = radix_bitmap_create(pageCount);
 *       if (swap->bmp == NULL) {
 *           delete swap;
 *           zram_kernel_destroy(dev);
 *           return B_NO_MEMORY;
 *       }
 *
 *       mutex_lock(&sSwapFileListLock);
 *       if (sSwapFileList.IsEmpty()) {
 *           swap->first_slot = 0;
 *           swap->last_slot = pageCount;
 *       } else {
 *           // deja un hueco de una pagina entre swap files
 *           swap->first_slot = sSwapFileList.Last()->last_slot + 1;
 *           swap->last_slot = swap->first_slot + pageCount;
 *       }
 *       sSwapFileList.Add(swap);
 *       sSwapFileCount++;
 *       mutex_unlock(&sSwapFileListLock);
 *
 *       mutex_lock(&sAvailableSwapSpaceLock);
 *       sAvailableSwapSpace += (off_t)pageCount * B_PAGE_SIZE;
 *       mutex_unlock(&sAvailableSwapSpaceLock);
 *
 *       return B_OK;
 *   }
 *
 * ---------------------------------------------------------------------------
 * 7.8  swap_init_post_modules(): leer la configuracion y activar zram.
 * ---------------------------------------------------------------------------
 *
 *   Al INICIO de la funcion (antes del chequeo gReadOnlyBootDevice, para que
 *   zram funcione también en medios de solo lectura), anadir:
 *
 *   +   {
 *   +       void* settings = load_driver_settings("virtual_memory");
 *   +       if (settings != NULL) {
 *   +           const char* size = get_driver_parameter(settings, "zram_size",
 *   +               NULL, NULL);
 *   +           const char* pool = get_driver_parameter(settings,
 *   +               "zram_pool_limit", NULL, NULL);
 *   +           if (size != NULL && pool != NULL) {
 *   +               uint64_t zramSize = atoll(size);
 *   +               uint64_t zramPool = atoll(pool);
 *   +               if (zramSize >= (16ull * 1024 * 1024)
 *   +                       && zramPool >= ZRAM_CHUNK_BYTES
 *   +                       && zramPool % ZRAM_CHUNK_BYTES == 0) {
 *   +                   status_t status = swap_zram_add(zramSize, zramPool);
 *   +                   if (status != B_OK)
 *   +                       dprintf("swap_init_post_modules: swap_zram_add: %s\n",
 *   +                           strerror(status));
 *   +               }
 *   +           }
 *   +           unload_driver_settings(settings);
 *   +       }
 *   +   }
 *
 *   Y registrar el comando de depuracion (puede ir en swap_init):
 *
 *   +   zram_kernel_debug_init();
 *
 * ---------------------------------------------------------------------------
 * 7.9  dump_swap_info(): identificar el swap_file zram.
 * ---------------------------------------------------------------------------
 *
 *   En el bucle de impresion:
 *
 *   +   if (file->zram != NULL)
 *   +       kprintf("  zram: pages: total: %" B_PRIu32 ", free: %" B_PRIu32 "\n",
 *   +           total, file->bmp->free_slots);
 *   +   else
 *           kprintf("  vnode: %p, pages: total: %" B_PRIu32 ", free: %"
 *               B_PRIu32 "\n", file->vnode, total, file->bmp->free_slots);
 *
 * ---------------------------------------------------------------------------
 * 7.10 swap_file_delete(): rechazar archivos zram.
 * ---------------------------------------------------------------------------
 *
 *   Al inicio:
 *
 *   +   if (swapFile->zram != NULL)
 *   +       return B_NOT_SUPPORTED;
 *
 * ---------------------------------------------------------------------------
 * Cabeceras a incluir en VMAnonymousCache.cpp
 * ---------------------------------------------------------------------------
 *
 *   +   #include "zram/zram_kernel.h"   // API de la capa 2
 *
 * (La ruta exacta depende de como se anada zram/ al arbol de Haiku.)
 */
