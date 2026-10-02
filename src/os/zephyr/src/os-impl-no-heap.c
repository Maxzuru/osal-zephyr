/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Placeholder until heap statistics are ported to Zephyr: reports
 * OS_ERR_NOT_IMPLEMENTED.
 */

#include "os-shared-heap.h"

int32 OS_HeapGetInfo_Impl(OS_heap_prop_t *heap_prop)
{
    return OS_ERR_NOT_IMPLEMENTED;
}
