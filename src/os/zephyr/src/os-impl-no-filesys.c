/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Placeholder until file systems are ported to Zephyr: every call reports
 * OS_ERR_NOT_IMPLEMENTED.
 */

#include "os-shared-filesys.h"

int32 OS_FileSysStartVolume_Impl(const OS_object_token_t *token)
{
    return OS_ERR_NOT_IMPLEMENTED;
}

int32 OS_FileSysStopVolume_Impl(const OS_object_token_t *token)
{
    return OS_ERR_NOT_IMPLEMENTED;
}

int32 OS_FileSysFormatVolume_Impl(const OS_object_token_t *token)
{
    return OS_ERR_NOT_IMPLEMENTED;
}

int32 OS_FileSysCheckVolume_Impl(const OS_object_token_t *token, bool repair)
{
    return OS_ERR_NOT_IMPLEMENTED;
}

int32 OS_FileSysStatVolume_Impl(const OS_object_token_t *token, OS_statvfs_t *result)
{
    return OS_ERR_NOT_IMPLEMENTED;
}

int32 OS_FileSysMountVolume_Impl(const OS_object_token_t *token)
{
    return OS_ERR_NOT_IMPLEMENTED;
}

int32 OS_FileSysUnmountVolume_Impl(const OS_object_token_t *token)
{
    return OS_ERR_NOT_IMPLEMENTED;
}
