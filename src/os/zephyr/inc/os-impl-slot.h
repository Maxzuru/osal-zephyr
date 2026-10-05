/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OS_IMPL_SLOT_H
#define OS_IMPL_SLOT_H

#include <zephyr/kernel.h>

#include "os-shared-idmap.h"

/* Lifecycle of a provider slot reached through unpinned shared tokens.
 *
 * A NONE token can outlive deletion and slot reuse before it enters a
 * provider. Each slot's kernel objects are therefore initialized once,
 * serialized by shared allocation, and never reinitialized: a delayed call
 * may still be about to lock them. Activate, Retire and Matches run with
 * the slot's lock held, and Matches is repeated after every wait before
 * any other state is used. */
typedef struct
{
    osal_id_t object_id;
    bool      initialized;
    bool      active;
} OS_Zephyr_slot_t;

/* Initializes lock and, if not NULL, changed on first use of the slot. */
static inline bool OS_Zephyr_SlotInit(OS_Zephyr_slot_t *slot, struct k_mutex *lock, struct k_condvar *changed)
{
    if (!slot->initialized)
    {
        if (k_mutex_init(lock) != 0 || (changed != NULL && k_condvar_init(changed) != 0))
        {
            return false;
        }
        slot->initialized = true;
    }
    return true;
}

static inline void OS_Zephyr_SlotActivate(OS_Zephyr_slot_t *slot, const OS_object_token_t *token)
{
    slot->object_id = OS_ObjectIdFromToken(token);
    slot->active    = true;
}

static inline void OS_Zephyr_SlotRetire(OS_Zephyr_slot_t *slot)
{
    slot->active = false;
}

static inline bool OS_Zephyr_SlotMatches(const OS_Zephyr_slot_t *slot, const OS_object_token_t *token)
{
    return slot->active && OS_ObjectIdEqual(slot->object_id, OS_ObjectIdFromToken(token));
}

#endif /* OS_IMPL_SLOT_H */
