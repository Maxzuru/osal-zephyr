/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OS_IMPL_DIRS_H
#define OS_IMPL_DIRS_H

#include "osconfig.h"
#include "common_types.h"
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>

#include "os-impl-slot.h"

/* OS_DirectoryRewind reaches the provider through an unpinned shared token,
 * so directories use the slot lifecycle, see os-impl-slot.h. The VFS has no
 * rewind; the directory is reopened from path instead. */
typedef struct
{
    OS_Zephyr_slot_t slot;
    struct k_mutex   lock;
    struct fs_dir_t  dir;
    bool             open;
    char             path[OS_MAX_LOCAL_PATH_LEN];
} OS_impl_dir_internal_record_t;

extern OS_impl_dir_internal_record_t OS_impl_dir_table[OS_MAX_NUM_OPEN_DIRS];

#endif /* OS_IMPL_DIRS_H */
