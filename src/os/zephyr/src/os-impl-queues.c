/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/clock.h>

#include "os-impl-queues.h"
#include "os-impl-tasks.h"
#include "os-shared-idmap.h"
#include "os-shared-queue.h"

OS_impl_queue_internal_record_t OS_impl_queue_table[OS_MAX_QUEUES];

K_HEAP_DEFINE(OS_queue_heap, CONFIG_CFS_OSAL_QUEUE_HEAP_SIZE);

/* Get and Put use unpinned shared tokens, see os-impl-countsem.c. The
 * storage is only touched under the slot mutex after the full ID matches. */
static bool OS_Zephyr_QueueMatches(const OS_impl_queue_internal_record_t *impl, const OS_object_token_t *token)
{
    return impl->active && OS_ObjectIdEqual(impl->object_id, OS_ObjectIdFromToken(token));
}

/* Runs with the slot mutex held after OS_TaskDelete aborts a reader. The
 * reader may already have been signaled, so pass that wakeup on. */
static void OS_Zephyr_QueueWaitAborted(void *arg)
{
    OS_impl_queue_internal_record_t *impl = arg;

    if (impl->active && impl->count != 0)
    {
        k_condvar_signal(&impl->changed);
    }
}

int32 OS_QueueCreate_Impl(const OS_object_token_t *token, uint32 flags)
{
    OS_impl_queue_internal_record_t *impl  = OS_OBJECT_TABLE_GET(OS_impl_queue_table, *token);
    OS_queue_internal_record_t      *queue = OS_OBJECT_TABLE_GET(OS_queue_table, *token);
    osal_blockcount_t                depth = queue->max_depth;
    size_t                           size  = queue->max_size;
    uint32                          *storage;

    OS_Zephyr_TaskEnter();

    ARG_UNUSED(flags);

    if (!impl->initialized)
    {
        if (k_mutex_init(&impl->lock) != 0 || k_condvar_init(&impl->changed) != 0)
        {
            return OS_Zephyr_TaskLeaveResult(OS_ERROR);
        }
        impl->initialized = true;
    }

    if (depth == 0 || size > SIZE_MAX / depth - sizeof(uint32))
    {
        return OS_Zephyr_TaskLeaveResult(OS_QUEUE_INVALID_SIZE);
    }

    storage = k_heap_alloc(&OS_queue_heap, depth * (sizeof(uint32) + size), K_NO_WAIT);
    if (storage == NULL)
    {
        return OS_Zephyr_TaskLeaveResult(OS_ERROR);
    }

    if (k_mutex_lock(&impl->lock, K_FOREVER) != 0)
    {
        k_heap_free(&OS_queue_heap, storage);
        return OS_Zephyr_TaskLeaveResult(OS_ERROR);
    }
    if (impl->active)
    {
        k_mutex_unlock(&impl->lock);
        k_heap_free(&OS_queue_heap, storage);
        return OS_Zephyr_TaskLeaveResult(OS_ERROR);
    }

    impl->object_id = OS_ObjectIdFromToken(token);
    impl->max_size  = size;
    impl->max_depth = depth;
    impl->head      = 0;
    impl->count     = 0;
    impl->lengths   = storage;
    impl->data      = (uint8 *)&storage[depth];
    impl->active    = true;
    k_mutex_unlock(&impl->lock);

    return OS_Zephyr_TaskLeaveResult(OS_SUCCESS);
}

int32 OS_QueueDelete_Impl(const OS_object_token_t *token)
{
    OS_impl_queue_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_queue_table, *token);

    OS_Zephyr_TaskEnter();

    if (k_mutex_lock(&impl->lock, K_FOREVER) != 0)
    {
        return OS_Zephyr_TaskLeaveResult(OS_ERROR);
    }
    if (!OS_Zephyr_QueueMatches(impl, token))
    {
        k_mutex_unlock(&impl->lock);
        return OS_Zephyr_TaskLeaveResult(OS_ERR_INVALID_ID);
    }

    impl->active = false;
    k_condvar_broadcast(&impl->changed);
    k_heap_free(&OS_queue_heap, impl->lengths);
    impl->lengths = NULL;
    impl->data    = NULL;
    k_mutex_unlock(&impl->lock);

    return OS_Zephyr_TaskLeaveResult(OS_SUCCESS);
}

int32 OS_QueueGet_Impl(const OS_object_token_t *token, void *data, size_t size, size_t *size_copied, int32 timeout)
{
    OS_impl_queue_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_queue_table, *token);
    k_timepoint_t                    deadline;
    int32                            return_code;
    int                              status;

    OS_Zephyr_TaskEnter();

    *size_copied = 0;

    if (timeout < OS_PEND)
    {
        return OS_Zephyr_TaskLeaveResult(OS_ERR_INVALID_ARGUMENT);
    }
    if (timeout > 0 && !IS_ENABLED(CONFIG_TIMEOUT_64BIT) && k_ms_to_ticks_ceil64(timeout) > INT32_MAX)
    {
        return OS_Zephyr_TaskLeaveResult(OS_ERR_NOT_IMPLEMENTED);
    }
    deadline = sys_timepoint_calc(timeout == OS_PEND ? K_FOREVER : K_MSEC(timeout));

    if (k_mutex_lock(&impl->lock, K_FOREVER) != 0)
    {
        return OS_Zephyr_TaskLeaveResult(OS_ERROR);
    }

    return_code = OS_SUCCESS;
    if (!OS_Zephyr_QueueMatches(impl, token))
    {
        return_code = OS_ERR_INVALID_ID;
    }
    else if (size < impl->max_size)
    {
        return_code = OS_QUEUE_INVALID_SIZE;
    }
    else
    {
        while (impl->count == 0)
        {
            if (timeout == OS_CHECK)
            {
                return_code = OS_QUEUE_EMPTY;
                break;
            }

            OS_Zephyr_TaskWaitBegin(&impl->lock, OS_Zephyr_QueueWaitAborted, impl);
            status = k_condvar_wait(&impl->changed, &impl->lock, sys_timepoint_timeout(deadline));
            OS_Zephyr_TaskWaitEnd();

            if (!OS_Zephyr_QueueMatches(impl, token))
            {
                return_code = OS_ERR_INVALID_ID;
                break;
            }
            if (status != 0 && impl->count == 0)
            {
                return_code = (status == -EAGAIN && timeout != OS_PEND) ? OS_QUEUE_TIMEOUT : OS_ERROR;
                break;
            }
        }
    }

    if (return_code == OS_SUCCESS)
    {
        *size_copied = impl->lengths[impl->head];
        memcpy(data, &impl->data[impl->head * impl->max_size], *size_copied);
        if (++impl->head == impl->max_depth)
        {
            impl->head = 0;
        }
        --impl->count;
    }

    k_mutex_unlock(&impl->lock);

    return OS_Zephyr_TaskLeaveResult(return_code);
}

int32 OS_QueuePut_Impl(const OS_object_token_t *token, const void *data, size_t size, uint32 flags)
{
    OS_impl_queue_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_queue_table, *token);
    osal_blockcount_t                tail;
    int32                            return_code;

    OS_Zephyr_TaskEnter();

    ARG_UNUSED(flags);

    if (k_mutex_lock(&impl->lock, K_FOREVER) != 0)
    {
        return OS_Zephyr_TaskLeaveResult(OS_ERROR);
    }

    if (!OS_Zephyr_QueueMatches(impl, token))
    {
        return_code = OS_ERR_INVALID_ID;
    }
    else if (size > impl->max_size)
    {
        return_code = OS_QUEUE_INVALID_SIZE;
    }
    else if (impl->count == impl->max_depth)
    {
        return_code = OS_QUEUE_FULL;
    }
    else
    {
        tail = impl->head + impl->count;
        if (tail >= impl->max_depth)
        {
            tail -= impl->max_depth;
        }
        impl->lengths[tail] = (uint32)size;
        memcpy(&impl->data[tail * impl->max_size], data, size);
        ++impl->count;
        k_condvar_signal(&impl->changed);
        return_code = OS_SUCCESS;
    }

    k_mutex_unlock(&impl->lock);

    return OS_Zephyr_TaskLeaveResult(return_code);
}

int32 OS_QueueGetInfo_Impl(const OS_object_token_t *token, OS_queue_prop_t *queue_prop)
{
    ARG_UNUSED(token);
    ARG_UNUSED(queue_prop);

    return OS_SUCCESS;
}
