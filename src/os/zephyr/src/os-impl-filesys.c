/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>

#include "os-impl-tasks.h"
#include "os-shared-filesys.h"
#include "os-shared-idmap.h"

/* OSAL does not mount Zephyr file systems: the application or a devicetree
 * fstab mounts them before OSAL starts. Every OSAL volume is a directory in
 * a mounted file system, created when the volume is mounted:
 *
 *  - A fixed map uses its physical path. A relative path resolves against
 *    CONFIG_CFS_OSAL_FILESYS_ROOT, which stands in for a working directory.
 *  - An OS_mkfs or OS_initfs volume is the directory in the root named after
 *    the device, with '/' replaced by '_'. Its address and geometry are not
 *    used. Formatting removes the directory's contents.
 *
 * Without a root, only fixed maps to absolute paths are available. */

/* Returns the length of the root without trailing separators. */
static size_t OS_Zephyr_FileSysRootLen(void)
{
    size_t len = strlen(CONFIG_CFS_OSAL_FILESYS_ROOT);

    while (len > 0 && CONFIG_CFS_OSAL_FILESYS_ROOT[len - 1] == '/')
    {
        --len;
    }
    return len;
}

/* Removes everything below path, which must be a directory or absent. Each
 * step reopens the directory being emptied, so no listing is open while an
 * entry is removed and the stack does not grow with the depth of the tree. */
static int32 OS_Zephyr_FileSysRemoveContents(char *path, size_t size)
{
    struct fs_dirent entry;
    struct fs_dir_t  dir;
    size_t           base = strlen(path);
    size_t           len  = base;
    size_t           name_len;
    int              rc;

    while (true)
    {
        fs_dir_t_init(&dir);
        rc = fs_opendir(&dir, path);
        if (rc == -ENOENT && len == base)
        {
            return OS_SUCCESS;
        }
        if (rc != 0)
        {
            return OS_ERROR;
        }
        rc = fs_readdir(&dir, &entry);
        (void)fs_closedir(&dir);
        if (rc != 0)
        {
            return OS_ERROR;
        }

        if (entry.name[0] == '\0')
        {
            if (len == base)
            {
                return OS_SUCCESS;
            }
            /* Remove the emptied directory and continue with its parent. */
            if (fs_unlink(path) != 0)
            {
                return OS_ERROR;
            }
            while (path[--len] != '/')
            {
            }
            path[len] = '\0';
            continue;
        }

        name_len = strlen(entry.name);
        if (len + 1 + name_len >= size)
        {
            return OS_FS_ERR_PATH_TOO_LONG;
        }
        path[len] = '/';
        memcpy(&path[len + 1], entry.name, name_len + 1);

        if (entry.type == FS_DIR_ENTRY_DIR)
        {
            len += 1 + name_len;
        }
        else
        {
            if (fs_unlink(path) != 0)
            {
                return OS_ERROR;
            }
            path[len] = '\0';
        }
    }
}

int32 OS_FileSysStartVolume_Impl(const OS_object_token_t *token)
{
    OS_filesys_internal_record_t *local = OS_OBJECT_TABLE_GET(OS_filesys_table, *token);
    char                          path[sizeof(local->system_mountpt)];
    size_t                        root_len = OS_Zephyr_FileSysRootLen();
    const char                   *name;
    char                         *sep;
    int                           len;

    if (local->fstype == OS_FILESYS_TYPE_FS_BASED)
    {
        if (local->system_mountpt[0] == '/')
        {
            return OS_SUCCESS;
        }
        name = local->system_mountpt;
        while (name[0] == '.' && name[1] == '/')
        {
            name += 2;
        }
        if (root_len == 0 || name[0] == '\0')
        {
            return OS_FS_ERR_PATH_INVALID;
        }
    }
    else
    {
        name = local->device_name;
        while (name[0] == '/')
        {
            ++name;
        }
        if (root_len == 0 || name[0] == '\0')
        {
            return OS_FS_ERR_DRIVE_NOT_CREATED;
        }
    }

    len = snprintf(path, sizeof(path), "%.*s/%s", (int)root_len, CONFIG_CFS_OSAL_FILESYS_ROOT, name);
    if (len < 0 || (size_t)len >= sizeof(path))
    {
        return OS_FS_ERR_PATH_TOO_LONG;
    }

    if (local->fstype != OS_FILESYS_TYPE_FS_BASED)
    {
        for (sep = strchr(&path[root_len + 1], '/'); sep != NULL; sep = strchr(sep, '/'))
        {
            *sep = '_';
        }
    }

    memcpy(local->system_mountpt, path, (size_t)len + 1);
    return OS_SUCCESS;
}

int32 OS_FileSysStopVolume_Impl(const OS_object_token_t *token)
{
    /* The directory stays, like a stopped disk keeps its contents. */
    return OS_SUCCESS;
}

int32 OS_FileSysFormatVolume_Impl(const OS_object_token_t *token)
{
    OS_filesys_internal_record_t *local = OS_OBJECT_TABLE_GET(OS_filesys_table, *token);
    char                          path[sizeof(local->system_mountpt)];
    int32                         status;

    memcpy(path, local->system_mountpt, sizeof(path));

    OS_Zephyr_TaskEnter();
    status = OS_Zephyr_FileSysRemoveContents(path, sizeof(path));
    return OS_Zephyr_TaskLeaveResult(status);
}

int32 OS_FileSysMountVolume_Impl(const OS_object_token_t *token)
{
    OS_filesys_internal_record_t *local = OS_OBJECT_TABLE_GET(OS_filesys_table, *token);
    struct fs_dirent              entry;
    int                           rc;

    OS_Zephyr_TaskEnter();

    rc = fs_stat(local->system_mountpt, &entry);
    if (rc != 0)
    {
        rc = fs_mkdir(local->system_mountpt);
    }
    else if (entry.type != FS_DIR_ENTRY_DIR)
    {
        rc = -ENOTDIR;
    }

    return OS_Zephyr_TaskLeaveResult(rc != 0 ? OS_FS_ERR_DRIVE_NOT_CREATED : OS_SUCCESS);
}

int32 OS_FileSysUnmountVolume_Impl(const OS_object_token_t *token)
{
    /* The directory stays for the next mount. */
    return OS_SUCCESS;
}

int32 OS_FileSysStatVolume_Impl(const OS_object_token_t *token, OS_statvfs_t *result)
{
    OS_filesys_internal_record_t *local = OS_OBJECT_TABLE_GET(OS_filesys_table, *token);
    struct fs_statvfs             stat;
    int                           rc;

    OS_Zephyr_TaskEnter();
    rc = fs_statvfs(local->system_mountpt, &stat);
    OS_Zephyr_TaskLeave();

    if (rc != 0)
    {
        return OS_ERROR;
    }

    /* Block counts are in fragment units. The statistics cover the whole
     * Zephyr file system holding the volume. */
    result->block_size   = OSAL_SIZE_C(stat.f_frsize);
    result->blocks_free  = OSAL_BLOCKCOUNT_C(stat.f_bfree);
    result->total_blocks = OSAL_BLOCKCOUNT_C(stat.f_blocks);

    return OS_SUCCESS;
}

int32 OS_FileSysCheckVolume_Impl(const OS_object_token_t *token, bool repair)
{
    return OS_ERR_NOT_IMPLEMENTED;
}
