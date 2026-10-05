/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>
#include <zephyr/kernel.h>

#include "os-shared-clock.h"
#include "os-shared-condvar.h"
#include "os-shared-idmap.h"
#include "os-impl-condvar.h"
#include "os-impl-tasks.h"

OS_impl_condvar_internal_record_t OS_impl_condvar_table[OS_MAX_CONDVARS];

/* Slots follow os-impl-slot.h, with lifecycle state protected by
 * state_lock rather than the application mutex, so Signal/Broadcast need
 * not acquire application ownership. When both locks are needed, take the
 * application mutex before the state mutex. */

/* Called by OS_TaskDelete after aborting a waiter, with the application
 * mutex held, so state_lock follows the documented lock order. */
static void OS_Zephyr_CondVarReleaseWaiter(void *arg)
{
    OS_impl_condvar_internal_record_t *impl = arg;

    k_mutex_lock(&impl->state_lock, K_FOREVER);
    --impl->waiters;
    k_mutex_unlock(&impl->state_lock);
}

int32 OS_CondVarCreate_Impl(const OS_object_token_t *token, uint32 options)
{
    OS_impl_condvar_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_condvar_table, *token);

    OS_Zephyr_TaskEnter();

    ARG_UNUSED(options);

    /* state_lock is unused until the slot is initialized, so it may be
     * initialized again if SlotInit failed on an earlier attempt. */
    if ((!impl->slot.initialized && k_mutex_init(&impl->state_lock) != 0) ||
        !OS_Zephyr_SlotInit(&impl->slot, &impl->lock, &impl->changed))
    {
        return OS_Zephyr_TaskLeaveResult(OS_ERROR);
    }

    if (k_mutex_lock(&impl->lock, K_FOREVER) != 0)
    {
        return OS_Zephyr_TaskLeaveResult(OS_ERROR);
    }
    if (k_mutex_lock(&impl->state_lock, K_FOREVER) != 0)
    {
        k_mutex_unlock(&impl->lock);
        return OS_Zephyr_TaskLeaveResult(OS_ERROR);
    }
    if (impl->slot.active)
    {
        k_mutex_unlock(&impl->state_lock);
        k_mutex_unlock(&impl->lock);
        return OS_Zephyr_TaskLeaveResult(OS_ERROR);
    }

    impl->depth   = 0;
    impl->waiters = 0;
    OS_Zephyr_SlotActivate(&impl->slot, token);
    k_mutex_unlock(&impl->state_lock);
    k_mutex_unlock(&impl->lock);

    return OS_Zephyr_TaskLeaveResult(OS_SUCCESS);
}

int32 OS_CondVarDelete_Impl(const OS_object_token_t *token)
{
    OS_impl_condvar_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_condvar_table, *token);
    int32                             status;

    OS_Zephyr_TaskEnter();

    /* The shared table is locked here. Never wait for application ownership
     * or a notifier holding state_lock. A failed delete restores the ID. */
    if (k_mutex_lock(&impl->lock, K_NO_WAIT) != 0)
    {
        return OS_Zephyr_TaskLeaveResult(OS_ERROR);
    }
    if (k_mutex_lock(&impl->state_lock, K_NO_WAIT) != 0)
    {
        k_mutex_unlock(&impl->lock);
        return OS_Zephyr_TaskLeaveResult(OS_ERROR);
    }
    if (!OS_Zephyr_SlotMatches(&impl->slot, token))
    {
        status = OS_ERR_INVALID_ID;
    }
    else if (impl->depth != 0 || impl->waiters != 0)
    {
        status = OS_ERROR;
    }
    else
    {
        OS_Zephyr_SlotRetire(&impl->slot);
        status = OS_SUCCESS;
    }
    k_mutex_unlock(&impl->state_lock);
    k_mutex_unlock(&impl->lock);

    return OS_Zephyr_TaskLeaveResult(status);
}

int32 OS_CondVarLock_Impl(const OS_object_token_t *token)
{
    OS_impl_condvar_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_condvar_table, *token);
    int32                             status;

    OS_Zephyr_TaskEnter();

    if (k_mutex_lock(&impl->lock, K_FOREVER) != 0)
    {
        return OS_Zephyr_TaskLeaveResult(OS_ERROR);
    }
    if (k_mutex_lock(&impl->state_lock, K_FOREVER) != 0)
    {
        k_mutex_unlock(&impl->lock);
        return OS_Zephyr_TaskLeaveResult(OS_ERROR);
    }
    if (!OS_Zephyr_SlotMatches(&impl->slot, token))
    {
        status = OS_ERR_INVALID_ID;
    }
    else if (impl->depth >= UINT32_MAX - 1U)
    {
        /* Reserve one native recursion level for ownership probes. */
        status = OS_ERROR;
    }
    else
    {
        ++impl->depth;
        status = OS_SUCCESS;
    }
    k_mutex_unlock(&impl->state_lock);
    if (status != OS_SUCCESS)
    {
        k_mutex_unlock(&impl->lock);
    }

    return OS_Zephyr_TaskLeaveResult(status);
}

int32 OS_CondVarUnlock_Impl(const OS_object_token_t *token)
{
    OS_impl_condvar_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_condvar_table, *token);
    int32                             status;

    OS_Zephyr_TaskEnter();

    /* Probe ownership without reading native private fields. The recursive
     * acquisition must be released even if the token or depth is invalid. */
    if (k_mutex_lock(&impl->lock, K_NO_WAIT) != 0)
    {
        return OS_Zephyr_TaskLeaveResult(OS_ERROR);
    }
    if (k_mutex_lock(&impl->state_lock, K_FOREVER) != 0)
    {
        k_mutex_unlock(&impl->lock);
        return OS_Zephyr_TaskLeaveResult(OS_ERROR);
    }
    if (!OS_Zephyr_SlotMatches(&impl->slot, token))
    {
        status = OS_ERR_INVALID_ID;
    }
    else if (impl->depth == 0)
    {
        status = OS_ERROR;
    }
    else
    {
        --impl->depth;
        status = OS_SUCCESS;
    }
    k_mutex_unlock(&impl->state_lock);
    k_mutex_unlock(&impl->lock); /* Release the probe. */
    if (status == OS_SUCCESS)
    {
        k_mutex_unlock(&impl->lock); /* Release one application acquisition. */
    }

    return OS_Zephyr_TaskLeaveResult(status);
}

static int32 OS_Zephyr_CondVarNotify(const OS_object_token_t *token, bool broadcast)
{
    OS_impl_condvar_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_condvar_table, *token);
    int32                             status;

    if (k_mutex_lock(&impl->state_lock, K_FOREVER) != 0)
    {
        return OS_ERROR;
    }
    if (!OS_Zephyr_SlotMatches(&impl->slot, token))
    {
        status = OS_ERR_INVALID_ID;
    }
    else
    {
        /* Keep the ID protected through notification. Neither branch needs
         * the application mutex; an awakened waiter may acquire it and then
         * wait for state_lock after this native call reschedules. */
        status = broadcast ? k_condvar_broadcast(&impl->changed) : k_condvar_signal(&impl->changed);
        status = status < 0 ? OS_ERROR : OS_SUCCESS;
    }
    k_mutex_unlock(&impl->state_lock);

    return status;
}

int32 OS_CondVarSignal_Impl(const OS_object_token_t *token)
{
    OS_Zephyr_TaskEnter();

    return OS_Zephyr_TaskLeaveResult(OS_Zephyr_CondVarNotify(token, false));
}

int32 OS_CondVarBroadcast_Impl(const OS_object_token_t *token)
{
    OS_Zephyr_TaskEnter();

    return OS_Zephyr_TaskLeaveResult(OS_Zephyr_CondVarNotify(token, true));
}

/* Shared Wait/TimedWait helper, called inside the caller's task guard. */
static int32 OS_Zephyr_CondVarWait(const OS_object_token_t *token, k_timeout_t timeout)
{
    OS_impl_condvar_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_condvar_table, *token);
    int32                             status;

    if (k_mutex_lock(&impl->lock, K_NO_WAIT) != 0)
    {
        return OS_ERROR;
    }
    if (k_mutex_lock(&impl->state_lock, K_FOREVER) != 0)
    {
        k_mutex_unlock(&impl->lock);
        return OS_ERROR;
    }
    if (!OS_Zephyr_SlotMatches(&impl->slot, token))
    {
        status = OS_ERR_INVALID_ID;
    }
    else if (impl->depth != 1 || impl->waiters == UINT32_MAX)
    {
        /* Native Wait releases the application mutex exactly once. Refuse
         * an unowned or recursively held lock without losing ownership. */
        status = OS_ERROR;
    }
    else
    {
        ++impl->waiters;
        impl->depth = 0;
        status      = OS_SUCCESS;
        /* Abortable once the native Wait releases the application mutex.
         * OS_TaskDelete then releases this waiter reservation instead. */
        OS_Zephyr_TaskWaitBegin(&impl->lock, OS_Zephyr_CondVarReleaseWaiter, impl);
    }
    k_mutex_unlock(&impl->state_lock);
    k_mutex_unlock(&impl->lock); /* Drop only the probe before native Wait. */
    if (status != OS_SUCCESS)
    {
        return status;
    }

    status = k_condvar_wait(&impl->changed, &impl->lock, timeout);
    OS_Zephyr_TaskWaitEnd();

    /* Native Wait reacquires the application mutex even on error. The
     * waiter reservation prevents deletion throughout sleep and reacquire. */
    k_mutex_lock(&impl->state_lock, K_FOREVER);
    impl->depth = 1;
    --impl->waiters;
    k_mutex_unlock(&impl->state_lock);

    if (status == -EAGAIN)
    {
        return OS_ERROR_TIMEOUT;
    }

    return status == 0 ? OS_SUCCESS : OS_ERROR;
}

int32 OS_CondVarWait_Impl(const OS_object_token_t *token)
{
    OS_Zephyr_TaskEnter();

    return OS_Zephyr_TaskLeaveResult(OS_Zephyr_CondVarWait(token, K_FOREVER));
}

/* OSAL deadlines are absolute OS_GetLocalTime() values. Fix the deadline as
 * a relative kernel timeout when the wait starts, so a later
 * OS_SetLocalTime() does not move it. Round up so the waiter never wakes
 * before the deadline. */
static int32 OS_Zephyr_CondVarTimeout(const OS_time_t *abs_wakeup_time, k_timeout_t *timeout)
{
    OS_time_t now;
    uint64_t  remaining;
    uint64_t  seconds;
    uint64_t  ticks;
    uint64_t  limit;
    int32     status;

    status = OS_GetLocalTime_Impl(&now);
    if (status != OS_SUCCESS)
    {
        return status;
    }
    if (abs_wakeup_time->ticks <= now.ticks)
    {
        *timeout = K_NO_WAIT;
        return OS_SUCCESS;
    }

    /* Unsigned subtraction is exact even where the signed one would overflow.
     * Whole seconds are converted separately to keep intermediates small. */
    remaining = (uint64_t)abs_wakeup_time->ticks - (uint64_t)now.ticks;
    seconds   = remaining / OS_TIME_TICKS_PER_SECOND;
    ticks     = k_ns_to_ticks_ceil64((remaining % OS_TIME_TICKS_PER_SECOND) * OS_TIME_TICK_RESOLUTION_NS);

    /* Like other finite waits, refuse a timeout the kernel cannot represent
     * rather than silently shortening it. */
    limit = IS_ENABLED(CONFIG_TIMEOUT_64BIT) ? INT64_MAX : INT32_MAX;
    if (seconds > (limit - ticks) / CONFIG_SYS_CLOCK_TICKS_PER_SEC)
    {
        return OS_ERR_NOT_IMPLEMENTED;
    }

    *timeout = K_TICKS(seconds * CONFIG_SYS_CLOCK_TICKS_PER_SEC + ticks);
    return OS_SUCCESS;
}

int32 OS_CondVarTimedWait_Impl(const OS_object_token_t *token, const OS_time_t *abs_wakeup_time)
{
    k_timeout_t timeout;
    int32       status;

    OS_Zephyr_TaskEnter();

    /* Compute the timeout before taking any condvar lock. */
    status = OS_Zephyr_CondVarTimeout(abs_wakeup_time, &timeout);
    if (status == OS_SUCCESS)
    {
        status = OS_Zephyr_CondVarWait(token, timeout);
    }

    return OS_Zephyr_TaskLeaveResult(status);
}

int32 OS_CondVarGetInfo_Impl(const OS_object_token_t *token, OS_condvar_prop_t *condvar_prop)
{
    OS_Zephyr_TaskEnter();

    ARG_UNUSED(token);
    ARG_UNUSED(condvar_prop);

    /* The shared GLOBAL token protects all currently reported properties. */
    return OS_Zephyr_TaskLeaveResult(OS_SUCCESS);
}
