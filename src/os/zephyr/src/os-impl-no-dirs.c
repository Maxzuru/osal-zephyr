/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Placeholder until directories are ported to Zephyr: every call reports
 * OS_ERR_NOT_IMPLEMENTED.
 */

#include "os-shared-dir.h"

int32 OS_DirCreate_Impl(const char *local_path, uint32 access)
{
    return OS_ERR_NOT_IMPLEMENTED;
}

int32 OS_DirOpen_Impl(const OS_object_token_t *token, const char *local_path)
{
    return OS_ERR_NOT_IMPLEMENTED;
}

int32 OS_DirClose_Impl(const OS_object_token_t *token)
{
    return OS_ERR_NOT_IMPLEMENTED;
}

int32 OS_DirRead_Impl(const OS_object_token_t *token, os_dirent_t *dirent)
{
    return OS_ERR_NOT_IMPLEMENTED;
}

int32 OS_DirRewind_Impl(const OS_object_token_t *token)
{
    return OS_ERR_NOT_IMPLEMENTED;
}

int32 OS_DirRemove_Impl(const char *local_path)
{
    return OS_ERR_NOT_IMPLEMENTED;
}
