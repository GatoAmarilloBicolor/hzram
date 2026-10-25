/*
 * Copyright 2026, hzram developers.
 * Distributed under the terms of the MIT License.
 *
 * hzramfs - virtual file system backed by the zram page store.
 */
#ifndef HZRAMFS_H
#define HZRAMFS_H

#include <fs_interface.h>
#include <lock.h>

#include <OS.h>

#ifdef __cplusplus
extern "C" {
#endif
#include "zram_core.h"
#ifdef __cplusplus
}
#endif


#define HZRAM_FS_NAME		"hzramfs"
#define HZRAM_FS_PRETTY_NAME	"ZRAM Swap File System"
#define HZRAM_FS_MODULE_NAME	"file_systems/" HZRAM_FS_NAME B_CURRENT_FS_API_VERSION

#define HZRAM_ROOT_ID		1
#define HZRAM_SWAP_ID		2
#define HZRAM_CONTROL_ID	3
#define HZRAM_STATS_ID		4

#define HZRAM_SWAP_NAME		"swap"
#define HZRAM_CONTROL_NAME	".control"
#define HZRAM_STATS_NAME	".stats"

#define HZRAM_DEFAULT_SWAP	(256ULL * 1024 * 1024)
#define HZRAM_MIN_SWAP		(16ULL * 1024 * 1024)
#define HZRAM_MAX_SWAP		(1024ULL * 1024 * 1024 * 1024)
#define HZRAM_MIN_POOL		(16ULL * 1024 * 1024)
#define HZRAM_POOL_ALIGN	(1024ULL * 1024)
#define HZRAM_SECTOR_SIZE	4096


/* Node kinds. */
enum hzram_node_kind {
	HZRAM_NODE_DIRECTORY = 0,
	HZRAM_NODE_SWAP,
	HZRAM_NODE_CONTROL,
	HZRAM_NODE_STATS
};


struct hzram_node {
	ino_t			id;
	mode_t			mode;
	nlink_t			nlink;
	uid_t			uid;
	gid_t			gid;
	uint64			size;		/* logical size (swap only) */
	uint32			mtime;
	uint32			crtime;
	enum hzram_node_kind kind;
};


struct hzram_volume {
	fs_volume*		fsVolume;
	mutex			lock;

	struct hzram_node	root;
	struct hzram_node	swap;
	struct hzram_node	control;
	struct hzram_node	stats;

	struct ZramDevice*	device;
	void*			pool;
	addr_t			poolBase;
	addr_t			poolEnd;
	addr_t			poolBump;
	uint64			diskBytes;
	uint64			poolLimit;
	area_id			poolArea;

	sem_id			refillSem;
	thread_id		refillThread;
	volatile bool	refillDone;

	bool			swapActive;
	char			name[B_FILE_NAME_LENGTH];
};


/* Internal operations (implemented in kernel_interface.cpp). */
status_t hzram_mount(fs_volume* fsVolume, const char* device, uint32 flags,
	const char* args, ino_t* _rootID);
status_t hzram_unmount(fs_volume* fsVolume);
struct hzram_volume* hzram_volume_create(fs_volume* fsVolume,
	const char* args, status_t* _error);
void hzram_volume_destroy(struct hzram_volume* volume);

void hzram_volume_get_stats_string(struct hzram_volume* volume,
	char* buffer, size_t size);

#endif	/* HZRAMFS_H */
