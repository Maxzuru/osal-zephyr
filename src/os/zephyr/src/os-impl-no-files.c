/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Placeholder until file and stream I/O are ported to Zephyr: every call
 * reports OS_ERR_NOT_IMPLEMENTED.
 */

#include "os-shared-file.h"

int32 OS_GenericSeek_Impl(const OS_object_token_t *token, osal_offset_t offset, uint32 whence)
{
    return OS_ERR_NOT_IMPLEMENTED;
}

int32 OS_GenericRead_Impl(const OS_object_token_t *token, void *buffer, size_t nbytes, OS_time_t abs_timeout)
{
    return OS_ERR_NOT_IMPLEMENTED;
}

int32 OS_GenericWrite_Impl(const OS_object_token_t *token, const void *buffer, size_t nbytes, OS_time_t abs_timeout)
{
    return OS_ERR_NOT_IMPLEMENTED;
}

int32 OS_GenericClose_Impl(const OS_object_token_t *token)
{
    return OS_ERR_NOT_IMPLEMENTED;
}

int32 OS_FileOpen_Impl(const OS_object_token_t *token, const char *local_path, int32 flags, int32 access_mode)
{
    return OS_ERR_NOT_IMPLEMENTED;
}

int32 OS_FileAllocate_Impl(const OS_object_token_t *token, osal_offset_t offset, osal_offset_t len)
{
    return OS_ERR_NOT_IMPLEMENTED;
}

int32 OS_FileTruncate_Impl(const OS_object_token_t *token, osal_offset_t len)
{
    return OS_ERR_NOT_IMPLEMENTED;
}

int32 OS_FileStat_Impl(const char *local_path, os_fstat_t *filestat)
{
    return OS_ERR_NOT_IMPLEMENTED;
}

int32 OS_FileRemove_Impl(const char *local_path)
{
    return OS_ERR_NOT_IMPLEMENTED;
}

int32 OS_FileRename_Impl(const char *old_path, const char *new_path)
{
    return OS_ERR_NOT_IMPLEMENTED;
}

int32 OS_FileChmod_Impl(const char *local_path, uint32 access_mode)
{
    return OS_ERR_NOT_IMPLEMENTED;
}
