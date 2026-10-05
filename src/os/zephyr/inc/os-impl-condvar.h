/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OS_IMPL_CONDVAR_H
#define OS_IMPL_CONDVAR_H

#include <zephyr/kernel.h>

#include "osconfig.h"
#include "common_types.h"
#include "os-impl-slot.h"

typedef struct
{
    OS_Zephyr_slot_t slot;
    struct k_mutex   lock;
    struct k_condvar changed;
    struct k_mutex   state_lock;
    uint32           depth;
    uint32           waiters;
} OS_impl_condvar_internal_record_t;

extern OS_impl_condvar_internal_record_t OS_impl_condvar_table[OS_MAX_CONDVARS];

#endif /* OS_IMPL_CONDVAR_H */
