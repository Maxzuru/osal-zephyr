/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OS_IMPL_SOCKETS_H
#define OS_IMPL_SOCKETS_H

#include "osapi-clock.h"
#include "os-impl-files.h"
#include "os-shared-idmap.h"

/* Stream records shared with files, see os-impl-files.h. The generic stream
 * operations hand socket records to these. */
static inline bool OS_Zephyr_IsSocket(const OS_object_token_t *token)
{
    return IS_ENABLED(CONFIG_CFS_OSAL_NETWORK) && OS_OBJECT_TABLE_GET(OS_impl_filehandle_table, *token)->is_socket;
}

#ifdef CONFIG_CFS_OSAL_NETWORK

int32 OS_Zephyr_SocketClose(const OS_object_token_t *token);
int32 OS_Zephyr_SocketRead(const OS_object_token_t *token, void *buffer, size_t nbytes, OS_time_t abs_timeout);
int32 OS_Zephyr_SocketWrite(const OS_object_token_t *token, const void *buffer, size_t nbytes, OS_time_t abs_timeout);

/* Absolute end of a socket wait in kernel ticks, or INT64_MAX to pend. */
int64_t OS_Zephyr_SocketDeadline(OS_time_t abs_timeout);

/* Next native wait for a sliced socket wait, in the milliseconds zsock_poll()
 * takes: the rest of the timeout, but at most one slice. */
int OS_Zephyr_SocketSliceMs(int64_t deadline);

/* Waits up to the deadline for one of events, or an error or hangup, on the
 * socket of the record that token pins. created, if not NULL, is a new ID
 * that the caller reserved. Should OS_TaskDelete abort the waiter, it
 * releases the pin and discards the new ID, as the shared caller would have.
 * Returns the poll revents, 0 on timeout, or -1. */
int OS_Zephyr_SocketWait(const OS_object_token_t *token, const OS_object_token_t *created, short events,
                         int64_t deadline);

#else

static inline int32 OS_Zephyr_SocketClose(const OS_object_token_t *token)
{
    ARG_UNUSED(token);
    return OS_ERR_NOT_IMPLEMENTED;
}

static inline int32 OS_Zephyr_SocketRead(const OS_object_token_t *token, void *buffer, size_t nbytes,
                                         OS_time_t abs_timeout)
{
    ARG_UNUSED(token);
    ARG_UNUSED(buffer);
    ARG_UNUSED(nbytes);
    ARG_UNUSED(abs_timeout);
    return OS_ERR_NOT_IMPLEMENTED;
}

static inline int32 OS_Zephyr_SocketWrite(const OS_object_token_t *token, const void *buffer, size_t nbytes,
                                          OS_time_t abs_timeout)
{
    ARG_UNUSED(token);
    ARG_UNUSED(buffer);
    ARG_UNUSED(nbytes);
    ARG_UNUSED(abs_timeout);
    return OS_ERR_NOT_IMPLEMENTED;
}

#endif /* CONFIG_CFS_OSAL_NETWORK */

#endif /* OS_IMPL_SOCKETS_H */
