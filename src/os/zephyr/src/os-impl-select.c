/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>

#include "os-impl-sockets.h"
#include "os-impl-tasks.h"
#include "os-shared-idmap.h"
#include "os-shared-select.h"

/* Only sockets are selectable; files report OS_ERR_OPERATION_NOT_SUPPORTED,
 * as handles that are not selectable do on POSIX. */

#define OS_ZEPHYR_SELECT_MAX MIN(OS_MAX_NUM_OPEN_FILES, CONFIG_ZVFS_POLL_MAX)

static short OS_Zephyr_SelectEvents(uint32 flags)
{
    short events = 0;

    if ((flags & OS_STREAM_STATE_READABLE) != 0)
    {
        events |= ZSOCK_POLLIN;
    }
    if ((flags & OS_STREAM_STATE_WRITABLE) != 0)
    {
        events |= ZSOCK_POLLOUT;
    }
    return events;
}

/* As with select(), a pending error or hangup makes a socket ready for both
 * directions: the next call returns at once. */
static uint32 OS_Zephyr_SelectReady(short revents)
{
    uint32 flags = 0;

    if ((revents & (ZSOCK_POLLIN | ZSOCK_POLLERR | ZSOCK_POLLHUP)) != 0)
    {
        flags |= OS_STREAM_STATE_READABLE;
    }
    if ((revents & (ZSOCK_POLLOUT | ZSOCK_POLLERR | ZSOCK_POLLHUP)) != 0)
    {
        flags |= OS_STREAM_STATE_WRITABLE;
    }
    return flags;
}

int32 OS_SelectSingle_Impl(const OS_object_token_t *token, uint32 *SelectFlags, OS_time_t abs_timeout)
{
    int64_t deadline;
    int     revents;

    if (!OS_Zephyr_IsSocket(token))
    {
        return OS_ERR_OPERATION_NOT_SUPPORTED;
    }
    if (*SelectFlags == 0)
    {
        return OS_SUCCESS;
    }
    deadline = OS_Zephyr_SocketDeadline(abs_timeout);

    OS_Zephyr_TaskEnter();
    revents = OS_Zephyr_SocketWait(token, NULL, OS_Zephyr_SelectEvents(*SelectFlags), deadline);
    OS_Zephyr_TaskLeave();

    if (revents <= 0 || (revents & ZSOCK_POLLNVAL) != 0)
    {
        *SelectFlags = 0;
        return revents == 0 ? OS_ERROR_TIMEOUT : OS_ERROR;
    }
    *SelectFlags &= OS_Zephyr_SelectReady(revents);
    return OS_SUCCESS;
}

static bool OS_Zephyr_FdIsSet(const OS_FdSet *set, osal_index_t idx)
{
    return set != NULL && (set->object_ids[idx / 8] & (1 << (idx % 8))) != 0;
}

static void OS_Zephyr_FdSet(OS_FdSet *set, osal_index_t idx)
{
    set->object_ids[idx / 8] |= 1 << (idx % 8);
}

/* Sockets pinned for one slice of OS_SelectMultiple. */
typedef struct
{
    struct zsock_pollfd fds[OS_ZEPHYR_SELECT_MAX];
    OS_object_token_t   tokens[OS_ZEPHYR_SELECT_MAX];
    int                 count;
} OS_Zephyr_select_slice_t;

static void OS_Zephyr_SelectUnpin(OS_Zephyr_select_slice_t *slice)
{
    while (slice->count > 0)
    {
        --slice->count;
        OS_ObjectIdRelease(&slice->tokens[slice->count]);
    }
}

/* The shared layer pins nothing here. A socket closed while it is polled
 * could reinitialize the network context that k_poll() still waits on, so
 * every slice pins its sockets and skips those already closed, as select()
 * does. Returns OS_SUCCESS with at least one socket pinned. */
static int32 OS_Zephyr_SelectPin(OS_Zephyr_select_slice_t *slice, const OS_FdSet *ReadSet, const OS_FdSet *WriteSet)
{
    OS_impl_file_internal_record_t *impl;
    OS_object_token_t               token;
    osal_index_t                    idx;
    osal_id_t                       id;
    uint32                          flags;

    slice->count = 0;
    for (idx = 0; idx < OS_MAX_NUM_OPEN_FILES; ++idx)
    {
        flags = 0;
        if (OS_Zephyr_FdIsSet(ReadSet, idx))
        {
            flags |= OS_STREAM_STATE_READABLE;
        }
        if (OS_Zephyr_FdIsSet(WriteSet, idx))
        {
            flags |= OS_STREAM_STATE_WRITABLE;
        }
        if (flags == 0)
        {
            continue;
        }

        id = OS_global_stream_table[idx].active_id;
        if (!OS_ObjectIdDefined(id) ||
            OS_ObjectIdGetById(OS_LOCK_MODE_REFCOUNT, OS_OBJECT_TYPE_OS_STREAM, id, &token) != OS_SUCCESS)
        {
            continue;
        }

        impl = OS_OBJECT_TABLE_GET(OS_impl_filehandle_table, token);
        if (!impl->is_socket || slice->count == OS_ZEPHYR_SELECT_MAX)
        {
            /* Files cannot be polled, and zsock_poll() takes at most
             * ZVFS_POLL_MAX entries. */
            OS_ObjectIdRelease(&token);
            OS_Zephyr_SelectUnpin(slice);
            return OS_ERR_OPERATION_NOT_SUPPORTED;
        }

        slice->fds[slice->count].fd      = impl->sock;
        slice->fds[slice->count].events  = OS_Zephyr_SelectEvents(flags);
        slice->fds[slice->count].revents = 0;
        slice->tokens[slice->count]      = token;
        ++slice->count;
    }

    return slice->count > 0 ? OS_SUCCESS : OS_ERR_INVALID_ID;
}

int32 OS_SelectMultiple_Impl(OS_FdSet *ReadSet, OS_FdSet *WriteSet, OS_time_t abs_timeout)
{
    OS_Zephyr_select_slice_t slice;
    OS_FdSet                 ready_read;
    OS_FdSet                 ready_write;
    struct k_mutex          *lock;
    int64_t                  deadline = OS_Zephyr_SocketDeadline(abs_timeout);
    uint32                   ready;
    int32                    status;
    int                      rc;
    int                      i;

    OS_Zephyr_TaskEnter();

    /* Sockets are pinned only within a slice, so an abort between slices
     * leaves nothing to release. */
    lock = OS_Zephyr_TaskSliceBegin(NULL, NULL);
    for (;;)
    {
        status = OS_Zephyr_SelectPin(&slice, ReadSet, WriteSet);
        if (status != OS_SUCCESS)
        {
            break;
        }

        rc = zsock_poll(slice.fds, slice.count, OS_Zephyr_SocketSliceMs(deadline));
        if (rc > 0)
        {
            memset(&ready_read, 0, sizeof(ready_read));
            memset(&ready_write, 0, sizeof(ready_write));
            for (i = 0; i < slice.count; ++i)
            {
                if ((slice.fds[i].revents & ZSOCK_POLLNVAL) != 0)
                {
                    status = OS_ERROR;
                }
                ready = OS_Zephyr_SelectReady(slice.fds[i].revents);
                if ((ready & OS_STREAM_STATE_READABLE) != 0 && OS_Zephyr_FdIsSet(ReadSet, slice.tokens[i].obj_idx))
                {
                    OS_Zephyr_FdSet(&ready_read, slice.tokens[i].obj_idx);
                }
                if ((ready & OS_STREAM_STATE_WRITABLE) != 0 && OS_Zephyr_FdIsSet(WriteSet, slice.tokens[i].obj_idx))
                {
                    OS_Zephyr_FdSet(&ready_write, slice.tokens[i].obj_idx);
                }
            }
            if (status == OS_SUCCESS)
            {
                if (ReadSet != NULL)
                {
                    *ReadSet = ready_read;
                }
                if (WriteSet != NULL)
                {
                    *WriteSet = ready_write;
                }
            }
        }
        OS_Zephyr_SelectUnpin(&slice);

        if (rc != 0)
        {
            status = rc < 0 ? OS_ERROR : status;
            break;
        }
        if (deadline != INT64_MAX && k_uptime_ticks() >= deadline)
        {
            status = OS_ERROR_TIMEOUT;
            break;
        }
        OS_Zephyr_TaskSlicePause(lock);
    }
    OS_Zephyr_TaskSliceEnd(lock);

    return OS_Zephyr_TaskLeaveResult(status);
}
