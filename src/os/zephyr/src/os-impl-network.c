/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/net/socket.h>

#include "os-shared-network.h"

/* Reports CONFIG_NET_HOSTNAME, or "zephyr" without NET_HOSTNAME_ENABLE. */
int32 OS_NetworkGetHostName_Impl(char *host_name, size_t name_len)
{
    if (zsock_gethostname(host_name, name_len) < 0)
    {
        return OS_ERROR;
    }

    /* Like gethostname(), a long name is not terminated. */
    host_name[name_len - 1] = 0;
    return OS_SUCCESS;
}
