/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>

#include "os-impl-dirs.h"
#include "os-impl-tasks.h"
#include "os-shared-dir.h"
#include "os-shared-idmap.h"

OS_impl_dir_internal_record_t OS_impl_dir_table[OS_MAX_NUM_OPEN_DIRS];

/* Like the file provider, every call runs guarded against task abort. */

static bool OS_Zephyr_IsDir(const char *local_path)
{
    struct fs_dirent entry;

    return fs_stat(local_path, &entry) == 0 && entry.type == FS_DIR_ENTRY_DIR;
}

/* Backends release the handle even when closing fails, see OS_GenericClose_Impl. */
static void OS_Zephyr_DirRelease(OS_impl_dir_internal_record_t *impl)
{
    if (impl->open)
    {
        (void)fs_closedir(&impl->dir);
        fs_dir_t_init(&impl->dir);
        impl->open = false;
    }
}

int32 OS_DirCreate_Impl(const char *local_path, uint32 access)
{
    int32 status = OS_SUCCESS;
    int   rc;

    ARG_UNUSED(access);

    OS_Zephyr_TaskEnter();

    rc = fs_mkdir(local_path);
    /* An existing directory is success. */
    if (rc != 0 && (rc != -EEXIST || !OS_Zephyr_IsDir(local_path)))
    {
        status = OS_ERROR;
    }

    return OS_Zephyr_TaskLeaveResult(status);
}

int32 OS_DirOpen_Impl(const OS_object_token_t *token, const char *local_path)
{
    OS_impl_dir_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_dir_table, *token);
    size_t                         len  = strlen(local_path);
    int32                          status;

    if (len >= sizeof(impl->path))
    {
        return OS_FS_ERR_PATH_TOO_LONG;
    }

    OS_Zephyr_TaskEnter();

    if (!OS_Zephyr_SlotInit(&impl->slot, &impl->lock, NULL))
    {
        return OS_Zephyr_TaskLeaveResult(OS_ERROR);
    }
    if (k_mutex_lock(&impl->lock, K_FOREVER) != 0)
    {
        return OS_Zephyr_TaskLeaveResult(OS_ERROR);
    }

    if (impl->slot.active)
    {
        status = OS_ERROR;
    }
    else
    {
        fs_dir_t_init(&impl->dir);
        if (fs_opendir(&impl->dir, local_path) != 0)
        {
            status = OS_ERROR;
        }
        else
        {
            memcpy(impl->path, local_path, len + 1);
            impl->open = true;
            OS_Zephyr_SlotActivate(&impl->slot, token);
            status = OS_SUCCESS;
        }
    }

    k_mutex_unlock(&impl->lock);
    return OS_Zephyr_TaskLeaveResult(status);
}

int32 OS_DirClose_Impl(const OS_object_token_t *token)
{
    OS_impl_dir_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_dir_table, *token);
    int32                          status;

    OS_Zephyr_TaskEnter();

    if (k_mutex_lock(&impl->lock, K_FOREVER) != 0)
    {
        return OS_Zephyr_TaskLeaveResult(OS_ERROR);
    }

    if (!OS_Zephyr_SlotMatches(&impl->slot, token))
    {
        status = OS_ERR_INVALID_ID;
    }
    else
    {
        OS_Zephyr_SlotRetire(&impl->slot);
        OS_Zephyr_DirRelease(impl);
        status = OS_SUCCESS;
    }

    k_mutex_unlock(&impl->lock);
    return OS_Zephyr_TaskLeaveResult(status);
}

int32 OS_DirRead_Impl(const OS_object_token_t *token, os_dirent_t *dirent)
{
    OS_impl_dir_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_dir_table, *token);
    struct fs_dirent               entry;
    int32                          status;

    OS_Zephyr_TaskEnter();

    if (k_mutex_lock(&impl->lock, K_FOREVER) != 0)
    {
        return OS_Zephyr_TaskLeaveResult(OS_ERROR);
    }

    if (!OS_Zephyr_SlotMatches(&impl->slot, token))
    {
        status = OS_ERR_INVALID_ID;
    }
    else if (!impl->open || fs_readdir(&impl->dir, &entry) != 0 || entry.name[0] == '\0')
    {
        /* Failure, a failed rewind, or the end of the directory. */
        status = OS_ERROR;
    }
    else
    {
        strncpy(dirent->FileName, entry.name, sizeof(dirent->FileName) - 1);
        dirent->FileName[sizeof(dirent->FileName) - 1] = '\0';
        status                                         = OS_SUCCESS;
    }

    k_mutex_unlock(&impl->lock);
    return OS_Zephyr_TaskLeaveResult(status);
}

int32 OS_DirRewind_Impl(const OS_object_token_t *token)
{
    OS_impl_dir_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_dir_table, *token);
    int32                          status;

    OS_Zephyr_TaskEnter();

    if (k_mutex_lock(&impl->lock, K_FOREVER) != 0)
    {
        return OS_Zephyr_TaskLeaveResult(OS_ERROR);
    }

    if (!OS_Zephyr_SlotMatches(&impl->slot, token))
    {
        status = OS_ERR_INVALID_ID;
    }
    else
    {
        /* If reopening fails, reads report an error until close. */
        OS_Zephyr_DirRelease(impl);
        fs_dir_t_init(&impl->dir);
        impl->open = fs_opendir(&impl->dir, impl->path) == 0;
        status     = impl->open ? OS_SUCCESS : OS_ERROR;
    }

    k_mutex_unlock(&impl->lock);
    return OS_Zephyr_TaskLeaveResult(status);
}

int32 OS_DirRemove_Impl(const char *local_path)
{
    int32 status = OS_ERROR;

    OS_Zephyr_TaskEnter();

    /* fs_unlink also removes files, rmdir() does not. */
    if (OS_Zephyr_IsDir(local_path) && fs_unlink(local_path) == 0)
    {
        status = OS_SUCCESS;
    }

    return OS_Zephyr_TaskLeaveResult(status);
}
