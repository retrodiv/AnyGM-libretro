/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* gml_thread.h — minimal portable thread/mutex/condvar wrapper.
 * POSIX: pthreads. Windows: KERNEL32 primitives only (CreateThread,
 * CRITICAL_SECTION, CONDITION_VARIABLE) so the self-contained DLL constraint
 * (KERNEL32 + msvcrt) holds. Exactly one .c must place GML_THREAD_BRIDGE_IMPL
 * at file scope (it is empty on POSIX). */
#ifndef GML_THREAD_H
#define GML_THREAD_H

#include <stdlib.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
typedef HANDLE gml_thread_t;
typedef CRITICAL_SECTION gml_mutex_t;
typedef CONDITION_VARIABLE gml_cond_t;
typedef struct { void *(*fn)(void*); void *arg; } GmlThreadBridge;
DWORD WINAPI gml_thread_bridge_(LPVOID p);
static inline void gml_mutex_init(gml_mutex_t *m){ InitializeCriticalSection(m); }
static inline void gml_mutex_destroy(gml_mutex_t *m){ DeleteCriticalSection(m); }
static inline void gml_mutex_lock(gml_mutex_t *m){ EnterCriticalSection(m); }
static inline void gml_mutex_unlock(gml_mutex_t *m){ LeaveCriticalSection(m); }
static inline void gml_cond_init(gml_cond_t *c){ InitializeConditionVariable(c); }
static inline void gml_cond_destroy(gml_cond_t *c){ (void)c; }
static inline void gml_cond_wait(gml_cond_t *c, gml_mutex_t *m){ SleepConditionVariableCS(c,m,INFINITE); }
static inline void gml_cond_broadcast(gml_cond_t *c){ WakeAllConditionVariable(c); }
static inline int gml_thread_create(gml_thread_t *t, void *(*fn)(void*), void *arg){
  GmlThreadBridge *b=(GmlThreadBridge*)malloc(sizeof *b); if(!b) return -1;
  b->fn=fn; b->arg=arg;
  *t=CreateThread(NULL,0,gml_thread_bridge_,b,0,NULL);
  if(!*t){ free(b); return -1; }
  return 0;
}
static inline void gml_thread_join(gml_thread_t t){ WaitForSingleObject(t,INFINITE); CloseHandle(t); }
static inline int gml_ncpu(void){ SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }
#define GML_THREAD_BRIDGE_IMPL \
  DWORD WINAPI gml_thread_bridge_(LPVOID p){ \
    GmlThreadBridge *b=(GmlThreadBridge*)p; \
    void *(*fn)(void*)=b->fn; void *arg=b->arg; free(b); fn(arg); return 0; }
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_t gml_thread_t;
typedef pthread_mutex_t gml_mutex_t;
typedef pthread_cond_t gml_cond_t;
static inline void gml_mutex_init(gml_mutex_t *m){ pthread_mutex_init(m,NULL); }
static inline void gml_mutex_destroy(gml_mutex_t *m){ pthread_mutex_destroy(m); }
static inline void gml_mutex_lock(gml_mutex_t *m){ pthread_mutex_lock(m); }
static inline void gml_mutex_unlock(gml_mutex_t *m){ pthread_mutex_unlock(m); }
static inline void gml_cond_init(gml_cond_t *c){ pthread_cond_init(c,NULL); }
static inline void gml_cond_destroy(gml_cond_t *c){ pthread_cond_destroy(c); }
static inline void gml_cond_wait(gml_cond_t *c, gml_mutex_t *m){ pthread_cond_wait(c,m); }
static inline void gml_cond_broadcast(gml_cond_t *c){ pthread_cond_broadcast(c); }
static inline int gml_thread_create(gml_thread_t *t, void *(*fn)(void*), void *arg){
  return pthread_create(t,NULL,fn,arg);
}
static inline void gml_thread_join(gml_thread_t t){ pthread_join(t,NULL); }
static inline int gml_ncpu(void){
  long n=sysconf(_SC_NPROCESSORS_ONLN);
  return n>0 ? (int)n : 1;
}
#define GML_THREAD_BRIDGE_IMPL
#endif

#endif
