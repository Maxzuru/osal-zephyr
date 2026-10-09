/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OS_IMPL_FILES_H
#define OS_IMPL_FILES_H

#include "osconfig.h"
#include "common_types.h"
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>

/* A record holds a file or, with CONFIG_CFS_OSAL_NETWORK, a socket. The
 * shared layer pins every call on an open stream, so open and close never
 * overlap other calls on the same record and is_socket is stable within one.
 * Pinned calls on a file may still run concurrently; the lock serializes
 * them, which the Zephyr VFS does not, and keeps a seek and the position it
 * reports together. Sockets are thread-safe and never take the lock, so a
 * blocked receive does not hold up a send. */
typedef struct
{
    struct k_mutex   lock;
    struct fs_file_t file;
    bool             is_socket;
    int              sock;
} OS_impl_file_internal_record_t;

extern OS_impl_file_internal_record_t OS_impl_filehandle_table[OS_MAX_NUM_OPEN_FILES];

#endif /* OS_IMPL_FILES_H */
