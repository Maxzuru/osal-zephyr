/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <zephyr/kernel.h>

#include "os-impl-mutex.h"
#include "os-impl-tasks.h"
#include "os-shared-mutex.h"
#include "os-shared-idmap.h"

OS_impl_mutex_internal_record_t OS_impl_mutex_table[OS_MAX_MUTEXES];

/* The native mutex is the slot lock of os-impl-slot.h and also protects
 * the application depth. This does not protect the shared last_owner
 * write performed before Give_Impl is called. */

int32 OS_MutSemCreate_Impl(const OS_object_token_t *token, uint32 options)
{
    OS_impl_mutex_internal_record_t *impl;

    OS_Zephyr_TaskEnter();

    impl = OS_OBJECT_TABLE_GET(OS_impl_mutex_table, *token);

    if (!OS_Zephyr_SlotInit(&impl->slot, &impl->lock, NULL))
    {
        return OS_Zephyr_TaskLeaveResult(OS_SEM_FAILURE);
    }

    if (k_mutex_lock(&impl->lock, K_FOREVER) != 0)
    {
        return OS_Zephyr_TaskLeaveResult(OS_SEM_FAILURE);
    }

    if (impl->slot.active)
    {
        k_mutex_unlock(&impl->lock);
        return OS_Zephyr_TaskLeaveResult(OS_SEM_FAILURE);
    }

    impl->depth = 0;
    OS_Zephyr_SlotActivate(&impl->slot, token);
    k_mutex_unlock(&impl->lock);

    return OS_Zephyr_TaskLeaveResult(OS_SUCCESS);
}

int32 OS_MutSemDelete_Impl(const OS_object_token_t *token)
{
    OS_impl_mutex_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_mutex_table, *token);

    OS_Zephyr_TaskEnter();

    /* Never wait for application ownership under the shared table lock.
     * A recursive probe can succeed for the owner, so depth must also be
     * zero. Native handoff keeps a queued acquirer protected as an owner. */
    if (k_mutex_lock(&impl->lock, K_NO_WAIT) != 0)
    {
        return OS_Zephyr_TaskLeaveResult(OS_SEM_FAILURE);
    }
    if (!OS_Zephyr_SlotMatches(&impl->slot, token))
    {
        k_mutex_unlock(&impl->lock);
        return OS_Zephyr_TaskLeaveResult(OS_ERR_INVALID_ID);
    }
    if (impl->depth != 0)
    {
        k_mutex_unlock(&impl->lock);
        return OS_Zephyr_TaskLeaveResult(OS_SEM_FAILURE);
    }

    OS_Zephyr_SlotRetire(&impl->slot);
    k_mutex_unlock(&impl->lock);

    return OS_Zephyr_TaskLeaveResult(OS_SUCCESS);
}

int32 OS_MutSemGive_Impl(const OS_object_token_t *token)
{
    OS_impl_mutex_internal_record_t *impl;

    OS_Zephyr_TaskEnter();

    impl = OS_OBJECT_TABLE_GET(OS_impl_mutex_table, *token);

    /* A nonblocking recursive probe provides synchronized ID/ownership
     * checks without reading Zephyr's private owner or lock-count fields. */
    if (k_mutex_lock(&impl->lock, K_NO_WAIT) != 0)
    {
        return OS_Zephyr_TaskLeaveResult(OS_SEM_FAILURE);
    }
    if (!OS_Zephyr_SlotMatches(&impl->slot, token))
    {
        k_mutex_unlock(&impl->lock);
        return OS_Zephyr_TaskLeaveResult(OS_ERR_INVALID_ID);
    }
    if (impl->depth == 0)
    {
        k_mutex_unlock(&impl->lock);
        return OS_Zephyr_TaskLeaveResult(OS_SEM_FAILURE);
    }

    --impl->depth;
    k_mutex_unlock(&impl->lock); /* Release the probe acquisition. */
    k_mutex_unlock(&impl->lock); /* Release one application acquisition. */

    return OS_Zephyr_TaskLeaveResult(OS_SUCCESS);
}

int32 OS_MutSemTake_Impl(const OS_object_token_t *token)
{
    OS_impl_mutex_internal_record_t *impl;
    int                              status;

    OS_Zephyr_TaskEnter();

    impl = OS_OBJECT_TABLE_GET(OS_impl_mutex_table, *token);

    /* A waiter holds nothing until the native hand-off makes it the owner.
     * OS_TaskDelete aborts it only while holding this mutex itself. */
    OS_Zephyr_TaskWaitBegin(&impl->lock, NULL, NULL);
    status = k_mutex_lock(&impl->lock, K_FOREVER);
    OS_Zephyr_TaskWaitEnd();
    if (status != 0)
    {
        return OS_Zephyr_TaskLeaveResult(OS_SEM_FAILURE);
    }
    if (!OS_Zephyr_SlotMatches(&impl->slot, token))
    {
        k_mutex_unlock(&impl->lock);
        return OS_Zephyr_TaskLeaveResult(OS_ERR_INVALID_ID);
    }
    /* Reserve one native recursion level for Give/Delete probes and the
     * next Take's validation, so native lock_count cannot wrap first. */
    if (impl->depth >= UINT32_MAX - 1U)
    {
        k_mutex_unlock(&impl->lock);
        return OS_Zephyr_TaskLeaveResult(OS_SEM_FAILURE);
    }
    ++impl->depth;

    return OS_Zephyr_TaskLeaveResult(OS_SUCCESS);
}

int32 OS_MutSemGetInfo_Impl(const OS_object_token_t *token, OS_mut_sem_prop_t *mut_prop)
{
    OS_Zephyr_TaskEnter();

    /* The shared layer fills in name/creator; there is nothing
     * Zephyr-specific to report. */
    (void)token;
    (void)mut_prop;

    return OS_Zephyr_TaskLeaveResult(OS_SUCCESS);
}
