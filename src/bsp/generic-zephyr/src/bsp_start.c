/*
 * Copyright (c) 2025 Space Cubics
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/fatal.h>
#include <zephyr/kernel.h>

#include "generic_zephyr_bsp_internal.h"
#include "os-impl-tasks.h"

#include <stdlib.h>
#ifdef CONFIG_ARCH_POSIX
#include <posix_board_if.h>
#endif

static K_MUTEX_DEFINE(OS_BSP_GenericZephyrMutex);

static FUNC_NORETURN void OS_BSP_Abort(void)
{
    k_panic();

    /* A custom fatal handler may return; shutdown still must not. */
    k_fatal_halt(K_ERR_KERNEL_PANIC);
}

void OS_BSP_Lock_Impl(void)
{
    OS_Zephyr_TaskEnter();
    if (k_mutex_lock(&OS_BSP_GenericZephyrMutex, K_FOREVER) != 0)
    {
        OS_BSP_Abort();
    }
}

void OS_BSP_Unlock_Impl(void)
{
    if (k_mutex_unlock(&OS_BSP_GenericZephyrMutex) != 0)
    {
        OS_BSP_Abort();
    }
    OS_Zephyr_TaskLeave();
}

void OS_BSP_Shutdown_Impl(void)
{
    OS_BSP_Abort();
}

static int OS_BSP_GetReturnStatus(void)
{
    switch (OS_BSP_Global.AppStatus)
    {
        case OS_SUCCESS:
            return EXIT_SUCCESS;
        case OS_ERROR:
            return EXIT_FAILURE;
        default:
            return OS_BSP_Global.AppStatus & 0x7F;
    }
}

int main(void)
{
    int retcode;

    OS_BSP_Global.ArgC = 0;
    OS_BSP_Global.ArgV = NULL;

    OS_Application_Startup();
    OS_Application_Run();

    retcode = OS_BSP_GetReturnStatus();
    printk("\nApplication exit status: %s (%d)\n",
           OS_BSP_Global.AppStatus == OS_SUCCESS ? "SUCCESS" : "ERROR",
           (int)OS_BSP_Global.AppStatus);

#ifdef CONFIG_ARCH_POSIX
    posix_exit(retcode);
#else
    /* Without a host process there is no exit code. Report failure as a
     * fatal error so test runners detect it without waiting for a timeout. */
    if (OS_BSP_Global.AppStatus != OS_SUCCESS)
    {
        OS_BSP_Abort();
    }
#endif
    return retcode;
}
