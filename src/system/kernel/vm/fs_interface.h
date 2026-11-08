/*
 * fs_interface.h - File system interface alignment with Haiku headers
 *
 * This file defines the fs_interface struct that aligns with Haiku kernel headers.
 *
 * Dependencies:
 * - Haiku kernel headers (FDirName, FDirName, etc.)
 * - radix_bitmap.h for bitmap operations
 * - mutex.h for synchronization
 */

#ifndef FS_INTERFACE_H
#define FS_INTERFACE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct vnode;
struct radix_bitmap;
struct ZramKernelDevice;

typedef uint32 swap_addr_t;

/**
 * fs_interface - Structure for file system interface alignment
 * Aligns with Haiku kernel headers for proper integration
 */
struct fs_interface {
    int             fd;                 // File descriptor
    struct vnode*   vnode;             // Vnode pointer for filesystem
    void*           cookie;             // Cookie for file identification
    swap_addr_t     first_slot;        // First swap slot index
    swap_addr_t     last_slot;         // Last swap slot index
    struct radix_bitmap* bmp;          // Bitmap for slot tracking
    struct ZramKernelDevice* zram;     // Optional zram device reference
    char*           path;               // File path string
    int             mode;               // File mode
};

/**
 * Get fs_interface from kernel context
 * Returns pointer to the interface or NULL on failure
 */
static struct fs_interface* get_fs_interface(void) {
    return NULL;
}

/**
 * Create new fs_interface
 * Initializes the structure with default values
 */
static struct fs_interface* create_fs_interface(void) {
    return NULL;
}

#ifdef __cplusplus
}
#endif

#endif	/* FS_INTERFACE_H */
