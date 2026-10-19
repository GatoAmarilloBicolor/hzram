/*
 * zram_kernel.h - zram kernel module API
 *
 * This header defines the API used by VMAnonymousCache.cpp to integrate
 * zram into the Haiku swap system.
 *
 * This header is ONLY included from within the kernel module. It should NOT
 * be included from user-space code.
 */

#ifndef ZRAM_KERNEL_H
#define ZRAM_KERNEL_H

#include <kernel/vm/vm.h>
#include <kernel/vm/vm_page.h>
#include <kernel/swap/swap.h>
#include <kernel/swap/swap_file.h>
#include <kernel/swap/swap_slot.h>
#include <kernel/swap/swap.h>

/*
 * ZramDevice - the kernel-side representation of a zram device.
 *
 * This is the public API for the kernel module. It exposes the same
 * operations as zram_core.h but wrapped in the kernel's swap API.
 */
struct ZramDevice;

/*
 * Create a zram device.
 *
 * diskBytes: the nominal size of the device in bytes.
 * poolLimit: the maximum amount of physical memory the pool can use.
 * out: pointer to receive the device pointer.
 *
 * Returns: B_OK on success, B_ERROR on failure.
 */
status_t zram_kernel_create(uint64_t diskBytes, uint64_t poolLimit, ZramDevice** out);

/*
 * Destroy a zram device.
 *
 * Frees all resources associated with the device.
 *
 * firstSlot: the first slot index to free.
 * count: the number of slots to free.
 */
void zram_kernel_free(ZramDevice* dev, swap_addr_t firstSlot, uint32_t count);

/*
 * Check if the device can accept new writes.
 *
 * Returns: true if there is room for at least one more page.
 */
bool zram_kernel_can_accept(ZramDevice* dev);

/*
 * Read pages from the device.
 *
 * pos: the byte offset within the device.
 * vecs: array of generic_io_vec structures.
 * count: number of vectors.
 * flags: IO flags.
 * numBytes: pointer to receive the number of bytes read.
 *
 * Returns: B_OK on success, B_ERROR on failure.
 */
status_t zram_kernel_read(ZramDevice* dev, off_t pos,
                         const generic_io_vec* vecs, size_t count,
                         uint32_t flags, generic_size_t* numBytes);

/*
 * Write pages to the device.
 *
 * pos: the byte offset within the device.
 * vecs: array of generic_io_vec structures.
 * count: number of vectors.
 * flags: IO flags.
 * numBytes: pointer to receive the number of bytes written.
 *
 * Returns: B_OK on success, B_ERROR on failure.
 */
status_t zram_kernel_write(ZramDevice* dev, off_t pos,
                          const generic_io_vec* vecs, size_t count,
                          uint32_t flags, generic_size_t* numBytes);

/*
 * Get statistics about the device.
 *
 * out: pointer to receive the statistics.
 */
void zram_kernel_get_stats(ZramDevice* dev, ZramStats* out);

/*
 * Check invariants (for testing).
 */
bool zram_kernel_check_invariants(ZramDevice* dev);

#endif /* ZRAM_KERNEL_H */