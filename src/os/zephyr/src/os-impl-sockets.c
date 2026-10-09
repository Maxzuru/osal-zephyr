/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/fdtable.h>

#include "os-impl-sockets.h"
#include "os-impl-tasks.h"
#include "os-shared-file.h"
#include "os-shared-idmap.h"
#include "os-shared-sockets.h"

/* Every native call runs guarded, see os-impl-tasks.h: the network stack
 * holds internal mutexes that an aborted task would never release. Sockets
 * are non-blocking. Calls that may block try the operation first and wait
 * for readiness only when it would block. */

/* Each address type must fit the opaque OSAL address. Only the IP families
 * are stored, so a larger net_sockaddr (e.g. with NET_SOCKETS_UNIX) is fine. */
BUILD_ASSERT(sizeof(struct net_sockaddr_in) <= sizeof(((OS_SockAddr_t *)0)->AddrData));
#ifdef CONFIG_NET_IPV6
BUILD_ASSERT(sizeof(struct net_sockaddr_in6) <= sizeof(((OS_SockAddr_t *)0)->AddrData));
#endif

/* Both IP address types start with the family and the port. */
static struct net_sockaddr_in *OS_Zephyr_SockAddrIn(OS_SockAddr_t *Addr)
{
    return (struct net_sockaddr_in *)&Addr->AddrData;
}

static const struct net_sockaddr_in *OS_Zephyr_ConstSockAddrIn(const OS_SockAddr_t *Addr)
{
    return (const struct net_sockaddr_in *)&Addr->AddrData;
}

/* Returns the native length of an address of family, or 0 if the family is
 * not supported. */
static net_socklen_t OS_Zephyr_SockAddrLen(net_sa_family_t family)
{
    if (IS_ENABLED(CONFIG_NET_IPV4) && family == NET_AF_INET)
    {
        return sizeof(struct net_sockaddr_in);
    }
#ifdef CONFIG_NET_IPV6
    if (family == NET_AF_INET6)
    {
        return sizeof(struct net_sockaddr_in6);
    }
#endif
    return 0;
}

/* Returns a pointer to the IP address in Addr, or NULL. */
static void *OS_Zephyr_SockAddrIp(OS_SockAddr_t *Addr)
{
    net_sa_family_t family = OS_Zephyr_SockAddrIn(Addr)->sin_family;

    if (OS_Zephyr_SockAddrLen(family) == 0)
    {
        return NULL;
    }
    if (family == NET_AF_INET)
    {
        return &OS_Zephyr_SockAddrIn(Addr)->sin_addr;
    }
    return &((struct net_sockaddr_in6 *)&Addr->AddrData)->sin6_addr;
}

/* Puts a new or accepted socket in non-blocking mode. A blocking socket
 * could block a guarded call, where OS_TaskDelete cannot abort its task. */
static int32 OS_Zephyr_SocketSetNonBlock(int sock)
{
    int flags = zsock_fcntl(sock, ZVFS_F_GETFL, 0);

    if (flags < 0 || zsock_fcntl(sock, ZVFS_F_SETFL, flags | ZVFS_O_NONBLOCK) < 0)
    {
        return OS_ERROR;
    }
    return OS_SUCCESS;
}

static bool OS_Zephyr_SocketWouldBlock(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

/*----------------------------------------------------------------
 * Sliced waits, see OS_Zephyr_TaskSliceBegin()
 *-----------------------------------------------------------------*/

int64_t OS_Zephyr_SocketDeadline(OS_time_t abs_timeout)
{
    int32 msecs = OS_TimeToRelativeMilliseconds(abs_timeout);

    /* Measured on the uptime clock, so setting the local time after the
     * conversion does not move it. */
    if (msecs == OS_PEND)
    {
        return INT64_MAX;
    }
    return k_uptime_ticks() + (int64_t)k_ms_to_ticks_ceil64((uint64_t)msecs);
}

int OS_Zephyr_SocketSliceMs(int64_t deadline)
{
    int64_t  remaining;
    uint64_t msecs;

    if (deadline == INT64_MAX)
    {
        return CONFIG_CFS_OSAL_NETWORK_WAIT_SLICE_MS;
    }
    remaining = deadline - k_uptime_ticks();
    if (remaining <= 0)
    {
        return 0;
    }
    msecs = k_ticks_to_ms_ceil64((uint64_t)remaining);
    return msecs < CONFIG_CFS_OSAL_NETWORK_WAIT_SLICE_MS ? (int)msecs : CONFIG_CFS_OSAL_NETWORK_WAIT_SLICE_MS;
}

/* Shared pins held across a socket wait. The waiter's stack, which holds
 * this record, stays intact until OS_TaskDelete has joined the task. */
typedef struct
{
    OS_object_token_t pinned;
    OS_object_token_t created;
    bool              has_created;
} OS_Zephyr_socket_waiter_t;

/* Runs in OS_TaskDelete after it aborts a socket waiter between slices. */
static void OS_Zephyr_SocketWaitAborted(void *arg)
{
    OS_Zephyr_socket_waiter_t *waiter = arg;

    if (waiter->has_created)
    {
        (void)OS_ObjectIdFinalizeNew(OS_ERROR, &waiter->created, NULL);
    }
    OS_ObjectIdRelease(&waiter->pinned);
}

int OS_Zephyr_SocketWait(const OS_object_token_t *token, const OS_object_token_t *created, short events,
                         int64_t deadline)
{
    OS_impl_file_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_filehandle_table, *token);
    OS_Zephyr_socket_waiter_t       waiter;
    struct zsock_pollfd             pfd;
    struct k_mutex                 *lock;
    int                             rc;

    waiter.pinned      = *token;
    waiter.has_created = created != NULL;
    if (created != NULL)
    {
        waiter.created = *created;
    }
    pfd.fd     = impl->sock;
    pfd.events = events;

    lock = OS_Zephyr_TaskSliceBegin(OS_Zephyr_SocketWaitAborted, &waiter);
    for (;;)
    {
        pfd.revents = 0;
        rc          = zsock_poll(&pfd, 1, OS_Zephyr_SocketSliceMs(deadline));
        if (rc != 0 || (deadline != INT64_MAX && k_uptime_ticks() >= deadline))
        {
            break;
        }
        OS_Zephyr_TaskSlicePause(lock);
    }
    OS_Zephyr_TaskSliceEnd(lock);

    if (rc < 0)
    {
        return -1;
    }
    return rc == 0 ? 0 : pfd.revents;
}

/*----------------------------------------------------------------
 * Generic stream operations on sockets, see os-impl-files.c
 *-----------------------------------------------------------------*/

int32 OS_Zephyr_SocketClose(const OS_object_token_t *token)
{
    OS_impl_file_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_filehandle_table, *token);

    OS_Zephyr_TaskEnter();
    /* As with POSIX close(), the descriptor is gone even on failure. */
    (void)zsock_close(impl->sock);
    impl->sock      = -1;
    impl->is_socket = false;

    return OS_Zephyr_TaskLeaveResult(OS_SUCCESS);
}

int32 OS_Zephyr_SocketRead(const OS_object_token_t *token, void *buffer, size_t nbytes, OS_time_t abs_timeout)
{
    OS_impl_file_internal_record_t *impl     = OS_OBJECT_TABLE_GET(OS_impl_filehandle_table, *token);
    int64_t                         deadline = OS_Zephyr_SocketDeadline(abs_timeout);
    ssize_t                         count;
    int                             revents;

    OS_Zephyr_TaskEnter();
    for (;;)
    {
        count = zsock_recv(impl->sock, buffer, nbytes, ZSOCK_MSG_DONTWAIT);
        if (count >= 0)
        {
            return OS_Zephyr_TaskLeaveResult((int32)count);
        }
        if (!OS_Zephyr_SocketWouldBlock())
        {
            return OS_Zephyr_TaskLeaveResult(OS_ERROR);
        }
        revents = OS_Zephyr_SocketWait(token, NULL, ZSOCK_POLLIN, deadline);
        if (revents <= 0)
        {
            return OS_Zephyr_TaskLeaveResult(revents == 0 ? OS_ERROR_TIMEOUT : OS_ERROR);
        }
    }
}

int32 OS_Zephyr_SocketWrite(const OS_object_token_t *token, const void *buffer, size_t nbytes, OS_time_t abs_timeout)
{
    OS_impl_file_internal_record_t *impl     = OS_OBJECT_TABLE_GET(OS_impl_filehandle_table, *token);
    int64_t                         deadline = OS_Zephyr_SocketDeadline(abs_timeout);
    ssize_t                         count;
    int                             revents;

    OS_Zephyr_TaskEnter();
    for (;;)
    {
        count = zsock_send(impl->sock, buffer, nbytes, ZSOCK_MSG_DONTWAIT);
        if (count >= 0)
        {
            return OS_Zephyr_TaskLeaveResult((int32)count);
        }
        if (!OS_Zephyr_SocketWouldBlock())
        {
            return OS_Zephyr_TaskLeaveResult(OS_ERROR);
        }
        revents = OS_Zephyr_SocketWait(token, NULL, ZSOCK_POLLOUT, deadline);
        if (revents <= 0)
        {
            return OS_Zephyr_TaskLeaveResult(revents == 0 ? OS_ERROR_TIMEOUT : OS_ERROR);
        }
    }
}

/*----------------------------------------------------------------
 * Sockets API
 *-----------------------------------------------------------------*/

int32 OS_SocketOpen_Impl(const OS_object_token_t *token)
{
    OS_impl_file_internal_record_t *impl   = OS_OBJECT_TABLE_GET(OS_impl_filehandle_table, *token);
    OS_stream_internal_record_t    *stream = OS_OBJECT_TABLE_GET(OS_stream_table, *token);
    net_sa_family_t                 family;
    int                             type;
    int                             proto;
    int                             one = 1;
    int                             sock;

    switch (stream->socket_type)
    {
        case OS_SocketType_DATAGRAM:
            type  = NET_SOCK_DGRAM;
            proto = NET_IPPROTO_UDP;
            break;
        case OS_SocketType_STREAM:
            type  = NET_SOCK_STREAM;
            proto = NET_IPPROTO_TCP;
            break;
        default:
            return OS_ERR_NOT_IMPLEMENTED;
    }

    switch (stream->socket_domain)
    {
        case OS_SocketDomain_INET:
            family = NET_AF_INET;
            break;
        case OS_SocketDomain_INET6:
            family = NET_AF_INET6;
            break;
        default:
            return OS_ERR_NOT_IMPLEMENTED;
    }
    if (OS_Zephyr_SockAddrLen(family) == 0)
    {
        return OS_ERR_NOT_IMPLEMENTED;
    }

    OS_Zephyr_TaskEnter();

    sock = zsock_socket(family, type, proto);
    if (sock < 0)
    {
        return OS_Zephyr_TaskLeaveResult(OS_ERROR);
    }
    if (OS_Zephyr_SocketSetNonBlock(sock) != OS_SUCCESS)
    {
        (void)zsock_close(sock);
        return OS_Zephyr_TaskLeaveResult(OS_ERROR);
    }

    /* Eases quick restarts, as on POSIX. Without NET_CONTEXT_REUSEADDR this
     * fails, which is not worth failing the open over. */
    (void)zsock_setsockopt(sock, ZSOCK_SOL_SOCKET, ZSOCK_SO_REUSEADDR, &one, sizeof(one));

    impl->sock      = sock;
    impl->is_socket = true;

    return OS_Zephyr_TaskLeaveResult(OS_SUCCESS);
}

int32 OS_SocketBindAddress_Impl(const OS_object_token_t *token, const OS_SockAddr_t *Addr)
{
    OS_impl_file_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_filehandle_table, *token);
    net_socklen_t                   addrlen;
    int                             rc;

    addrlen = OS_Zephyr_SockAddrLen(OS_Zephyr_ConstSockAddrIn(Addr)->sin_family);
    if (addrlen == 0)
    {
        return OS_ERR_BAD_ADDRESS;
    }

    OS_Zephyr_TaskEnter();
    rc = zsock_bind(impl->sock, (const struct net_sockaddr *)&Addr->AddrData, addrlen);

    return OS_Zephyr_TaskLeaveResult(rc < 0 ? OS_ERROR : OS_SUCCESS);
}

int32 OS_SocketListen_Impl(const OS_object_token_t *token)
{
    OS_impl_file_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_filehandle_table, *token);
    int                             rc;

    OS_Zephyr_TaskEnter();
    rc = zsock_listen(impl->sock, 10);

    return OS_Zephyr_TaskLeaveResult(rc < 0 ? OS_ERROR : OS_SUCCESS);
}

int32 OS_SocketConnect_Impl(const OS_object_token_t *token, const OS_SockAddr_t *Addr, OS_time_t abs_timeout)
{
    OS_impl_file_internal_record_t *impl     = OS_OBJECT_TABLE_GET(OS_impl_filehandle_table, *token);
    int64_t                         deadline = OS_Zephyr_SocketDeadline(abs_timeout);
    net_socklen_t                   addrlen;
    net_socklen_t                   optlen;
    int                             sockerr;
    int                             revents;

    addrlen = OS_Zephyr_SockAddrLen(OS_Zephyr_ConstSockAddrIn(Addr)->sin_family);
    if (addrlen == 0 || addrlen != Addr->ActualLength)
    {
        return OS_ERR_BAD_ADDRESS;
    }

    OS_Zephyr_TaskEnter();

    if (zsock_connect(impl->sock, (const struct net_sockaddr *)&Addr->AddrData, addrlen) == 0)
    {
        return OS_Zephyr_TaskLeaveResult(OS_SUCCESS);
    }
    if (errno != EINPROGRESS)
    {
        return OS_Zephyr_TaskLeaveResult(OS_ERROR);
    }

    /* The application, not the stack, bounds the connection time, as on
     * POSIX. A timed-out socket keeps connecting in the background. */
    revents = OS_Zephyr_SocketWait(token, NULL, ZSOCK_POLLOUT, deadline);
    if (revents <= 0)
    {
        return OS_Zephyr_TaskLeaveResult(revents == 0 ? OS_ERROR_TIMEOUT : OS_ERROR);
    }

    sockerr = 0;
    optlen  = sizeof(sockerr);
    if (zsock_getsockopt(impl->sock, ZSOCK_SOL_SOCKET, ZSOCK_SO_ERROR, &sockerr, &optlen) < 0 || sockerr != 0)
    {
        return OS_Zephyr_TaskLeaveResult(OS_ERROR);
    }

    return OS_Zephyr_TaskLeaveResult(OS_SUCCESS);
}

int32 OS_SocketShutdown_Impl(const OS_object_token_t *token, OS_SocketShutdownMode_t Mode)
{
    OS_impl_file_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_filehandle_table, *token);
    int                             how;
    int                             rc;

    /* The shared layer has validated Mode. */
    if (Mode == OS_SocketShutdownMode_SHUT_READ)
    {
        how = ZSOCK_SHUT_RD;
    }
    else if (Mode == OS_SocketShutdownMode_SHUT_WRITE)
    {
        how = ZSOCK_SHUT_WR;
    }
    else
    {
        how = ZSOCK_SHUT_RDWR;
    }

    OS_Zephyr_TaskEnter();
    rc = zsock_shutdown(impl->sock, how);

    /* The native stack cannot close only the sending side of a connection
     * and rejects write shutdowns. */
    if (rc < 0)
    {
        return OS_Zephyr_TaskLeaveResult(errno == ENOTSUP ? OS_ERR_OPERATION_NOT_SUPPORTED : OS_ERROR);
    }
    return OS_Zephyr_TaskLeaveResult(OS_SUCCESS);
}

int32 OS_SocketAccept_Impl(const OS_object_token_t *sock_token,
                           const OS_object_token_t *conn_token,
                           OS_SockAddr_t           *Addr,
                           OS_time_t                abs_timeout)
{
    OS_impl_file_internal_record_t *sock_impl = OS_OBJECT_TABLE_GET(OS_impl_filehandle_table, *sock_token);
    OS_impl_file_internal_record_t *conn_impl = OS_OBJECT_TABLE_GET(OS_impl_filehandle_table, *conn_token);
    int64_t                         deadline  = OS_Zephyr_SocketDeadline(abs_timeout);
    net_socklen_t                   addrlen;
    int                             revents;
    int                             sock;

    OS_Zephyr_TaskEnter();
    for (;;)
    {
        addrlen = Addr->ActualLength;
        sock    = zsock_accept(sock_impl->sock, (struct net_sockaddr *)&Addr->AddrData, &addrlen);
        if (sock >= 0)
        {
            break;
        }
        /* Unlike most stacks, Zephyr discards a connection that the peer
         * closed before it was accepted, even with data pending, and reports
         * ECONNABORTED. Wait for the next connection instead. */
        if (!OS_Zephyr_SocketWouldBlock() && errno != ECONNABORTED)
        {
            return OS_Zephyr_TaskLeaveResult(OS_ERROR);
        }
        revents = OS_Zephyr_SocketWait(sock_token, conn_token, ZSOCK_POLLIN, deadline);
        if (revents <= 0)
        {
            return OS_Zephyr_TaskLeaveResult(revents == 0 ? OS_ERROR_TIMEOUT : OS_ERROR);
        }
    }

    if (OS_Zephyr_SocketSetNonBlock(sock) != OS_SUCCESS)
    {
        (void)zsock_close(sock);
        return OS_Zephyr_TaskLeaveResult(OS_ERROR);
    }

    Addr->ActualLength  = addrlen;
    conn_impl->sock      = sock;
    conn_impl->is_socket = true;

    return OS_Zephyr_TaskLeaveResult(OS_SUCCESS);
}

int32 OS_SocketRecvFrom_Impl(const OS_object_token_t *token,
                             void                    *buffer,
                             size_t                   buflen,
                             OS_SockAddr_t           *RemoteAddr,
                             OS_time_t                abs_timeout)
{
    OS_impl_file_internal_record_t *impl     = OS_OBJECT_TABLE_GET(OS_impl_filehandle_table, *token);
    int64_t                         deadline = OS_Zephyr_SocketDeadline(abs_timeout);
    struct net_sockaddr            *sa       = NULL;
    net_socklen_t                   addrlen  = 0;
    ssize_t                         count;
    int                             revents;

    OS_Zephyr_TaskEnter();
    for (;;)
    {
        if (RemoteAddr != NULL)
        {
            sa      = (struct net_sockaddr *)&RemoteAddr->AddrData;
            addrlen = sizeof(RemoteAddr->AddrData);
        }
        count = zsock_recvfrom(impl->sock, buffer, buflen, ZSOCK_MSG_DONTWAIT, sa, sa != NULL ? &addrlen : NULL);
        if (count >= 0)
        {
            break;
        }
        if (!OS_Zephyr_SocketWouldBlock())
        {
            return OS_Zephyr_TaskLeaveResult(OS_ERROR);
        }
        revents = OS_Zephyr_SocketWait(token, NULL, ZSOCK_POLLIN, deadline);
        if (revents <= 0)
        {
            return OS_Zephyr_TaskLeaveResult(revents == 0 ? OS_ERROR_TIMEOUT : OS_ERROR);
        }
    }

    if (RemoteAddr != NULL)
    {
        RemoteAddr->ActualLength = addrlen;
    }
    return OS_Zephyr_TaskLeaveResult((int32)count);
}

int32 OS_SocketSendTo_Impl(const OS_object_token_t *token,
                           const void              *buffer,
                           size_t                   buflen,
                           const OS_SockAddr_t     *RemoteAddr)
{
    OS_impl_file_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_filehandle_table, *token);
    net_socklen_t                   addrlen;
    ssize_t                         count;

    addrlen = OS_Zephyr_SockAddrLen(OS_Zephyr_ConstSockAddrIn(RemoteAddr)->sin_family);
    if (addrlen == 0 || addrlen != RemoteAddr->ActualLength)
    {
        return OS_ERR_BAD_ADDRESS;
    }

    /* Never waits, as on POSIX. */
    OS_Zephyr_TaskEnter();
    count = zsock_sendto(impl->sock, buffer, buflen, ZSOCK_MSG_DONTWAIT,
                         (const struct net_sockaddr *)&RemoteAddr->AddrData, addrlen);

    return OS_Zephyr_TaskLeaveResult(count < 0 ? OS_ERROR : (int32)count);
}

int32 OS_SocketGetInfo_Impl(const OS_object_token_t *token, OS_socket_prop_t *sock_prop)
{
    /* All portable properties are supplied by the shared layer. */
    ARG_UNUSED(token);
    ARG_UNUSED(sock_prop);
    return OS_SUCCESS;
}

/* The DSCP value lives in the upper 6 bits of the IPv4 ToS byte. Zephyr
 * takes the whole byte as a uint8_t and rejects any other length. */
static int32 OS_Zephyr_SocketGetTos(int sock, uint8_t *tos)
{
    net_socklen_t optlen = sizeof(*tos);

    if (zsock_getsockopt(sock, NET_IPPROTO_IP, ZSOCK_IP_TOS, tos, &optlen) < 0)
    {
        return (errno == ENOTSUP || errno == ENOPROTOOPT) ? OS_ERR_OPERATION_NOT_SUPPORTED : OS_ERROR;
    }
    return OS_SUCCESS;
}

int32 OS_SocketGetOption_Impl(const OS_object_token_t *token, OS_socket_option_t opt_id, OS_socket_optval_t *optval)
{
    OS_impl_file_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_filehandle_table, *token);
    uint8_t                         tos;
    int32                           status;

    switch (opt_id)
    {
        case OS_socket_option_UNDEFINED:
            return OS_SUCCESS;

        case OS_socket_option_IP_DSCP:
            OS_Zephyr_TaskEnter();
            status = OS_Zephyr_SocketGetTos(impl->sock, &tos);
            if (status == OS_SUCCESS)
            {
                optval->IntVal = (tos >> 2) & 0x3F;
            }
            return OS_Zephyr_TaskLeaveResult(status);

        default:
            return OS_ERR_OPERATION_NOT_SUPPORTED;
    }
}

int32 OS_SocketSetOption_Impl(const OS_object_token_t  *token,
                              OS_socket_option_t        opt_id,
                              const OS_socket_optval_t *optval)
{
    OS_impl_file_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_filehandle_table, *token);
    uint8_t                         tos;
    int32                           status;

    switch (opt_id)
    {
        case OS_socket_option_UNDEFINED:
            return OS_SUCCESS;

        case OS_socket_option_IP_DSCP:
            OS_Zephyr_TaskEnter();
            /* Preserve the ECN bits. */
            status = OS_Zephyr_SocketGetTos(impl->sock, &tos);
            if (status == OS_SUCCESS)
            {
                tos = (tos & 0x03) | ((optval->IntVal << 2) & 0xFC);
                if (zsock_setsockopt(impl->sock, NET_IPPROTO_IP, ZSOCK_IP_TOS, &tos, sizeof(tos)) < 0)
                {
                    status = OS_ERROR;
                }
            }
            return OS_Zephyr_TaskLeaveResult(status);

        default:
            return OS_ERR_OPERATION_NOT_SUPPORTED;
    }
}

/*----------------------------------------------------------------
 * Socket addresses. Names are numeric only; DNS is not supported.
 *-----------------------------------------------------------------*/

int32 OS_SocketAddrInit_Impl(OS_SockAddr_t *Addr, OS_SocketDomain_t Domain)
{
    net_sa_family_t family;
    net_socklen_t   addrlen;

    switch (Domain)
    {
        case OS_SocketDomain_INET:
            family = NET_AF_INET;
            break;
        case OS_SocketDomain_INET6:
            family = NET_AF_INET6;
            break;
        default:
            return OS_ERR_NOT_IMPLEMENTED;
    }

    addrlen = OS_Zephyr_SockAddrLen(family);
    if (addrlen == 0)
    {
        return OS_ERR_NOT_IMPLEMENTED;
    }

    memset(Addr, 0, sizeof(*Addr));
    Addr->ActualLength                     = addrlen;
    OS_Zephyr_SockAddrIn(Addr)->sin_family = family;

    return OS_SUCCESS;
}

int32 OS_SocketAddrToString_Impl(char *buffer, size_t buflen, const OS_SockAddr_t *Addr)
{
    const void *ip = OS_Zephyr_SockAddrIp((OS_SockAddr_t *)Addr);

    if (ip == NULL)
    {
        return OS_ERR_BAD_ADDRESS;
    }
    if (zsock_inet_ntop(OS_Zephyr_ConstSockAddrIn(Addr)->sin_family, ip, buffer, buflen) == NULL)
    {
        return OS_ERROR;
    }
    return OS_SUCCESS;
}

int32 OS_SocketAddrFromString_Impl(OS_SockAddr_t *Addr, const char *string)
{
    void *ip = OS_Zephyr_SockAddrIp(Addr);

    if (ip == NULL)
    {
        return OS_ERR_BAD_ADDRESS;
    }
    if (zsock_inet_pton(OS_Zephyr_SockAddrIn(Addr)->sin_family, string, ip) != 1)
    {
        return OS_ERROR;
    }
    return OS_SUCCESS;
}

int32 OS_SocketAddrGetPort_Impl(uint16 *PortNum, const OS_SockAddr_t *Addr)
{
    if (OS_Zephyr_SockAddrLen(OS_Zephyr_ConstSockAddrIn(Addr)->sin_family) == 0)
    {
        return OS_ERR_BAD_ADDRESS;
    }
    *PortNum = net_ntohs(OS_Zephyr_ConstSockAddrIn(Addr)->sin_port);
    return OS_SUCCESS;
}

int32 OS_SocketAddrSetPort_Impl(OS_SockAddr_t *Addr, uint16 PortNum)
{
    if (OS_Zephyr_SockAddrLen(OS_Zephyr_SockAddrIn(Addr)->sin_family) == 0)
    {
        return OS_ERR_BAD_ADDRESS;
    }
    OS_Zephyr_SockAddrIn(Addr)->sin_port = net_htons(PortNum);
    return OS_SUCCESS;
}
