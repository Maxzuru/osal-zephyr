/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OS_IMPL_QUEUES_H
#define OS_IMPL_QUEUES_H

#include "osconfig.h"
#include "common_types.h"
#include <zephyr/kernel.h>

typedef struct
{
    struct k_mutex    lock;
    struct k_condvar  changed;
    osal_id_t         object_id;
    bool              initialized;
    bool              active;
    size_t            max_size;
    osal_blockcount_t max_depth;
    osal_blockcount_t head;
    osal_blockcount_t count;
    uint32           *lengths;
    uint8            *data;
} OS_impl_queue_internal_record_t;

extern OS_impl_queue_internal_record_t OS_impl_queue_table[OS_MAX_QUEUES];

#endif /* OS_IMPL_QUEUES_H */
