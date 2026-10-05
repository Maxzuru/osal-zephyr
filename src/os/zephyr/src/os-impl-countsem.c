/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/clock.h>

#include "os-impl-countsem.h"
#include "os-impl-tasks.h"
#include "os-shared-countsem.h"
#include "os-shared-idmap.h"

OS_impl_countsem_internal_record_t OS_impl_count_sem_table[OS_MAX_COUNT_SEMAPHORES];

/* Slots follow os-impl-slot.h. No reference or waiter count is retained
 * across a kernel wait, so OS_TaskDelete may abort a waiter there. All
 * operations are thread-context APIs, including Give. */

int32 OS_CountSemCreate_Impl(const OS_object_token_t *token, uint32 sem_initial_value, uint32 options)
{
    OS_impl_countsem_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_count_sem_table, *token);

    OS_Zephyr_TaskEnter();

    ARG_UNUSED(options);

    /* GetInfo reports a signed 32-bit count. Never accept an initial value
     * that cannot be represented by that public property. */
    if (sem_initial_value > INT32_MAX)
    {
        return OS_Zephyr_TaskLeaveResult(OS_INVALID_SEM_VALUE);
    }

    if (!OS_Zephyr_SlotInit(&impl->slot, &impl->lock, &impl->changed))
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
    impl->current_value = sem_initial_value;
    OS_Zephyr_SlotActivate(&impl->slot, token);
    k_mutex_unlock(&impl->lock);
    return OS_Zephyr_TaskLeaveResult(OS_SUCCESS);
}

int32 OS_CountSemDelete_Impl(const OS_object_token_t *token)
{
    OS_impl_countsem_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_count_sem_table, *token);

    OS_Zephyr_TaskEnter();

    /* The shared EXCLUSIVE transaction reserves the public ID. Never wait
     * for a giver; failure restores that ID. A sleeping waiter has released
     * this lock and may be invalidated safely. */
    if (k_mutex_lock(&impl->lock, K_NO_WAIT) != 0)
    {
        return OS_Zephyr_TaskLeaveResult(OS_SEM_FAILURE);
    }
    if (!OS_Zephyr_SlotMatches(&impl->slot, token))
    {
        k_mutex_unlock(&impl->lock);
        return OS_Zephyr_TaskLeaveResult(OS_ERR_INVALID_ID);
    }

    OS_Zephyr_SlotRetire(&impl->slot);
    k_condvar_broadcast(&impl->changed);
    k_mutex_unlock(&impl->lock);
    return OS_Zephyr_TaskLeaveResult(OS_SUCCESS);
}

int32 OS_CountSemGive_Impl(const OS_object_token_t *token)
{
    OS_impl_countsem_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_count_sem_table, *token);
    int32                             status;

    OS_Zephyr_TaskEnter();

    if (k_mutex_lock(&impl->lock, K_FOREVER) != 0)
    {
        return OS_Zephyr_TaskLeaveResult(OS_SEM_FAILURE);
    }
    if (!OS_Zephyr_SlotMatches(&impl->slot, token))
    {
        status = OS_ERR_INVALID_ID;
    }
    else if (impl->current_value == INT32_MAX)
    {
        status = OS_SEM_FAILURE;
    }
    else
    {
        ++impl->current_value;
        /* Delete broadcasts before reuse, and old waiters cannot requeue
         * after their ID check fails. This signal therefore serves the
         * current generation only. */
        k_condvar_signal(&impl->changed);
        status = OS_SUCCESS;
    }
    k_mutex_unlock(&impl->lock);
    return OS_Zephyr_TaskLeaveResult(status);
}

/*
 * Shared Take/TimedWait helper. "deadline" is computed by the caller via
 * sys_timepoint_calc() so that a spurious wakeup re-derives the remaining
 * budget instead of restarting a fresh K_MSEC() window each iteration.
 */
static int32 OS_Zephyr_CountSemTake(const OS_object_token_t *token, k_timepoint_t deadline)
{
    OS_impl_countsem_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_count_sem_table, *token);
    int32                             return_code;
    int                               status;

    status = k_mutex_lock(&impl->lock, sys_timepoint_timeout(deadline));
    if (status != 0)
    {
        return status == -EAGAIN || status == -EBUSY ? OS_SEM_TIMEOUT : OS_SEM_FAILURE;
    }
    if (!OS_Zephyr_SlotMatches(&impl->slot, token))
    {
        k_mutex_unlock(&impl->lock);
        return OS_ERR_INVALID_ID;
    }

    return_code = OS_SUCCESS;
    while (impl->current_value == 0)
    {
        OS_Zephyr_TaskWaitBegin(&impl->lock, NULL, NULL);
        status = k_condvar_wait(&impl->changed, &impl->lock, sys_timepoint_timeout(deadline));
        OS_Zephyr_TaskWaitEnd();
        /* The permanent lock is reacquired even when the old object has
         * been deleted and this slot already belongs to another ID. */
        if (!OS_Zephyr_SlotMatches(&impl->slot, token))
        {
            return_code = OS_ERR_INVALID_ID;
            break;
        }
        if (status != 0)
        {
            return_code = status == -EAGAIN ? OS_SEM_TIMEOUT : OS_SEM_FAILURE;
            break;
        }
    }

    if (return_code == OS_SUCCESS)
    {
        --impl->current_value;
    }

    k_mutex_unlock(&impl->lock);

    return return_code;
}

int32 OS_CountSemTake_Impl(const OS_object_token_t *token)
{
    OS_Zephyr_TaskEnter();

    return OS_Zephyr_TaskLeaveResult(OS_Zephyr_CountSemTake(token, sys_timepoint_calc(K_FOREVER)));
}

int32 OS_CountSemTimedWait_Impl(const OS_object_token_t *token, uint32 msecs)
{
    OS_Zephyr_TaskEnter();

    /* A clockless kernel turns positive timeouts into infinite waits.
     * A 32-bit tick conversion can wrap a large millisecond argument.
     * Refuse unsupported finite waits rather than silently changing them. */
    if (msecs != 0 && !IS_ENABLED(CONFIG_SYS_CLOCK_EXISTS))
    {
        return OS_Zephyr_TaskLeaveResult(OS_ERR_NOT_IMPLEMENTED);
    }
    if (!IS_ENABLED(CONFIG_TIMEOUT_64BIT) && k_ms_to_ticks_ceil64(msecs) > INT32_MAX)
    {
        return OS_Zephyr_TaskLeaveResult(OS_ERR_NOT_IMPLEMENTED);
    }
    return OS_Zephyr_TaskLeaveResult(OS_Zephyr_CountSemTake(token, sys_timepoint_calc(K_MSEC(msecs))));
}

int32 OS_CountSemGetInfo_Impl(const OS_object_token_t *token, OS_count_sem_prop_t *count_prop)
{
    OS_impl_countsem_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_count_sem_table, *token);

    OS_Zephyr_TaskEnter();

    if (k_mutex_lock(&impl->lock, K_FOREVER) != 0)
    {
        return OS_Zephyr_TaskLeaveResult(OS_SEM_FAILURE);
    }
    if (!OS_Zephyr_SlotMatches(&impl->slot, token))
    {
        k_mutex_unlock(&impl->lock);
        return OS_Zephyr_TaskLeaveResult(OS_ERR_INVALID_ID);
    }
    count_prop->value = (int32)impl->current_value;
    k_mutex_unlock(&impl->lock);
    return OS_Zephyr_TaskLeaveResult(OS_SUCCESS);
}
