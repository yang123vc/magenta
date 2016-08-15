#define _GNU_SOURCE

#include "futex_impl.h"
#include "pthread_impl.h"

#include <magenta/syscalls.h>
#include <pthread.h>
#include <runtime/thread.h>
#include <runtime/tls.h>
#include <stddef.h>
#include <string.h>

/* pthread_key_create.c overrides this */
static volatile size_t dummy = 0;
weak_alias(dummy, __pthread_tsd_size);

static void dummy_0(void) {}
weak_alias(dummy_0, __acquire_ptc);
weak_alias(dummy_0, __dl_thread_cleanup);
weak_alias(dummy_0, __do_orphaned_stdio_locks);
weak_alias(dummy_0, __pthread_tsd_run_dtors);
weak_alias(dummy_0, __release_ptc);

static intptr_t start_pthread(void* arg) {
    struct pthread* self = arg;
    mxr_tls_set(__pthread_key, self);
    pthread_exit(self->start(self->start_arg));
}

static intptr_t start_c11(void* arg) {
    struct pthread* self = arg;
    mxr_tls_set(__pthread_key, self);
    int (*start)(void*) = (int (*)(void*))self->start;
    pthread_exit((void*)(uintptr_t)start(self->start_arg));
}

int pthread_create(pthread_t* restrict res, const pthread_attr_t* restrict attrp,
                   void* (*entry)(void*), void* restrict arg) {
    pthread_attr_t attr = {0};
    if (attrp)
        attr = *attrp;

    const char* name = attr.__name ? attr.__name : "";
    mxr_thread_entry_t start = attr.__c11 ? start_c11 : start_pthread;

    struct pthread* self = malloc(sizeof(*self));
    if (self == NULL) {
        return ENOMEM;
    }
    self->start = entry;
    self->start_arg = arg;

    mx_status_t status = mxr_thread_create(start, self, name, &self->mxr_thread);
    switch (status) {
    case NO_ERROR:
        *res = self;
        return 0;
    case ERR_ACCESS_DENIED:
        return EPERM;
    default:
        return EAGAIN;
    }
}

_Noreturn void pthread_exit(void* result) {
    pthread_t self = __pthread_self();
    // TODO(kulakowski) Signals?
    // sigset_t set;

    self->canceldisable = 1;
    self->cancelasync = 0;
    self->result = result;

    // TODO(kulakowski) pthread_cancel?
    // while (self->cancelbuf) {
    //     void (*f)(void*) = self->cancelbuf->__f;
    //     void* x = self->cancelbuf->__x;
    //     self->cancelbuf = self->cancelbuf->__next;
    //     f(x);
    // }

    // TODO(kulakowski) ELF TLS
    // __pthread_tsd_run_dtors();

    mtx_lock(&self->exitlock);

    /* Mark this thread dead before decrementing count */
    mtx_lock(&self->killlock);
    self->dead = 1;

    /* Block all signals before decrementing the live thread count.
     * This is important to ensure that dynamically allocated TLS
     * is not under-allocated/over-committed, and possibly for other
     * reasons as well. */
    // TODO(kulakowski) Signals?
    // __block_all_sigs(&set);

    /* Wait to unlock the kill lock, which governs functions like
     * pthread_kill which target a thread id, until signals have
     * been blocked. This precludes observation of the thread id
     * as a live thread (with application code running in it) after
     * the thread was reported dead by ESRCH being returned. */
    mtx_unlock(&self->killlock);

    /* TODO(kulakowski) Thread exit process teardown. */
    /* It's impossible to determine whether this is "the last thread"
     * until performing the atomic decrement, since multiple threads
     * could exit at the same time. For the last thread, revert the
     * decrement and unblock signals to give the atexit handlers and
     * stdio cleanup code a consistent state. */
    /* if (a_fetch_add(&libc.threads_minus_1, -1) == 0) { */
    /*     libc.threads_minus_1 = 0; */
    /*     __restore_sigs(&set); */
    /*     exit(0); */
    /* } */

    /* TODO(kulakowski): Pthread robust mutex processing used to occur
     * inside this vm lock/unlock pair. I don't if there is also
     * implicitly a need to synchronize on this lock in this function
     * in any case, so I'm leaving the lock/unlock pair.
     */
    __vm_lock();
    __vm_unlock();

    __do_orphaned_stdio_locks();
    __dl_thread_cleanup();

    if (self->detached && self->map_base) {
        /* Detached threads must avoid the kernel clear_child_tid
         * feature, since the virtual address will have been
         * unmapped and possibly already reused by a new mapping
         * at the time the kernel would perform the write. In
         * the case of threads that started out detached, the
         * initial clone flags are correct, but if the thread was
         * detached later (== 2), we need to clear it here. */
        if (self->detached == 2)
            __syscall(SYS_set_tid_address, 0);

        /* Since __unmapself bypasses the normal munmap code path,
         * explicitly wait for vmlock holders first. */
        __vm_wait();

        /* The following call unmaps the thread's stack mapping
         * and then exits without touching the stack. */
        __unmapself(self->map_base, self->map_size);
    }

    mxr_thread_exit(0);
}

void __do_cleanup_push(struct __ptcb* cb) {
    struct pthread* self = __pthread_self();
    cb->__next = self->cancelbuf;
    self->cancelbuf = cb;
}

void __do_cleanup_pop(struct __ptcb* cb) {
    __pthread_self()->cancelbuf = cb->__next;
}

/* pthread_key_create.c overrides this */
static void* dummy_tsd[1] = {0};
weak_alias(dummy_tsd, __pthread_tsd_main);

static FILE* volatile dummy_file = 0;
weak_alias(dummy_file, __stdin_used);
weak_alias(dummy_file, __stdout_used);
weak_alias(dummy_file, __stderr_used);
