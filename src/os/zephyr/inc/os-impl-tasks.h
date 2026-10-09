/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OS_IMPL_TASKS_H
#define OS_IMPL_TASKS_H

#include "common_types.h"

/* OS_TaskCreate stack contract for Zephyr supervisor tasks:
 *
 * Supply the symbol of a static-duration K_KERNEL_STACK_DEFINE object as
 * stack_pointer and K_KERNEL_STACK_SIZEOF(symbol) as stack_size. Plain byte
 * arrays, the usable-buffer pointer, and user-capable stacks are not part of
 * this contract. The caller must provide the complete declared object and
 * enough usable stack for the entry function, OSAL and native call paths.
 * Supervisor mode cannot verify the declaration or actual storage capacity.
 *
 * Invalid size/rounding geometry returns OS_ERR_INVALID_SIZE; a misaligned
 * base or overflowing address range returns OS_INVALID_POINTER. A claim
 * overlapping another OSAL generation's full stack object (including guard
 * and padding) returns OS_ERROR without waiting for that stack's owner.
 * The claim persists after public ID removal until native join completes.
 * Reuse through OSAL may therefore fail temporarily following self-exit;
 * public ID disappearance alone never permits external reuse of the storage.
 * Caller stacks are never freed. Lifetime must also exclude native threads
 * or other users outside OSAL, whose stack ownership is not tracked here.
 *
 * A NULL pointer returns OS_ERR_NOT_IMPLEMENTED unless
 * CONFIG_CFS_OSAL_DYNAMIC_TASK_STACKS is enabled. In that mode only the
 * stack is allocated, allocation failure returns OS_ERROR, and OSAL frees
 * the owned stack after join. Every task's k_thread is statically stored.
 * CONFIG_USERSPACE task creation is unsupported in both configurations.
 */
int32 OS_Zephyr_TaskAPI_Impl_Init(void);

/* Protect provider-owned resources from native task abort. Enter before
 * acquiring an internal resource, and leave after its final release. These
 * calls nest and do nothing for threads not created by OSAL. A task whose
 * deletion has committed parks in Enter until the deleter aborts it. */
void OS_Zephyr_TaskEnter(void);
void OS_Zephyr_TaskLeave(void);

static inline int32 OS_Zephyr_TaskLeaveResult(int32 status)
{
    OS_Zephyr_TaskLeave();
    return status;
}

struct k_mutex;

/* Abortable provider waits. A guarded task blocked in a native wait holds no
 * provider lock, so OS_TaskDelete may abort it there. Call WaitBegin right
 * before the native wait and WaitEnd once it returns; WaitEnd must run while
 * holding `lock`. The deleter trusts the record only after acquiring `lock`
 * itself: Zephyr can hand a mutex to a waiter before the waiter runs, and
 * holding `lock` excludes both that hand-off and a provider critical section.
 * After the abort, release(arg) runs with `lock` held to undo reservations
 * the waiter kept across the wait; it may be NULL. `lock` must stay valid for
 * the life of the port. A wait inside an outer guard is never abortable,
 * because the outer operation may hold other provider resources. */
void OS_Zephyr_TaskWaitBegin(struct k_mutex *lock, void (*release)(void *arg), void *arg);
void OS_Zephyr_TaskWaitEnd(void);

/* Sliced provider waits, for native waits that are unsafe to abort: k_poll()
 * leaves its event registrations behind when its waiter is aborted. The
 * waiter instead repeats bounded native waits while holding its task's slice
 * lock and calls Pause between them, where it holds no provider lock and a
 * pending OS_TaskDelete may abort it. Pass the result of Begin to Pause and
 * End. Begin returns NULL, and the waits are then not abortable, for threads
 * not created by OSAL and inside an outer guard. After an abort, release(arg)
 * runs as for OS_Zephyr_TaskWaitBegin(); it may be NULL. Keep each native
 * wait well below the time OS_TaskDelete waits for its target.
 *
 * TODO: Wake sliced waiters with an extra per-task descriptor instead, such
 * as a zvfs eventfd that every poll set includes and OS_TaskDelete signals.
 * Waits could then block for their whole timeout, and deletion would not wait
 * for the current slice to end, at the cost of one descriptor per task. */
struct k_mutex *OS_Zephyr_TaskSliceBegin(void (*release)(void *arg), void *arg);
void            OS_Zephyr_TaskSlicePause(struct k_mutex *lock);
void            OS_Zephyr_TaskSliceEnd(struct k_mutex *lock);

/* Timebase helpers must unregister before terminating their native thread. */
void OS_Zephyr_TaskUnregister(void);

#endif /* OS_IMPL_TASKS_H */
