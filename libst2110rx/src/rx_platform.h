#ifndef RX_PLATFORM_H
#define RX_PLATFORM_H

#include <stdint.h>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

typedef SOCKET rx_socket_t;
typedef HANDLE rx_thread_t;
#define RX_INVALID_SOCKET INVALID_SOCKET

static inline int rx_socket_init(void)
{
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa);
}

static inline void rx_socket_cleanup(void)
{
    WSACleanup();
}

static inline int rx_socket_close(rx_socket_t s)
{
    return closesocket(s);
}

static inline int rx_socket_error(void)
{
    return WSAGetLastError();
}

typedef volatile long long rx_atomic64_t;

#define rx_atomic64_load(p) ((int64_t)InterlockedCompareExchange64((volatile LONG64 *)(p), 0, 0))
#define rx_atomic64_add(p, v) ((int64_t)InterlockedExchangeAdd64((volatile LONG64 *)(p), (LONG64)(v)))
#define rx_atomic64_store(p, v) ((void)InterlockedExchange64((volatile LONG64 *)(p), (LONG64)(v)))

typedef unsigned(__stdcall *rx_thread_func)(void *);

static inline int rx_thread_create(rx_thread_t *t, rx_thread_func func, void *arg)
{
    *t = (HANDLE)_beginthreadex(NULL, 0, func, arg, 0, NULL);
    return (*t == NULL) ? -1 : 0;
}

static inline void rx_thread_join(rx_thread_t t)
{
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

static inline void rx_thread_set_high_priority(void)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

#else

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <unistd.h>

typedef int rx_socket_t;
typedef pthread_t rx_thread_t;
#define RX_INVALID_SOCKET (-1)

static inline int rx_socket_init(void)
{
    return 0;
}

static inline void rx_socket_cleanup(void)
{
}

static inline int rx_socket_close(rx_socket_t s)
{
    return close(s);
}

static inline int rx_socket_error(void)
{
    return errno;
}

typedef _Atomic int64_t rx_atomic64_t;

#define rx_atomic64_load(p) atomic_load(p)
#define rx_atomic64_add(p, v) atomic_fetch_add((p), (v))
#define rx_atomic64_store(p, v) atomic_store((p), (v))

typedef void *(*rx_thread_func)(void *);

static inline int rx_thread_create(rx_thread_t *t, rx_thread_func func, void *arg)
{
    return pthread_create(t, NULL, func, arg);
}

static inline void rx_thread_join(rx_thread_t t)
{
    pthread_join(t, NULL);
}

static inline void rx_thread_set_high_priority(void)
{
    struct sched_param sp;
    sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
    (void)pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
}

#endif

#endif
