/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>

#include "os-impl-files.h"
#include "os-impl-tasks.h"
#include "os-shared-file.h"
#include "os-shared-idmap.h"

OS_impl_file_internal_record_t OS_impl_filehandle_table[OS_MAX_NUM_OPEN_FILES];

/* Every provider call runs guarded, see os-impl-tasks.h: the VFS and its
 * backends hold internal mutexes that an aborted task would never release. */

static OS_impl_file_internal_record_t *OS_Zephyr_FileLock(const OS_object_token_t *token)
{
    OS_impl_file_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_filehandle_table, *token);

    OS_Zephyr_TaskEnter();
    (void)k_mutex_lock(&impl->lock, K_FOREVER);
    return impl;
}

static int32 OS_Zephyr_FileUnlock(OS_impl_file_internal_record_t *impl, int32 status)
{
    k_mutex_unlock(&impl->lock);
    return OS_Zephyr_TaskLeaveResult(status);
}

/* Returns the file size, or a negative errno. The position is kept. */
static off_t OS_Zephyr_FileSize(struct fs_file_t *file)
{
    off_t pos = fs_tell(file);
    off_t size;
    int   rc;

    if (pos < 0)
    {
        return pos;
    }
    rc = fs_seek(file, 0, FS_SEEK_END);
    if (rc != 0)
    {
        return rc;
    }
    size = fs_tell(file);
    rc   = fs_seek(file, pos, FS_SEEK_SET);
    return rc != 0 ? rc : size;
}

/* fs_truncate reports success when the volume fills up while extending the
 * file, so the resulting size is checked. Some backends move the position
 * while resizing; the caller's position is restored. Backends such as
 * littlefs report the new size to fs_stat only once synced. */
static int OS_Zephyr_FileResize(struct fs_file_t *file, off_t length)
{
    off_t pos = fs_tell(file);
    off_t size;
    int   rc;

    if (pos < 0)
    {
        return pos;
    }
    rc = fs_truncate(file, length);
    if (rc == 0)
    {
        rc = fs_sync(file);
    }
    if (rc == 0)
    {
        rc = fs_seek(file, 0, FS_SEEK_END);
    }
    if (rc == 0)
    {
        size = fs_tell(file);
        if (size < 0)
        {
            rc = size;
        }
        else if (size != length)
        {
            rc = -ENOSPC;
        }
    }
    if (fs_seek(file, pos, FS_SEEK_SET) != 0 && rc == 0)
    {
        rc = -EIO;
    }
    return rc;
}

int32 OS_FileOpen_Impl(const OS_object_token_t *token, const char *local_path, int32 flags, int32 access_mode)
{
    OS_impl_file_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_filehandle_table, *token);
    fs_mode_t                       mode;
    int                             rc;

    switch (access_mode)
    {
        case OS_WRITE_ONLY:
            mode = FS_O_WRITE;
            break;
        case OS_READ_ONLY:
            mode = FS_O_READ;
            break;
        case OS_READ_WRITE:
            mode = FS_O_RDWR;
            break;
        default:
            return OS_ERROR;
    }

    if ((flags & OS_FILE_FLAG_CREATE) != 0)
    {
        mode |= FS_O_CREATE;
    }
    if ((flags & OS_FILE_FLAG_TRUNCATE) != 0)
    {
        mode |= FS_O_TRUNC;
    }

    OS_Zephyr_TaskEnter();

    /* Shared allocation excludes every other user of this record, and the
     * previous close left it unlocked. */
    k_mutex_init(&impl->lock);
    fs_file_t_init(&impl->file);

    rc = fs_open(&impl->file, local_path, mode);

    return OS_Zephyr_TaskLeaveResult(rc < 0 ? OS_ERROR : OS_SUCCESS);
}

int32 OS_GenericClose_Impl(const OS_object_token_t *token)
{
    OS_impl_file_internal_record_t *impl = OS_Zephyr_FileLock(token);

    /* Backends release the handle even when the final flush fails, but the
     * VFS then still marks it open; retrying would release it twice. As with
     * POSIX close(), the handle is gone either way. */
    (void)fs_close(&impl->file);
    fs_file_t_init(&impl->file);

    return OS_Zephyr_FileUnlock(impl, OS_SUCCESS);
}

int32 OS_GenericSeek_Impl(const OS_object_token_t *token, osal_offset_t offset, uint32 whence)
{
    OS_impl_file_internal_record_t *impl;
    int                             where;
    off_t                           pos;
    int32                           status;

    switch (whence)
    {
        case OS_SEEK_SET:
            where = FS_SEEK_SET;
            break;
        case OS_SEEK_CUR:
            where = FS_SEEK_CUR;
            break;
        case OS_SEEK_END:
            where = FS_SEEK_END;
            break;
        default:
            return OS_ERROR;
    }

    impl = OS_Zephyr_FileLock(token);

    if (fs_seek(&impl->file, (off_t)offset, where) != 0)
    {
        status = OS_ERROR;
    }
    else
    {
        pos = fs_tell(&impl->file);
        /* The shared API reports the position as int32. */
        status = (pos < 0 || pos > INT32_MAX) ? OS_ERROR : (int32)pos;
    }

    return OS_Zephyr_FileUnlock(impl, status);
}

/* Regular files have no readiness to wait for, so read and write ignore
 * abs_timeout, as POSIX does for handles that are not selectable. Backends
 * need not check the access mode (littlefs only asserts it), so it is
 * checked here. */
int32 OS_GenericRead_Impl(const OS_object_token_t *token, void *buffer, size_t nbytes, OS_time_t abs_timeout)
{
    OS_impl_file_internal_record_t *impl;
    ssize_t                         count;

    ARG_UNUSED(abs_timeout);

    if (nbytes == 0)
    {
        return OS_SUCCESS;
    }

    impl = OS_Zephyr_FileLock(token);
    if ((impl->file.flags & FS_O_READ) == 0)
    {
        return OS_Zephyr_FileUnlock(impl, OS_ERROR);
    }
    count = fs_read(&impl->file, buffer, nbytes);

    return OS_Zephyr_FileUnlock(impl, count < 0 ? OS_ERROR : (int32)count);
}

int32 OS_GenericWrite_Impl(const OS_object_token_t *token, const void *buffer, size_t nbytes, OS_time_t abs_timeout)
{
    OS_impl_file_internal_record_t *impl;
    ssize_t                         count;

    ARG_UNUSED(abs_timeout);

    if (nbytes == 0)
    {
        return OS_SUCCESS;
    }

    impl = OS_Zephyr_FileLock(token);
    if ((impl->file.flags & FS_O_WRITE) == 0)
    {
        return OS_Zephyr_FileUnlock(impl, OS_ERROR);
    }
    count = fs_write(&impl->file, buffer, nbytes);

    return OS_Zephyr_FileUnlock(impl, count < 0 ? OS_ERROR : (int32)count);
}

int32 OS_FileTruncate_Impl(const OS_object_token_t *token, osal_offset_t len)
{
    OS_impl_file_internal_record_t *impl;
    int                             rc;
    int32                           status;

    if (len < 0)
    {
        return OS_ERROR;
    }

    impl = OS_Zephyr_FileLock(token);
    rc   = OS_Zephyr_FileResize(&impl->file, (off_t)len);

    if (rc == 0)
    {
        status = OS_SUCCESS;
    }
    else if (rc == -EACCES || rc == -EPERM || rc == -EROFS || rc == -ENOTSUP)
    {
        status = OS_ERR_OPERATION_NOT_SUPPORTED;
    }
    else if (rc == -EFBIG || rc == -ENOSPC)
    {
        status = OS_ERR_OUTPUT_TOO_LARGE;
    }
    else
    {
        status = OS_ERROR;
    }

    return OS_Zephyr_FileUnlock(impl, status);
}

/* Like posix_fallocate(): the file grows to cover the range and never
 * shrinks. The VFS has no allocation call; extending zero-fills instead. */
int32 OS_FileAllocate_Impl(const OS_object_token_t *token, osal_offset_t offset, osal_offset_t len)
{
    OS_impl_file_internal_record_t *impl;
    off_t                           size;
    int                             rc;
    int32                           status;

    if (offset < 0 || len < 0)
    {
        return OS_ERROR;
    }
    if (offset > LONG_MAX - len)
    {
        return OS_ERR_OUTPUT_TOO_LARGE;
    }

    impl = OS_Zephyr_FileLock(token);
    size = OS_Zephyr_FileSize(&impl->file);

    if (size < 0)
    {
        rc = size;
    }
    else if (size < offset + len)
    {
        rc = OS_Zephyr_FileResize(&impl->file, (off_t)(offset + len));
    }
    else
    {
        rc = 0;
    }

    if (rc == 0)
    {
        status = OS_SUCCESS;
    }
    else if (rc == -ENOTSUP)
    {
        status = OS_ERR_OPERATION_NOT_SUPPORTED;
    }
    else if (rc == -EFBIG || rc == -ENOSPC)
    {
        status = OS_ERR_OUTPUT_TOO_LARGE;
    }
    else
    {
        status = OS_ERROR;
    }

    return OS_Zephyr_FileUnlock(impl, status);
}

/* The VFS keeps no timestamps or permissions. Every entry reports read and
 * write access and a zero modification time. */
int32 OS_FileStat_Impl(const char *local_path, os_fstat_t *filestat)
{
    struct fs_dirent entry;
    int              rc;

    OS_Zephyr_TaskEnter();
    rc = fs_stat(local_path, &entry);
    OS_Zephyr_TaskLeave();

    if (rc != 0)
    {
        return OS_ERROR;
    }

    filestat->FileSize     = entry.size;
    filestat->FileModeBits = OS_FILESTAT_MODE_READ | OS_FILESTAT_MODE_WRITE;
    if (entry.type == FS_DIR_ENTRY_DIR)
    {
        filestat->FileModeBits |= OS_FILESTAT_MODE_DIR;
    }

    return OS_SUCCESS;
}

int32 OS_FileChmod_Impl(const char *local_path, uint32 access_mode)
{
    struct fs_dirent entry;
    int              rc;

    ARG_UNUSED(access_mode);

    OS_Zephyr_TaskEnter();
    rc = fs_stat(local_path, &entry);
    OS_Zephyr_TaskLeave();

    /* Like FAT on POSIX: the file must exist, but has no permissions. */
    return rc != 0 ? OS_ERROR : OS_ERR_NOT_IMPLEMENTED;
}

int32 OS_FileRemove_Impl(const char *local_path)
{
    int rc;

    OS_Zephyr_TaskEnter();
    rc = fs_unlink(local_path);
    OS_Zephyr_TaskLeave();

    return rc != 0 ? OS_ERROR : OS_SUCCESS;
}

int32 OS_FileRename_Impl(const char *old_path, const char *new_path)
{
    int rc;

    OS_Zephyr_TaskEnter();
    rc = fs_rename(old_path, new_path);
    OS_Zephyr_TaskLeave();

    return rc != 0 ? OS_ERROR : OS_SUCCESS;
}
